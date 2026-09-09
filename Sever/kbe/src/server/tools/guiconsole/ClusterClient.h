#pragma once
#include "common/common.h"
#include "cluster/cluster_interface.h"

#include <string>
#include <vector>

// guiconsole 到 cluster 副本 servicePort 的短连接请求客户端(阻塞式)。
//
// 帧格式与 lib/server/components.cpp 中组件侧的 cluster 客户端保持一致：
//     u32(网络字节序长度) + [u8 type][payload]
// 服务端每帧应答后关闭连接；应答若为 MSG_RESP_NOT_LEADER，本客户端自动跟随
// 应答携带的 leader 地址重发一次(最多重定向一次)。
//
// 说明：cluster 服务端口默认取 ClusterInterface::DEFAULT_CLUSTER_SERVICE_PORT(20093)，
// 与 <cluster><servicePort> 配置一致；guiconsole 读不到远端服务器配置，若部署时
// 修改了 servicePort，请同步修改此处或调用方传入的 servicePort。
namespace KBEngine {
namespace ClusterClient {

struct CtlResult
{
	uint8 type;			// 应答消息类型(MSG_RESP_*)
	int32 code;			// MSG_RESP_CMD_RESULT 业务码(0=成功, <0 失败)
	std::string message;	// MSG_RESP_CMD_RESULT 的描述信息
	bool reached;			// 传输层取回一帧有效应答
	bool redirected;		// 期间是否经历过 NOT_LEADER 重定向

	CtlResult() : type(0), code(0), reached(false), redirected(false)
	{
	}
};

// 发送一条完整消息(首字节必须为消息类型)到 ip(点分字符串):servicePort(主机序端口)。
// 返回 false 表示传输失败或尚无可用 leader；返回 true 表示取回应答(结果在 result 中)。
bool request(const std::string& requestMsg, const std::string& ip, uint16 servicePort,
	CtlResult& result, uint32 timeoutMS = 5000);

// 便捷接口：发送 start/stop/kill 指令。
// op 为 MSG_START_SERVER / MSG_STOP_SERVER / MSG_KILL_SERVER。
// uid/componentType/cid/gus/targetHost 与 ServerCommandData 语义一致(cid=0 表示按类型定位)。
bool sendServerCommand(uint8 op, int32 uid, int32 componentType, uint64 cid, uint16 gus,
	const std::string& targetHost, const std::string& ip, uint16 servicePort,
	CtlResult& result, uint32 timeoutMS = 5000);

// 向 ip:servicePort 的 cluster 副本发起 MSG_QUERY_ALL(全注册表查询，payload=uid)，
// 取回该 uid 下全部组件的注册数据。成功返回 true 并填充 out(按应答原始顺序)；
// 失败(传输错误/尚无 leader/协议不符)返回 false。
bool queryAllComponents(int32 uid, const std::string& ip, uint16 servicePort,
	std::vector<ClusterInterface::ComponentData>& out, uint32 timeoutMS = 5000);

}
}
