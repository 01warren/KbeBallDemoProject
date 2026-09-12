// Copyright 2008-2018 Yolo Technologies, Inc. All Rights Reserved. https://www.comblockengine.com

#ifndef KBE_CELLAPP_H
#define KBE_CELLAPP_H

#include "entity.h"
#include "spacememorys.h"
#include "cells.h"
#include "space_viewer.h"
#include "updatables.h"
#include "ghost_manager.h"
#include "witnessed_timeout_handler.h"
#include "server/entity_app.h"
#include "server/forward_messagebuffer.h"
	
namespace KBEngine{

class TelnetServer;
class InitProgressHandler;

class Cellapp:	public EntityApp<Entity>, 
				public Singleton<Cellapp>
{
public:
	enum TimeOutType
	{
		TIMEOUT_LOADING_TICK = TIMEOUT_ENTITYAPP_MAX + 1
	};
	
	Cellapp(Network::EventDispatcher& dispatcher, 
		Network::NetworkInterface& ninterface, 
		COMPONENT_TYPE componentType,
		COMPONENT_ID componentID);

	~Cellapp();

	virtual bool installPyModules();
	virtual void onInstallPyModules();
	virtual bool uninstallPyModules();
	
	bool run();
	
	virtual bool initializeWatcher();

	/**  
		相关处理接口 
	*/
	virtual void handleTimeout(TimerHandle handle, void * arg);
	virtual void handleGameTick();

	/**  
		初始化相关接口 
	*/
	bool initializeBegin();
	bool initializeEnd();
	void finalise();

	virtual ShutdownHandler::CAN_SHUTDOWN_STATE canShutdown();
	virtual void onShutdown(bool first);

	void destroyObjPool();

	float _getLoad() const { return getLoad(); }
	virtual void onUpdateLoad();

	/**  网络接口
		dbmgr告知已经启动的其他baseapp或者cellapp的地址
		当前app需要主动的去与他们建立连接
	*/
	virtual void onGetEntityAppFromDbmgr(Network::Channel* pChannel, 
							int32 uid, 
							std::string& username, 
							COMPONENT_TYPE componentType, COMPONENT_ID componentID, COMPONENT_ORDER globalorderID, COMPONENT_ORDER grouporderID,
							uint32 intaddr, uint16 intport, uint32 extaddr, uint16 extport, std::string& extaddrEx);

	/**
		创建了一个entity回调
	*/
	virtual Entity* onCreateEntity(PyObject* pyEntity, ScriptDefModule* sm, ENTITY_ID eid);

	/**  
		创建一个entity 
	*/
	static PyObject* __py_createEntity(PyObject* self, PyObject* args);

	/** 
		向dbmgr请求执行一个数据库命令
	*/
	static PyObject* __py_executeRawDatabaseCommand(PyObject* self, PyObject* args);
	void executeRawDatabaseCommand(const char* datas, uint32 size, PyObject* pycallback, ENTITY_ID eid, const std::string& dbInterfaceName);
	void onExecuteRawDatabaseCommandCB(Network::Channel* pChannel, KBEngine::MemoryStream& s);

	/** 网络接口
		dbmgr发送初始信息
		startID: 初始分配ENTITY_ID 段起始位置
		endID: 初始分配ENTITY_ID 段结束位置
		startGlobalOrder: 全局启动顺序 包括各种不同组件
		startGroupOrder: 组内启动顺序， 比如在所有baseapp中第几个启动。
		machineGroupOrder: 在machine中真实的组顺序, 提供底层在某些时候判断是否为第一个cellapp时使用
	*/
	void onDbmgrInitCompleted(Network::Channel* pChannel, GAME_TIME gametime, 
		ENTITY_ID startID, ENTITY_ID endID, COMPONENT_ORDER startGlobalOrder, COMPONENT_ORDER startGroupOrder, 
		const std::string& digest);

	/** 网络接口
		dbmgr广播global数据的改变
	*/
	void onBroadcastCellAppDataChanged(Network::Channel* pChannel, KBEngine::MemoryStream& s);

	/** 网络接口
		baseEntity请求创建在一个新的space中
	*/
	void onCreateCellEntityInNewSpaceFromBaseapp(Network::Channel* pChannel, KBEngine::MemoryStream& s);

	/** 网络接口
		baseEntity请求创建在一个新的space中
	*/
	void onRestoreSpaceInCellFromBaseapp(Network::Channel* pChannel, KBEngine::MemoryStream& s);
	
	/** 网络接口
	工具请求改变space查看器（含添加和删除功能）
	如果是请求更新并且服务器上不存在该地址的查看器则自动创建，如果是删除则明确给出删除要求
	*/
	void setSpaceViewer(Network::Channel* pChannel, MemoryStream& s);

	/** 网络接口
		其他APP请求在此灾难恢复
	*/
	void requestRestore(Network::Channel* pChannel, KBEngine::MemoryStream& s);

	/** 网络接口
		baseapp请求在这个cellapp上创建一个entity
	*/
	void onCreateCellEntityFromBaseapp(Network::Channel* pChannel, KBEngine::MemoryStream& s);
	void _onCreateCellEntityFromBaseapp(std::string& entityType, ENTITY_ID createToEntityID, ENTITY_ID entityID, 
		MemoryStream* pCellData, bool hasClient, bool inRescore, COMPONENT_ID componentID, SPACE_ID spaceID);

	/** 网络接口
		销毁某个cellEntity
	*/
	void onDestroyCellEntityFromBaseapp(Network::Channel* pChannel, ENTITY_ID eid);

	/** 网络接口
		entity收到远程call请求, 由某个app上的entitycall发起
	*/
	void onEntityCall(Network::Channel* pChannel, KBEngine::MemoryStream& s);
	
	/** 网络接口
		client访问entity的cell方法由baseapp转发
	*/
	void onRemoteCallMethodFromClient(Network::Channel* pChannel, KBEngine::MemoryStream& s);

	/** 网络接口
		client更新数据
	*/
	void onUpdateDataFromClient(Network::Channel* pChannel, KBEngine::MemoryStream& s);
	void onUpdateDataFromClientForControlledEntity(Network::Channel* pChannel, KBEngine::MemoryStream& s);

	/** 网络接口
		real请求更新属性到ghost
	*/
	void onUpdateGhostPropertys(Network::Channel* pChannel, KBEngine::MemoryStream& s);
	
	/** 网络接口
		ghost请求调用def方法real
	*/
	void onRemoteRealMethodCall(Network::Channel* pChannel, KBEngine::MemoryStream& s);

	/** 网络接口
		real请求更新属性到ghost
	*/
	void onUpdateGhostVolatileData(Network::Channel* pChannel, KBEngine::MemoryStream& s);

	/** 网络接口
		base请求获取celldata
	*/
	void reqBackupEntityCellData(Network::Channel* pChannel, KBEngine::MemoryStream& s);

	/** 网络接口
		base请求获取WriteToDB
	*/
	void reqWriteToDBFromBaseapp(Network::Channel* pChannel, KBEngine::MemoryStream& s);

	/** 网络接口
		客户端直接发送消息给cell实体
	*/
	void forwardEntityMessageToCellappFromClient(Network::Channel* pChannel, MemoryStream& s);

	/** 网络接口
		请求设置flags
	*/
	void reqSetFlags(Network::Channel* pChannel, MemoryStream& s);

	/**
		获取游戏时间
	*/
	static PyObject* __py_gametime(PyObject* self, PyObject* args);

	/**
		添加与删除一个Updatable对象
	*/
	bool addUpdatable(Updatable* pObject);
	bool removeUpdatable(Updatable* pObject);

	/**
		hook entitycallcall
	*/
	RemoteEntityMethod* createEntityCallCallEntityRemoteMethod(MethodDescription* pMethodDescription, EntityCallAbstract* pEntityCall);

	/** 网络接口
		某个app请求查看该app
	*/
	virtual void lookApp(Network::Channel* pChannel);

	/**
		重新导入所有的脚本
	*/
	static PyObject* __py_reloadScript(PyObject* self, PyObject* args);
	virtual void reloadScript(bool fullReload);
	virtual void onReloadScript(bool fullReload);

	/**
		获取进程是否正在关闭中
	*/
	static PyObject* __py_isShuttingDown(PyObject* self, PyObject* args);

	/**
		获取进程内部网络地址
	*/
	static PyObject* __py_address(PyObject* self, PyObject* args);

	WitnessedTimeoutHandler	* pWitnessedTimeoutHandler(){ return pWitnessedTimeoutHandler_; }

	/**
		网络接口
		另一个cellapp的entity要teleport到本cellapp上的space中
	*/
	void reqTeleportToCellApp(Network::Channel* pChannel, MemoryStream& s);
	void reqTeleportToCellAppCB(Network::Channel* pChannel, MemoryStream& s);
	void reqTeleportToCellAppOver(Network::Channel* pChannel, MemoryStream& s);

	// ------------------------------------------------------------------
	// Router 接入(见 ServerApp 与 docs/kbe_router_design.md)
	//
	// cell 实体 Actor 地址只由 entityID 决定：MailboxAddress{MAILBOX_ENTITY,
	// componentType=CELLAPP_TYPE, actorId=entityID}。base 部分(宿主 baseapp)与
	// cell 部分(宿主 cellapp)是同一 entityID 下的两个 Actor，Router 侧靠 componentType 区分。
	//
	// 只有"真实体"所在进程才注册该地址，ghost 不注册——否则路由会在
	// "真实体宿主"与"ghost 宿主"之间来回抖动。迁移期间由 SUSPEND/RESUME
	// 保证消息既不丢也不投错，取代了旧 ghost 转发缓冲。
	// ------------------------------------------------------------------

	/** entity 创建后注册 cell 实体 Actor(仅真实体) */
	virtual void onEntityCreated(Entity* entity);

	/** entity 销毁前注销 cell 实体 Actor */
	virtual void onEntityDestroyed(Entity* entity);

	/** router(重)连成功：重建本进程全部真实体 Actor */
	virtual void onRouterReady();

	/** 收到投递到本进程的 Mail：按 RouterMail::BodyTag 派发到原有 handler */
	virtual void onRouterMail(const RouterClient::Mail& mail);

	/**
		经 Router 向某实体对应的客户端 Actor 投递一段"拍平的 legacy client 消息载荷"。

		body tag 为 RouterMail::BODY_CLIENT_MESSAGE，客户端按原有 MessageHandlers
		逐条解析即可，因此客户端消息层无需为 Router 做解析改动。与 baseapp 的
		Baseapp::sendMailToClientActor 保持同一约定(cellapp 不再直连客户端)。

		仅 Router 模式可用；返回 false 时调用方应回退到原有直连 Channel 通路。
	*/
	bool sendMailToClientActor(ENTITY_ID eid, const std::string& bundleBody);

	/**
		同上，但入参是"经 baseapp 中继的 legacy client 消息"Bundle：
		本函数负责 finiMessage -> 拍平 -> 剥掉 BaseappInterface::
		forwardMessageToClientFromCellapp 这层中继外壳 -> 投递 -> 回收。

		注意 eid 必须与构造 bundle 时 NETWORK_ENTITY_MESSAGE_FORWARD_CLIENT_*
		写入的 entityID 一致(即"该消息要投递到的客户端")，剥壳时会做一致性校验。

		@return true  Router 模式下 pBundle 已被消费(调用方不得再使用)；
		        false 未启用 Router，pBundle 仍归调用方所有(用于回退旧通路)。
	*/
	bool sendBundleToClientActor(ENTITY_ID eid, Network::Bundle* pBundle);

	/**
		获取和设置ghost管理器
	*/
	void pGhostManager(GhostManager* v){ pGhostManager_ = v; }
	GhostManager* pGhostManager() const{ return pGhostManager_; }

	ArraySize spaceSize() const { return (ArraySize)SpaceMemorys::size(); }

	/** 
		射线 
	*/
	int raycast(SPACE_ID spaceID, int layer, const Position3D& start, const Position3D& end, std::vector<Position3D>& hitPos);
	static PyObject* __py_raycast(PyObject* self, PyObject* args);

	uint32 flags() const { return flags_; }
	void flags(uint32 v) { flags_ = v; }
	static PyObject* __py_setFlags(PyObject* self, PyObject* args);
	static PyObject* __py_getFlags(PyObject* self, PyObject* args);

protected:
	// cellAppData
	GlobalDataClient*					pCellAppData_;

	ForwardComponent_MessageBuffer		forward_messagebuffer_;

	Updatables							updatables_;

	// 所有的cell
	Cells								cells_;

	TelnetServer*						pTelnetServer_;

	WitnessedTimeoutHandler	*			pWitnessedTimeoutHandler_;

	GhostManager*						pGhostManager_;
	
	// APP的标志
	uint32								flags_;

	// 通过工具查看space
	SpaceViewers						spaceViewers_;

	InitProgressHandler*				pInitProgressHandler_;
};

}

#endif // KBE_CELLAPP_H
