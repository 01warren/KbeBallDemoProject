// Copyright 2008-2018 Yolo Technologies, Inc. All Rights Reserved. https://www.comblockengine.com

/*
	router 协议头的转发壳。

	协议的**唯一权威定义**已迁移到 lib/server/router_interface.h：
	原因是接入层(RouterClient)位于 lib/server，而 lib 无法反向包含 server/router 目录。
	这里保留同名壳文件，使 server/router 下的既有 #include "router_interface.h" 无需改动。
*/

#ifndef KBE_ROUTER_INTERFACE_SHIM_H
#define KBE_ROUTER_INTERFACE_SHIM_H

#include "server/router_interface.h"

#endif // KBE_ROUTER_INTERFACE_SHIM_H
