// Copyright 2008-2018 Yolo Technologies, Inc. All Rights Reserved. https://www.comblockengine.com

/*
	router 接入/路由中心组件。
	见 docs/kbe_router_design.md
*/

#ifndef KBE_ROUTER_H
#define KBE_ROUTER_H

// common include
#include "server/kbemain.h"
#include "server/serverapp.h"
#include "server/idallocate.h"
#include "server/serverconfig.h"
#include "server/components.h"
#include "common/timer.h"
#include "network/endpoint.h"
#include "network/common.h"

#include <memory>
#include <string>

// windows include
#if KBE_PLATFORM == PLATFORM_WIN32
#include <winsock2.h>
#else
// linux include
#endif

namespace KBEngine{

class RouterCore;

class Router:	public ServerApp,
				public Singleton<Router>
{
public:
	enum TimeOutType
	{
		TIMEOUT_GAME_TICK = TIMEOUT_SERVERAPP_MAX + 1,
		TIMEOUT_CORE_FLUSH
	};

	Router(Network::EventDispatcher& dispatcher,
		Network::NetworkInterface& ninterface,
		COMPONENT_TYPE componentType,
		COMPONENT_ID componentID);

	~Router();

	bool run();

	void handleTimeout(TimerHandle handle, void * arg);

	/* 初始化相关接口 */
	bool initializeBegin();
	bool inInitialize();
	bool initializeEnd();
	void finalise();

	virtual bool installSignals();
	virtual void onSignalled(int sigNum);

	/** 启动 clientPort / serverPort 两个 TCP 监听(接入线程)，
		由 initializeEnd() 调用，失败返回 false
	*/
	bool startServiceListeners();

protected:
	/** 每帧被调度：推进路由中心(事件队列/挂起超时回滚) */
	void handleGameTick();

	TimerHandle gameTimer_;

	// 接入 + 路由核心，实现在 router_core.h/.cpp
	std::unique_ptr<RouterCore> pCore_;
};

}

#endif // KBE_ROUTER_H
