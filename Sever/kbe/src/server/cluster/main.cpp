// Copyright 2008-2018 Yolo Technologies, Inc. All Rights Reserved. https://www.comblockengine.com


#include "common/common.h"
#include "server/kbemain.h"
#include "cluster.h"

#undef DEFINE_IN_INTERFACE
#include "baseappmgr/baseappmgr_interface.h"
#define DEFINE_IN_INTERFACE
#include "baseappmgr/baseappmgr_interface.h"

#undef DEFINE_IN_INTERFACE
#include "cellappmgr/cellappmgr_interface.h"
#define DEFINE_IN_INTERFACE
#include "cellappmgr/cellappmgr_interface.h"

#undef DEFINE_IN_INTERFACE
#include "cellapp/cellapp_interface.h"
#define DEFINE_IN_INTERFACE
#include "cellapp/cellapp_interface.h"

#undef DEFINE_IN_INTERFACE
#include "baseapp/baseapp_interface.h"
#define DEFINE_IN_INTERFACE
#include "baseapp/baseapp_interface.h"

#undef DEFINE_IN_INTERFACE
#include "dbmgr/dbmgr_interface.h"
#define DEFINE_IN_INTERFACE
#include "dbmgr/dbmgr_interface.h"

#undef DEFINE_IN_INTERFACE
#include "loginapp/loginapp_interface.h"
#define DEFINE_IN_INTERFACE
#include "loginapp/loginapp_interface.h"

#undef DEFINE_IN_INTERFACE
#include "tools/logger/logger_interface.h"
#define DEFINE_IN_INTERFACE
#include "tools/logger/logger_interface.h"

#undef DEFINE_IN_INTERFACE
#include "tools/interfaces/interfaces_interface.h"
#define DEFINE_IN_INTERFACE
#include "tools/interfaces/interfaces_interface.h"

#undef DEFINE_IN_INTERFACE
#include "tools/bots/bots_interface.h"
#define DEFINE_IN_INTERFACE
#include "tools/bots/bots_interface.h"

using namespace KBEngine;

int KBENGINE_MAIN(int argc, char* argv[])
{
#if KBE_PLATFORM != PLATFORM_WIN32
	rlimit rlimitData = { RLIM_INFINITY, RLIM_INFINITY };
	setrlimit(RLIMIT_CORE, &rlimitData);
#endif

	ENGINE_COMPONENT_INFO& info = g_kbeSrvConfig.getKCluster();

	// 集群对外服务使用自建 TCP 监听(servicePort/peerPort)，不绑定引擎外部端口；
	// 引擎内部网络(intTcpEndpoint)绑定内部网卡上的自动端口，用于接收/发送少量引擎级消息。
	int ret = kbeMainT<Cluster>(argc, argv, CLUSTER_TYPE, 
		-1, -1,		// extlisteningTcpPort_min/max
		-1, -1,		// extlisteningUdpPort_min/max
		"",			// extlisteningInterface
		0, 0,		// intlisteningPort_min/max
		info.internalInterface);
	return ret;
}
