// Copyright 2008-2018 Yolo Technologies, Inc. All Rights Reserved. https://www.comblockengine.com


#include "cluster.h"
#include "cluster_interface.h"
#include "cluster_core.h"
#include "network/common.h"
#include "network/tcp_packet.h"
#include "network/message_handler.h"
#include "network/address.h"
#include "helper/sys_info.h"
#include "thread/threadpool.h"
#include "server/component_active_report_handler.h"

namespace KBEngine{

ServerConfig g_serverConfig;
KBE_SINGLETON_INIT(Cluster);

//-------------------------------------------------------------------------------------
Cluster::Cluster(Network::EventDispatcher& dispatcher, 
				 Network::NetworkInterface& ninterface, 
				 COMPONENT_TYPE componentType,
				 COMPONENT_ID componentID):
	ServerApp(dispatcher, ninterface, componentType, componentID),
	pCore_(nullptr)
{
	INFO_MSG(fmt::format("Cluster::Cluster(): cluster component created.\n"));
}

//-------------------------------------------------------------------------------------
Cluster::~Cluster()
{
	finalise();
}

//-------------------------------------------------------------------------------------
bool Cluster::run()
{
	INFO_MSG(fmt::format("Cluster::run(): entering main loop.\n"));
	return ServerApp::run();
}

//-------------------------------------------------------------------------------------
bool Cluster::installSignals()
{
	return ServerApp::installSignals();
}

//-------------------------------------------------------------------------------------
void Cluster::onSignalled(int sigNum)
{
	ServerApp::onSignalled(sigNum);
}

//-------------------------------------------------------------------------------------
bool Cluster::initializeBegin()
{
	INFO_MSG(fmt::format("Cluster::initializeBegin()\n"));

	ENGINE_COMPONENT_INFO& info = g_kbeSrvConfig.getKCluster();
	INFO_MSG(fmt::format("Cluster::initializeBegin(): servicePort={}, peerPort={}, members={}\n",
		info.clusterServicePort, info.clusterPeerPort, info.cluster_addresses.size()));

	return ServerApp::initializeBegin();
}
//-------------------------------------------------------------------------------------
bool Cluster::inInitialize()
{
	// engine 引导所需基座初始化
	return ServerApp::inInitialize();
}

//-------------------------------------------------------------------------------------
bool Cluster::initializeEnd()
{
	INFO_MSG(fmt::format("Cluster::initializeEnd()\n"));

	if(!startServiceListeners())
	{
		ERROR_MSG(fmt::format("Cluster::initializeEnd(): failed to start service listeners!\n"));
		return false;
	}

	// 每帧推进一致性核心/命令队列/TTL 清理
	this->gameTimer_ = this->dispatcher().addTimer(100, this,
		reinterpret_cast<void*>(TIMEOUT_GAME_TICK));

	return ServerApp::initializeEnd();
}

//-------------------------------------------------------------------------------------
void Cluster::finalise()
{
	if(gameTimer_.isSet())
	{
		gameTimer_.cancel();
		gameTimer_.clearWithoutCancel();
	}

	if(pCore_)
	{
		INFO_MSG(fmt::format("Cluster::finalise(): stopping consensus core...\n"));
		pCore_->stop();
		pCore_.reset();
	}

	ServerApp::finalise();
}

//-------------------------------------------------------------------------------------
bool Cluster::startServiceListeners()
{
	ENGINE_COMPONENT_INFO& info = g_kbeSrvConfig.getKCluster();

	// 本机引擎绑定 IP(网络字节序 -> 点分十进制字符串)
	char selfIP[64] = { 0 };
	Network::Address::ip2string(this->networkInterface().intTcpAddr().ip, selfIP);

	std::vector<std::string> members = info.cluster_addresses;
	int selfIdx = 0;

	if(members.empty())
	{
		// 未配置副本表：按单成员集群运行(单机演示/开发环境)，以本机IP为唯一成员
		WARNING_MSG(fmt::format("Cluster::startServiceListeners(): <cluster><addresses> is empty, running as single-member cluster on {}.\n",
			selfIP));
		members.push_back(selfIP);
	}
	else
	{
		bool found = false;
		for(size_t i = 0; i < members.size(); ++i)
		{
			if(members[i] == selfIP)
			{
				selfIdx = (int)i;
				found = true;
				break;
			}
		}

		if(!found)
		{
			ERROR_MSG(fmt::format("Cluster::startServiceListeners(): this host IP {} is not in <cluster><addresses>, "
				"please check the cluster member list!\n", selfIP));
			return false;
		}
	}

	pCore_.reset(new ClusterCore(selfIP, selfIdx, members,
		(uint16)info.clusterServicePort, (uint16)info.clusterPeerPort,
		info.clusterElectionTimeoutMS, info.clusterHeartbeatIntervalMS,
		info.clusterComponentHeartbeatInterval, info.clusterComponentLeaseSeconds));

	if(!pCore_->start())
	{
		ERROR_MSG(fmt::format("Cluster::startServiceListeners(): ClusterCore::start() failed!\n"));
		pCore_.reset();
		return false;
	}

	INFO_MSG(fmt::format("Cluster::startServiceListeners(): core started, self={}({}), members={}, servicePort={}, peerPort={}.\n",
		selfIdx, selfIP, members.size(), (int)info.clusterServicePort, (int)info.clusterPeerPort));

	return true;
}

//-------------------------------------------------------------------------------------
void Cluster::handleTimeout(TimerHandle handle, void * arg)
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
void Cluster::handleGameTick()
{
	if(pCore_)
		pCore_->tick();
}

}
