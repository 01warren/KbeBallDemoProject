// Copyright 2008-2018 Yolo Technologies, Inc. All Rights Reserved. https://www.comblockengine.com


#ifndef KBE_SERVER_APP_H
#define KBE_SERVER_APP_H

#include "common/common.h"
#if KBE_PLATFORM == PLATFORM_WIN32
#pragma warning (disable : 4996)
#endif

#include <stdarg.h> 
#include "helper/debug_helper.h"
#include "helper/watcher.h"
#include "helper/profile.h"
#include "helper/profiler.h"
#include "helper/profile_handler.h"
#include "xml/xml.h"	
#include "server/common.h"
#include "server/components.h"
#include "server/router_client.h"
#include "server/serverconfig.h"
#include "server/signal_handler.h"
#include "server/shutdown_handler.h"
#include "common/smartpointer.h"
#include "common/timer.h"
#include "common/singleton.h"
#include "network/interfaces.h"
#include "network/event_dispatcher.h"
#include "network/network_interface.h"
#include "thread/threadpool.h"

	
namespace KBEngine{

namespace Network
{

class Channel;
class Bundle;
}

class Shutdowner;
class ComponentActiveReportHandler;

class ServerApp : 
	public SignalHandler, 
	public TimerHandler, 
	public ShutdownHandler,
	public Network::ChannelTimeOutHandler,
	public Network::ChannelDeregisterHandler,
	public Components::ComponentsNotificationHandler
{
public:
	enum TimeOutType
	{
		TIMEOUT_SERVERAPP_MAX,

		// 各 App 的每帧心跳最终都经 ServerApp::handleTimeout 收口，
		// 这里给它一个统一名字，便于基类识别"这一帧到了"。
		// 派生类可各自再定义同名枚举(值相同)，语义完全一致。
		TIMEOUT_GAME_TICK = TIMEOUT_SERVERAPP_MAX + 1
	};

public:
	ServerApp(Network::EventDispatcher& dispatcher, 
			Network::NetworkInterface& ninterface, 
			COMPONENT_TYPE componentType,
			COMPONENT_ID componentID);

	~ServerApp();

	virtual bool initialize();
	virtual bool initializeBegin() { return true; }
	virtual bool inInitialize(){ return true; }
	virtual bool initializeEnd() {return true; }
	virtual void finalise();
	virtual bool run();
	
	virtual bool initThreadPool();

	virtual bool installSignals();

	virtual bool initializeWatcher();

	virtual bool loadConfig();
	const char* name(){return COMPONENT_NAME_EX(componentType_);}
	
	virtual void handleTimeout(TimerHandle, void * pUser);

	// ------------------------------------------------------------------
	// Router 接入(详见 docs/kbe_router_design.md)
	//
	// 所有业务进程各持一条到 router 的长连接，跨进程 / 跨实体消息统一经 router
	// 投递(星型)，连接复杂度由 O(N^2) 降为 O(N)。
	//
	// 接入点刻意放在 ServerApp 而不是各个 App：一次接入、全部生效。
	// 是否参与由 isRouterParticipant() 白名单决定——router 自身也派生自 ServerApp，
	// 必须排除，否则它会去连自己。
	// ------------------------------------------------------------------

	/** 本进程是否参与 router 路由 */
	bool isRouterParticipant() const;

	/** 启动 router 接入(幂等)。失败时返回 false，本进程保持原有的 Components 直连通路。 */
	bool startRouter();

	/** 停止 router 接入(幂等) */
	void stopRouter();

	/** 每帧派发 router 事件(由 handleTimeout 的 TIMEOUT_GAME_TICK 分支调用) */
	void tickRouter();

	RouterClient* routerClient() { return pRouterClient_; }
	bool routerReady() const;

	// ---- 对本进程所持有 Actor / Service 的薄封装 ----
	// router 未就绪(未接入 / 断线重连中)时返回 false，调用方**无需特殊处理**：
	// 重连成功后 onRouterReady() 会把本进程全部 Actor / Service 重新注册一遍。

	/** 注册一个本进程持有的 Actor。
		targetConnId 非 0 时表示"把该 Actor 绑定到指定连接"——典型场景是
		baseapp 把客户端 proxyId 绑定到该客户端在 router 上的连接(见 docs/kbe_router_design.md §9.2)，
		此时 src(本进程)与目标连接不是同一个 peer。默认 0 = 绑定到本连接。 */
	bool routerRegisterActor(const RouterInterface::MailboxAddress& addr, uint64_t targetConnId = 0);

	/** 注销一个 Actor(实体销毁 / 客户端下线)。
		注意：一条客户端连接在 router 内只应绑定一个 proxyId，
		重新绑定前必须先 unregister，否则路由表会残留旧映射。 */
	bool routerUnregisterActor(const RouterInterface::MailboxAddress& addr);

	/** 实体迁移：挂起。此后投往该 Actor 的 Mail 由 router 缓冲，既不投递也不丢。 */
	bool routerSuspendActor(const RouterInterface::MailboxAddress& addr);

	/** 实体迁移：恢复。router 把路由改指本连接，并按到达顺序冲刷缓冲的 Mail。 */
	bool routerResumeActor(const RouterInterface::MailboxAddress& addr);

	/** 注册业务服务(CallService 的被调用方)。
		内部同时注册 Actor + Service——只注册 Service 的服务能被发现却无法被投递。 */
	bool routerRegisterService(const std::string& name, const RouterInterface::MailboxAddress& addr);

	/** 服务发现(异步)，结果走 onRouterServiceFound / onRouterServiceNotFound */
	bool routerFindService(const std::string& name, uint64_t requestId = 0);

	/** 直接发一条 Mail(src 取本进程组件地址) */
	bool routerSendMail(const RouterInterface::MailboxAddress& dst, const std::string& body);

	/**
		把一条"待发送的 Bundle"作为**组件级裸消息**(RouterMail::BODY_COMPONENT_MESSAGE)
		投递给目标 Actor。

		@param dst     目标组件 Actor(通常由 RouterMail::componentMailbox(componentID, componentType) 构造)
		@param pBundle 已写好 interface 消息的 Bundle
		               (本函数负责 finiMessage -> 拍平 -> 投递 -> 回收，调用方不应再使用 pBundle)
		@return 是否投递成功；router 未就绪 / 目标不存在时返回 false，
		        具体原因另由 onRouterDeliveryFailed 暴露(本函数不重试)。

		这是"组件不再两两直连"之后
		`Components::findComponent(componentType, componentID)->pChannel->send(pBundle)`
		的统一替代：目标尚未注册(组件未连上 / 实体 cell 部分还没建好)时
		由 router 的待定缓冲兜底，不再需要业务侧自己入队。
	*/
	bool sendMailToComponent(const RouterInterface::MailboxAddress& dst, Network::Bundle* pBundle);

	/** 同上，按 (componentID, componentType) 定位目标组件 Actor */
	bool sendMailToComponent(COMPONENT_ID componentID, COMPONENT_TYPE componentType,
		Network::Bundle* pBundle);

protected:
	/**
		子类覆写：router 就绪(含断线重连)后，把本进程当前持有的**全部** Actor / Service 重新注册一遍。

		这不是可选优化：router 的路由表是纯内存软状态，进程重连后表内条目已全部丢失，
		不重建就会导致后续投递全部 TARGET_NOT_FOUND。
	*/
	virtual void onRouterReady() {}

	/** 子类覆写：收到一条投递到本进程的 Mail。基类默认打日志并丢弃。 */
	virtual void onRouterMail(const RouterClient::Mail& mail);

	/** 子类覆写：router 回报投递失败。失败可见是业务侧自建可靠性的前提。 */
	virtual void onRouterDeliveryFailed(const RouterClient::DeliveryFailure& failure);

	/** 子类覆写：服务发现命中(CallService) */
	virtual void onRouterServiceFound(uint64_t requestId,
		const std::vector< RouterInterface::MailboxAddress >& addrs);

	/** 子类覆写：服务发现未命中 */
	virtual void onRouterServiceNotFound(uint64_t requestId);

public:
	GAME_TIME time() const { return g_kbetime; }
	Timers & timers() { return timers_; }
	double gameTimeInSeconds() const;
	void handleTimers();

	thread::ThreadPool& threadPool() { return threadPool_; }

	Network::EventDispatcher & dispatcher()				{ return dispatcher_; }
	Network::NetworkInterface & networkInterface()			{ return networkInterface_; }

	COMPONENT_ID componentID() const	{ return componentID_; }
	COMPONENT_TYPE componentType() const	{ return componentType_; }
		
	virtual void onSignalled(int sigNum);
	virtual void onChannelTimeOut(Network::Channel * pChannel);
	virtual void onChannelDeregister(Network::Channel * pChannel);
	virtual void onAddComponent(const Components::ComponentInfos* pInfos);
	virtual void onRemoveComponent(const Components::ComponentInfos* pInfos);
	virtual void onIdentityillegal(COMPONENT_TYPE componentType, COMPONENT_ID componentID, uint32 pid, const char* pAddr);

	virtual void onShutdownBegin();
	virtual void onShutdown(bool first);
	virtual void onShutdownEnd();

	/** 网络接口
		请求查看watcher
	*/
	void queryWatcher(Network::Channel* pChannel, MemoryStream& s);

	void shutDown(float shutdowntime = -FLT_MAX);

	COMPONENT_ORDER globalOrder() const { return startGlobalOrder_; }
	COMPONENT_ORDER groupOrder() const { return startGroupOrder_; }

	/** 网络接口
		注册一个新激活的baseapp或者cellapp或者dbmgr
		通常是一个新的app被启动了， 它需要向某些组件注册自己。
	*/
	virtual void onRegisterNewApp(Network::Channel* pChannel, 
							int32 uid, 
							std::string& username, 
							COMPONENT_TYPE componentType, COMPONENT_ID componentID, COMPONENT_ORDER globalorderID, COMPONENT_ORDER grouporderID,
							uint32 intaddr, uint16 intport, uint32 extaddr, uint16 extport, std::string& extaddrEx);

	/** 网络接口
		某个app向本app告知处于活动状态。
	*/
	void onAppActiveTick(Network::Channel* pChannel, COMPONENT_TYPE componentType, COMPONENT_ID componentID);
	
	/** 网络接口
		请求断开服务器的连接
	*/
	virtual void reqClose(Network::Channel* pChannel);

	/** 网络接口
		某个app请求查看该app
	*/
	virtual void lookApp(Network::Channel* pChannel);

	/** 网络接口
		请求关闭服务器
	*/
	virtual void reqCloseServer(Network::Channel* pChannel, MemoryStream& s);

	/** 网络接口
		某个app请求查看该app负载状态， 通常是console请求查看
	*/
	virtual void queryLoad(Network::Channel* pChannel);

	/** 网络接口
		请求关闭服务器
	*/
	void reqKillServer(Network::Channel* pChannel, MemoryStream& s);

	/** 网络接口
		客户端与服务端第一次建立交互, 客户端发送自己的版本号与通讯密钥等信息
		给服务端， 服务端返回是否握手成功
	*/
	virtual void hello(Network::Channel* pChannel, MemoryStream& s);
	virtual void onHello(Network::Channel* pChannel, 
		const std::string& verInfo, 
		const std::string& scriptVerInfo, 
		const std::string& encryptedKey);

	// 引擎版本不匹配
	virtual void onVersionNotMatch(Network::Channel* pChannel);

	// 引擎脚本层版本不匹配
	virtual void onScriptVersionNotMatch(Network::Channel* pChannel);

	/** 网络接口
		console请求开始profile
	*/
	void startProfile(Network::Channel* pChannel, KBEngine::MemoryStream& s);
	virtual void startProfile_(Network::Channel* pChannel, std::string profileName, int8 profileType, uint32 timelen);
		
protected:
	COMPONENT_TYPE											componentType_;
	COMPONENT_ID											componentID_;									// 本组件的ID

	Network::EventDispatcher& 								dispatcher_;	
	Network::NetworkInterface&								networkInterface_;
	
	Timers													timers_;

	// app启动顺序， global为全局(如dbmgr，cellapp的顺序)启动顺序， 
	// group为组启动顺序(如:所有baseapp为一组)
	COMPONENT_ORDER											startGlobalOrder_;
	COMPONENT_ORDER											startGroupOrder_;

	Shutdowner*												pShutdowner_;
	ComponentActiveReportHandler*							pActiveTimerHandle_;

	// router 接入层(见 startRouter)。未参与 router 的进程保持 NULL，
	// 此时全引擎走改造前的 Components 直连通路，行为与迁移前完全一致。
	RouterClient*											pRouterClient_;

	// 线程池
	thread::ThreadPool										threadPool_;	
};

}

#endif // KBE_SERVER_APP_H
