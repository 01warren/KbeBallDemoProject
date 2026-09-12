// Copyright 2008-2018 Yolo Technologies, Inc. All Rights Reserved. https://www.comblockengine.com

/*
	RouterClient —— 所有服务器进程(loginapp / baseapp / cellapp / dbmgr / baseappmgr / cellappmgr ...)
	接入 router 的统一接入层。详见 docs/kbe_router_design.md 第 4 节(连接模型)与第 6 节(路由表/生命周期)。

	设计要点：
	1. 一条 TCP 长连接 + 一个独立网络线程(connect -> HELLO -> 阻塞收帧)，断线自动重连；
	2. 主线程只做三件事：tick() 取事件并回调、调用 sendXxx() 发包、执行业务逻辑。
	   所有发送都在 sendMutex_ 保护下进行；读线程退出时在锁内置 alive_=false 并关闭 ep_，
	   因此不存在"向已关闭 socket 发送"的竞态(与 router_core.cpp 同一约定)；
	3. 不做可靠性：router 只在无法投递时回报 MSG_DELIVERY_FAILED，
	   补发/去重/ACK 由业务侧负责(设计文档 §8 投递语义)。

	线程模型：
	   [网络线程] connect/HELLO/recvFrame -> 解析 -> pushEvent(加锁入队)
	   [主线程]   tick() -> drainEvents() -> 触发 onXxx 回调；发送时 sendFrame() 直接写 socket
*/

#ifndef KBE_ROUTER_CLIENT_H
#define KBE_ROUTER_CLIENT_H

#include "common/common.h"
#include "network/endpoint.h"
#include "server/router_interface.h"

#include <atomic>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace KBEngine{

class RouterClient
{
public:
	/** 收到一条投递到本进程的 Mail(dst 可能是本进程的 Actor，也可能是绑到本连接的客户端 Actor)。 */
	struct Mail
	{
		RouterInterface::MailboxAddress src;
		RouterInterface::MailboxAddress dst;
		uint64_t msgId;
		std::string body;

		Mail() : msgId(0) {}
	};

	/** router 回报的投递失败(设计文档 §8.3：失败可见是业务可靠性的硬前提)。 */
	struct DeliveryFailure
	{
		RouterInterface::MailboxAddress src;
		RouterInterface::MailboxAddress dst;
		uint64_t msgId;
		uint32_t code;		// RouterInterface::DeliveryResult

		DeliveryFailure() : msgId(0), code(0) {}
	};

public:
	RouterClient(COMPONENT_TYPE componentType, COMPONENT_ID componentId);
	~RouterClient();

	/** 启动接入(非阻塞，立即返回)。
		@param addrs        router 地址列表，元素形如 "127.0.0.1" 或 "192.168.1.7:20096"；
		                    为空时使用 127.0.0.1。
		@param defaultPort  地址中未显式给出端口时使用的端口(组件用 serverPort，客户端用 clientPort)。
		@return             false 表示已在运行或参数非法。
	*/
	bool start(const std::vector< std::string >& addrs, uint16 defaultPort);
	void stop();

	/** 是否已与 router 完成 HELLO 握手(可以发包)。 */
	bool ready() const { return ready_.load(); }

	/** router 分配的连接 id(未连接时为 0)。 */
	uint64_t connId() const;

	/** 主循环钩子：由宿主进程每帧(handleGameTick)调用，派发网络线程投递的事件。 */
	void tick();

	// ------------------------------------------------------------------
	// 发送接口(主线程调用)。未 ready 时返回 false 且不发送——
	// 调用方应在 onReadyChanged(true) 回调里把本地 Actor/Service 全部重新注册。
	// ------------------------------------------------------------------

	/** 发送一条 Mail。body 由业务定义，router 原样透传。 */
	bool sendMail(const RouterInterface::MailboxAddress& dst,
		const RouterInterface::MailboxAddress& src,
		uint64_t msgId,
		const std::string& body);

	/** 注册 Actor。targetConnId 非 0 时表示"把该 Actor 绑定到指定连接"
		(典型场景：baseapp 把客户端的 proxyId 绑定到该客户端在 router 上的连接)。 */
	bool registerActor(const RouterInterface::MailboxAddress& addr, uint64_t targetConnId = 0);

