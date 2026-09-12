// Copyright 2008-2018 Yolo Technologies, Inc. All Rights Reserved. https://www.comblockengine.com


#include "router.h"
#include "router_interface.h"
#include "router_core.h"
#include "network/common.h"
#include "network/address.h"
#include "helper/sys_info.h"
#include "server/component_active_report_handler.h"

namespace KBEngine{

ServerConfig g_serverConfig;
KBE_SINGLETON_INIT(Router);

//-------------------------------------------------------------------------------------
Router::Router(Network::EventDispatcher& dispatcher, 
			   Network::NetworkInterface& ninterface, 
			   COMPONENT_TYPE componentType,
			   COMPONENT_ID componentID):
	ServerApp(dispatcher, ninterface, componentType, componentID),
	pCore_(nullptr)
{
	INFO_MSG(fmt::format("Router::Router(): router component created.\n"));
}

//-------------------------------------------------------------------------------------
Router::~Router()
{
	finalise();
}

//-------------------------------------------------------------------------------------
bool Router::run()
{
	INFO_MSG(fmt::format("Router::run(): entering main loop.\n"));
	return ServerApp::run();
}

//-------------------------------------------------------------------------------------
bool Router::installSignals()
{
	return ServerApp::installSignals();
}

//-------------------------------------------------------------------------------------
void Router::onSignalled(int sigNum)
{
	ServerApp::onSignalled(sigNum);
}

//-------------------------------------------------------------------------------------
bool Router::initializeBegin()
{
	INFO_MSG(fmt::format("Router::initializeBegin()\n"));

	ENGINE_COMPONENT_INFO& info = g_kbeSrvConfig.getKRouter();
	INFO_MSG(fmt::format("Router::initializeBegin(): clientPort={}, serverPort={}, suspendTimeoutMS={}, actorBufferMax={}, "
		"pendingTimeoutMS={}, pendingBufferMax={}, addresses={}\n",
		info.routerClientPort, info.routerServerPort, info.routerSuspendTimeoutMS,
		info.routerActorBufferMax, info.routerPendingTimeoutMS, info.routerPendingBufferMax,
		info.router_addresses.size()));

	return ServerApp::initializeBegin();
}

//-------------------------------------------------------------------------------------
bool Router::inInitialize()
{
	// engine 引导所需基座初始化
	return ServerApp::inInitialize();
}

//-------------------------------------------------------------------------------------
bool Router::initializeEnd()
{
	INFO_MSG(fmt::format("Router::initializeEnd()\n"));

	if(!startServiceListeners())
	{
		ERROR_MSG(fmt::format("Router::initializeEnd(): failed to start service listeners!\n"));
		return false;
	}

	// 每帧推进：处理网络事件队列 + 挂起超时回滚
	this->gameTimer_ = this->dispatcher().addTimer(100, this,
		reinterpret_cast<void*>(TIMEOUT_GAME_TICK));

	return ServerApp::initializeEnd();
}

//-------------------------------------------------------------------------------------
void Router::finalise()
{
	if(gameTimer_.isSet())
	{
		gameTimer_.cancel();
		gameTimer_.clearWithoutCancel();
	}

	if(pCore_)
	{
		INFO_MSG(fmt::format("Router::finalise(): stopping router core...\n"));
		pCore_->stop();
		pCore_.reset();
	}

	ServerApp::finalise();
}

//-------------------------------------------------------------------------------------
bool Router::startServiceListeners()
{
	ENGINE_COMPONENT_INFO& info = g_kbeSrvConfig.getKRouter();

	// 本机引擎绑定 IP(网络字节序 -> 点分十进制字符串)
	char selfIP[64] = { 0 };
	Network::Address::ip2string(this->networkInterface().intTcpAddr().ip, selfIP);

	if(info.router_addresses.empty())
	{
		// 单机部署：<router><addresses> 留空时以本机 IP 作为唯一 Router 地址
		WARNING_MSG(fmt::format("Router::startServiceListeners(): <router><addresses> is empty, "
			"running as single router on {} (clientPort={}, serverPort={}).\n",
			selfIP, (int)info.routerClientPort, (int)info.routerServerPort));
	}

	pCore_.reset(new RouterCore(selfIP,
		(uint16)info.routerClientPort, (uint16)info.routerServerPort,
		info.routerSuspendTimeoutMS, info.routerActorBufferMax,
		info.routerPendingTimeoutMS, info.routerPendingBufferMax));

	if(!pCore_->start())
	{
		ERROR_MSG(fmt::format("Router::startServiceListeners(): RouterCore::start() failed!\n"));
		pCore_.reset();
		return false;
	}

	INFO_MSG(fmt::format("Router::startServiceListeners(): core started, self={}, clientPort={}, serverPort={}.\n",
		selfIP, (int)info.routerClientPort, (int)info.routerServerPort));

	return true;
}

//-------------------------------------------------------------------------------------
void Router::handleTimeout(TimerHandle handle, void * arg)
{
	switch(reinterpret_cast<uintptr>(arg))
	{
	case TIMEOUT_GAME_TICK:
		this->handleGameTick();
		break;
	case TIMEOUT_CORE_FLUSH:
		break;
	default:
		break;
	}

	ServerApp::handleTimeout(handle, arg);
}

//-------------------------------------------------------------------------------------
void Router::handleGameTick()
{
	if(pCore_)
		pCore_->tick();
}

//-------------------------------------------------------------------------------------
}
