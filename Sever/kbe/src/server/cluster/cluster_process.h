// Copyright 2008-2018 Yolo Technologies, Inc. All Rights Reserved. https://www.comblockengine.com

/*
	cluster 本机代理执行器(process 控制面)。

	在 "不再运行 machine" 的架构下, guiconsole 的 startserver/stopserver/killserver
	经 cluster leader 路由后, 由组件所在主机的 cluster 副本在本机执行进程拉起/关闭。
	本模块不依赖任何组件私有网络协议:
		- start: fork/exec(Linux) 或 CreateProcess(Windows), 环境变量取父进程已导出的
		         KBE_ROOT/KBE_RES_PATH/KBE_BIN_PATH(未导出时尝试 Resmgr 默认值);
		- stop : 发送 SIGINT(Linux, 引擎 ServerApp 将其作为优雅退出信号) /
		         不带 /f 的 taskkill(Windows), 不等待退出(与原 machine 发送
		         reqCloseServer 后即返回的语义一致);
		- kill : SIGKILL(Linux) / taskkill /f(Windows), 轮询直到进程消失。

	注意: 本模块在 cluster 副本主循环内被调用, 各操作需保持耗时上界(见函数注释)。
*/
#ifndef KBE_CLUSTER_PROCESS_H
#define KBE_CLUSTER_PROCESS_H

#include "common/common.h"

#include <cstdint>
#include <string>
#include <vector>

namespace KBEngine
{
namespace ClusterProcess
{

// 由 leader 解析注册表后下发的单个组件实例(位于本副本所在主机)
struct CtlTarget
{
	uint32 pid;			// 组件进程 pid
	uint32 intaddr;		// 组件 internal 接口地址(网络字节序, 保留备用)
	uint16 intport;		// 组件 internal 接口端口(保留备用)

	CtlTarget() :
		pid(0),
		intaddr(0),
		intport(0)
	{
	}
};

/**
	本机拉起一个服务进程(Linux: fork+exec; Windows: CreateProcess)。

	@return 成功返回 true; 失败时 err 给出原因。
	@note 阻塞上限: fork/CreateProcess 本身毫秒级; exec 成败通过 CLOEXEC 管道探测,
	      select 等待上限约 1s。
*/
bool startProcess(int32 uid, COMPONENT_TYPE componentType, uint64 cid, uint16 gus, std::string& err);

/**
	本机优雅停止 target 中的每个实例(引擎以 SIGINT/关窗消息触发自身优雅退出)。

	@return 全部成功(或原本已不在运行)返回 true。
	@note 阻塞上限: 每个实例仅发信号 + 一次 taskkill, 毫秒级。
*/
bool stopProcess(COMPONENT_TYPE componentType, const std::vector<CtlTarget>& targets, std::string& err);

/**
	本机强制杀死 target 中的每个实例。

	@return 全部在轮询窗口内消失返回 true。
	@note 阻塞上限: 每实例轮询约 1.5s。
*/
bool killProcess(const std::vector<CtlTarget>& targets, std::string& err);

}
}

#endif // KBE_CLUSTER_PROCESS_H