	/** 注销 Actor(实体销毁 / 客户端踢下线)。 */
	bool unregisterActor(const RouterInterface::MailboxAddress& addr);

	/** 迁移挂起：此后投往该 Actor 的 Mail 由 router 缓冲，不投递也不丢。 */
	bool suspendActor(const RouterInterface::MailboxAddress& addr);

	/** 迁移恢复：router 把路由改指本连接，并按到达顺序冲刷缓冲的 Mail。 */
	bool resumeActor(const RouterInterface::MailboxAddress& addr);

	/** 注册业务服务。内部会同时发 REGISTER_ACTOR + REGISTER_SERVICE：
	    前者让 MSG_SEND 能按地址路由到服务宿主，后者让 findService 能按名字发现它。 */
	bool registerService(const std::string& name, const RouterInterface::MailboxAddress& addr);

	/** 服务发现(异步)。requestId 由调用方指定用于关联；结果走 onServiceFound/onServiceNotFound。
		说明：router 对 findService 的应答在单连接上按 FIFO 返回，故按请求先后配对。 */
	bool findService(const std::string& name, uint64_t requestId = 0);

	// ------------------------------------------------------------------
	// 回调(业务侧设置；全部在主线程 tick() 中触发，可安全访问引擎主线程状态)
	// ------------------------------------------------------------------
	std::function< void(const Mail&) >					onMail;
	std::function< void(const DeliveryFailure&) >		onDeliveryFailed;
	std::function< void(uint64_t requestId,
		const std::vector< RouterInterface::MailboxAddress >&) > onServiceFound;
	std::function< void(uint64_t requestId) >			onServiceNotFound;

	/** 连接就绪状态变化。
		注意：router 的路由表是可重建的软状态，本进程**重连后必须重新注册**全部 Actor/Service，
		该回调就是做这件事的地方。 */
	std::function< void(bool ready) >					onReadyChanged;

	/** 非致命错误信息(协议错误等)，仅供日志/观测。 */
	std::function< void(const std::string&) >			onError;

private:
	// router 地址
	struct RouterAddr
	{
		std::string ip;
		uint16 port;

		RouterAddr() : port(0) {}
	};

	// 网络线程 -> 主线程 的事件
	struct RouterEvent
	{
		enum Type
		{
			EV_READY,
			EV_DISCONNECTED,
			EV_MAIL,
			EV_DELIVERY_FAILED,
			EV_SERVICE_FOUND,
			EV_SERVICE_NOT_FOUND,
			EV_ERROR
		};

		Type type;

		RouterInterface::MailboxAddress src;
		RouterInterface::MailboxAddress dst;
		uint64_t msgId;
		uint32_t code;
		uint64_t requestId;
		std::vector< RouterInterface::MailboxAddress > addrs;
		std::string body;		// EV_MAIL 的业务体
		std::string text;		// EV_ERROR 的错误描述

		RouterEvent() : type(EV_ERROR), msgId(0), code(0), requestId(0) {}
	};

	// ---- 网络线程 ----
	void netThreadMain();
	Network::EndPoint* doConnect(const RouterAddr& addr);
	bool doHello(Network::EndPoint* ep);
	void handleInboundFrame(const std::string& payload);
	void pushEvent(const RouterEvent& ev);

	// ---- 主线程 ----
	void drainEvents();
	bool sendFrame(const std::string& payload);

	static bool parseRouterAddr(const std::string& s, uint16 defaultPort, RouterAddr& out);

private:
	COMPONENT_TYPE	componentType_;
	COMPONENT_ID	componentId_;

	std::vector< RouterAddr >	endpoints_;

	std::atomic<bool>	running_;
	std::atomic<bool>	ready_;

	std::thread*		thread_;

	// 由网络线程建立/销毁；主线程仅在 sendMutex_ 保护下访问
	Network::EndPoint*	ep_;
	bool				alive_;
	uint64_t			connId_;
	std::mutex			sendMutex_;

	std::mutex					queueMutex_;
	std::deque< RouterEvent >	queue_;

	// findService 的 FIFO 配对(仅主线程访问)
	std::deque< uint64_t >		pendingFindService_;
};

}

#endif // KBE_ROUTER_CLIENT_H
