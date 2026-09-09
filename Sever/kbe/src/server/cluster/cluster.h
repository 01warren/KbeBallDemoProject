// Copyright 2008-2018 Yolo Technologies, Inc. All Rights Reserved. https://www.comblockengine.com

/*
	cluster 集群组件（取代 kbe_Machine 的注册/发现/远程起停）
	见 docs/kbe_cluster_design.md
*/

#ifndef KBE_CLUSTER_H
#define KBE_CLUSTER_H
	
// common include	
#include "server/kbemain.h"
#include "server/serverapp.h"
#include "server/idallocate.h"
#include "server/serverconfig.h"
#include "server/components.h"
#include "common/timer.h"
#include "network/endpoint.h"
#include "network/common.h"

#include <map>
#include <string>
#include <memory>

// windows include	
#if KBE_PLATFORM == PLATFORM_WIN32
#include <winsock2.h>
#else
// linux include
#endif
	
namespace KBEngine{

class ClusterCore;


class Cluster:	public ServerApp, 
				public Singleton<Cluster>
{
public:
	enum TimeOutType
	{
		TIMEOUT_GAME_TICK = TIMEOUT_SERVERAPP_MAX + 1,
		TIMEOUT_CORE_FLUSH
	};
	
	Cluster(Network::EventDispatcher& dispatcher, 
		Network::NetworkInterface& ninterface, 
		COMPONENT_TYPE componentType,
		COMPONENT_ID componentID);

	~Cluster();
	
	bool run();

	void handleTimeout(TimerHandle handle, void * arg);

	/* 初始化相关接口 */
	bool initializeBegin();
	bool inInitialize();
	bool initializeEnd();
	void finalise();

	virtual bool installSignals();
	virtual void onSignalled(int sigNum);

	/** 启动 servicePort / peerPort 两个 TCP 监听(副本线程), 
		由 initializeEnd() 调用，失败返回false
	*/
	bool startServiceListeners();

protected:
	/** 每帧被调度: 推进一致性核心(命令队列刷新/选举/心跳/日志复制/TTL清理)*/
	void handleGameTick();

	TimerHandle gameTimer_;

	// 一致性核心 + 两个TCP监听(线程)，实现在 cluster_core.h/.cpp
	std::unique_ptr<ClusterCore> pCore_;
};

}

#endif // KBE_CLUSTER_H
