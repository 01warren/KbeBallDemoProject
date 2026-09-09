// Copyright 2008-2018 Yolo Technologies, Inc. All Rights Reserved. https://www.comblockengine.com

/*
	cluster 集群进程与服务器组件/guiconsole 之间的服务面协议。

	设计要点：
	1. 该协议为自包含的请求/响应二进制协议（不依赖 lib/network 的 MessageHandler digest 机制），
	   保证集群内部代码与组件侧(components.cpp/cluster_client)可独立演化；
	2. 字节序统一为网络字节序(大端)，字符串以 uint32 长度前缀 + UTF-8 原始字节传输；
	3. 单条消息 = 1 字节消息类型 + 载荷(payload)，客户端每次交互使用一次短连接，服务器应答后关闭；
	4. 所有字段命名与语义尽量对齐原 machine_interface.h 的
	   onBroadcastInterface / onFindInterfaceAddr / startserver / stopserver / killserver，
	   便于迁移与验证。

	说明：组件/工具请求发给任意副本的 servicePort；
	     非 leader 副本在服务尚未就绪或本身非 leader 时返回 MSG_RESP_NOT_LEADER(含 leader 地址)。
*/

#ifndef KBE_CLUSTER_INTERFACE_H
#define KBE_CLUSTER_INTERFACE_H

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace KBEngine {
namespace ClusterInterface {

// 协议版本（变更不兼容字段时递增）
static const uint8_t PROTOCOL_VERSION = 1;

// 默认端口(与 serverconfig.h / kbengine_defaults.xml 中 <cluster><servicePort>/<peerPort>
// 的默认值一致)。guiconsole 等外部工具读不到服务器配置文件时按此值连接副本的 servicePort。
static const uint16_t DEFAULT_CLUSTER_SERVICE_PORT = 20093;
static const uint16_t DEFAULT_CLUSTER_PEER_PORT = 20094;

// 消息类型
enum MsgType : uint8_t
{
	// ---- 客户端(组件/工具) -> 副本 ----
	MSG_QUERY_LEADER			= 0x01,	// 载荷：无；响应 MSG_RESP_LEADER 或 MSG_RESP_NOT_LEADER
	MSG_REGISTER				= 0x02,	// 载荷：encodeComponentData
	MSG_RENEW					= 0x03,	// 载荷：uid(int32) type(int32) cid(uint64) pid(uint32)
	MSG_UNREGISTER				= 0x04,	// 载荷：uid(int32) type(int32) cid(uint64)
	MSG_FIND					= 0x05,	// 载荷：uid(int32) findType(int32) findCid(uint64, 0=按类型找全部)
	MSG_QUERY_ALL				= 0x06,	// 载荷：uid(int32)
	MSG_START_SERVER			= 0x07,	// 载荷：encodeServerCommand
	MSG_STOP_SERVER				= 0x08,	// 载荷：encodeServerCommand
	MSG_KILL_SERVER				= 0x09,	// 载荷：encodeServerCommand

	// ---- 内部指令(仅 leader -> 指定主机副本, 副本收到后必须在本机执行, 不回leader) ----
	// 由本机代理在本机拉起/关闭进程(替代原 machine 在 guiconsole 指令下启动子进程)。
	// 副本不校验自身角色, 也不重定向回 leader, 避免循环转发。
	MSG_PROXY_START_SERVER		= 0x0A,	// 载荷：encodeServerCommand
	MSG_PROXY_STOP_SERVER		= 0x0B,	// 载荷：encodeServerCommand
	MSG_PROXY_KILL_SERVER		= 0x0C,	// 载荷：encodeServerCommand

	// ---- 副本 -> 客户端 响应 ----
	MSG_RESP_LEADER				= 0x81,	// 载荷：uint32 leaderIP, uint16 leaderServicePort
	MSG_RESP_NOT_LEADER			= 0x82,	// 载荷：uint32 leaderIP, uint16 leaderServicePort (未知时均为0)
	MSG_RESP_OK					= 0x83,	// 载荷：无
	MSG_RESP_NOT_FOUND			= 0x84,	// 载荷：无
	MSG_RESP_IDENTITY_CONFLICT	= 0x85,	// 载荷：encodeComponentData(已存在进程的信息)
	MSG_RESP_ENTRY				= 0x86,	// 载荷：encodeComponentData(找到的组件)
	MSG_RESP_ENTRY_LIST			= 0x87,	// 载荷：uint16 count + count * encodeComponentData
	MSG_RESP_CMD_RESULT			= 0x88,	// 载荷：int32 code, string message
	MSG_RESP_ERROR				= 0x89	// 载荷：string message
};

// 与 machine onBroadcastInterface 字段一一对应的注册信息
struct ComponentData
{
	int32_t uid;				// 工程uid
	std::string username;		// 账号
	int32_t componentType;		// COMPONENT_TYPE 数值
	uint64_t componentID;		// 组件全局ID
	uint64_t componentIDEx;		// 全局ID(扩展)
	int32_t globalOrder;		// global顺序
	int32_t groupOrder;			// 组内顺序
	uint16_t gus;				// gus(同机实例序号)
	uint32_t intaddr;			// 内网地址
	uint16_t intport;			// 内网端口
	uint32_t extaddr;			// 外网地址
	uint16_t extport;			// 外网端口
	std::string extaddrEx;		// 外部地址(扩展, 如字符串IP)
	uint32_t pid;				// 进程ID
	float cpu;					// cpu使用率
	float mem;					// 内存使用率
	uint32_t usedmem;			// 已使用内存(KB)
	int8_t state;				// 组件运行状态
	uint32_t machineID;			// 机器标识(原machine进程号, 集群下可为0)
	uint64_t extradata[4];		// 预留扩展数据

	ComponentData() : uid(0), componentType(-1), componentID(0), componentIDEx(0),
		globalOrder(0), groupOrder(0), gus(0), intaddr(0), intport(0), extaddr(0),
		extport(0), pid(0), cpu(0.f), mem(0.f), usedmem(0), state(0), machineID(0)
	{
		memset(extradata, 0, sizeof(extradata));
	}
};

// 远程起停命令数据(与原 machine startserver/stopserver/killserver 输入语义一致)
struct ServerCommandData
{
	int32_t uid;
	std::string username;
	int32_t componentType;
	uint64_t cid;			// 0 表示按类型解析
	uint16_t gus;
	std::string targetHost;	// 目标主机IP(空串 = 依据注册表/cid 定位)
	std::string rootPath;	// KBE_ROOT
	std::string resPath;	// KBE_RES_PATH
	std::string binPath;	// KBE_BIN_PATH

	ServerCommandData() : uid(0), componentType(-1), cid(0), gus(0) {}
};

// ---- 二进制序列化工具(网络字节序) ----

class Wire
{
public:
	static inline void putU8(std::string& b, uint8_t v) { b.append(1, (char)v); }
	static inline void putI8(std::string& b, int8_t v) { putU8(b, (uint8_t)v); }
	static inline void putU16(std::string& b, uint16_t v)
	{
		b.append(1, (char)((v >> 8) & 0xFF)); b.append(1, (char)(v & 0xFF));
	}
	static inline void putU32(std::string& b, uint32_t v)
	{
		b.append(1, (char)((v >> 24) & 0xFF)); b.append(1, (char)((v >> 16) & 0xFF));
		b.append(1, (char)((v >> 8) & 0xFF)); b.append(1, (char)(v & 0xFF));
	}
	static inline void putI32(std::string& b, int32_t v) { putU32(b, (uint32_t)v); }
	static inline void putU64(std::string& b, uint64_t v)
	{
		for(int i = 7; i >= 0; --i) b.append(1, (char)((v >> (i * 8)) & 0xFF));
	}
	static inline void putFloat(std::string& b, float v)
	{
		uint32_t t = 0; std::memcpy(&t, &v, 4); putU32(b, t);
	}
	static inline void putString(std::string& b, const std::string& s)
	{
		putU32(b, (uint32_t)s.size());
		b.append(s);
	}

	static inline bool getU8(const char* d, size_t size, size_t& off, uint8_t& v)
	{
		if(off + 1 > size) return false; v = (uint8_t)d[off]; off += 1; return true;
	}
	static inline bool getI8(const char* d, size_t size, size_t& off, int8_t& v)
	{
		uint8_t t = 0;
		if(!getU8(d, size, off, t)) return false;
		v = (int8_t)t; return true;
	}
	static inline bool getU16(const char* d, size_t size, size_t& off, uint16_t& v)
	{
		if(off + 2 > size) return false;
		v = (uint16_t)(((uint8_t)d[off] << 8) | (uint8_t)d[off + 1]); off += 2; return true;
	}
	static inline bool getU32(const char* d, size_t size, size_t& off, uint32_t& v)
	{
		if(off + 4 > size) return false;
		v = ((uint32_t)(uint8_t)d[off] << 24) | ((uint32_t)(uint8_t)d[off + 1] << 16) |
			((uint32_t)(uint8_t)d[off + 2] << 8) | (uint32_t)(uint8_t)d[off + 3];
		off += 4; return true;
	}
	static inline bool getI32(const char* d, size_t size, size_t& off, int32_t& v)
	{
		uint32_t t = 0;
		if(!getU32(d, size, off, t)) return false;
		v = (int32_t)t; return true;
	}
	static inline bool getU64(const char* d, size_t size, size_t& off, uint64_t& v)
	{
		if(off + 8 > size) return false;
		v = 0;
		for(int i = 0; i < 8; ++i) v = (v << 8) | (uint8_t)d[off + i];
		off += 8; return true;
	}
	static inline bool getFloat(const char* d, size_t size, size_t& off, float& v)
	{
		uint32_t t = 0;
		if(!getU32(d, size, off, t)) return false;
		std::memcpy(&v, &t, 4); return true;
	}
	static inline bool getString(const char* d, size_t size, size_t& off, std::string& v)
	{
		uint32_t len = 0;
		if(!getU32(d, size, off, len)) return false;
		if((size_t)off + len > size) return false;
		v.assign(d + off, len); off += len; return true;
	}
};

// ---- 消息打包(客户端) ----

// 将一条消息打包: [type][payload]
inline std::string encodeMessage(uint8_t type, const std::string& payload)
{
	std::string msg;
	Wire::putU8(msg, type);
	msg += payload;
	return msg;
}

inline std::string encodeComponentData(const ComponentData& c)
{
	std::string b;
	Wire::putU8(b, PROTOCOL_VERSION);
	Wire::putU32(b, (uint32_t)c.uid);
	Wire::putString(b, c.username);
	Wire::putU32(b, (uint32_t)c.componentType);
	Wire::putU64(b, c.componentID);
	Wire::putU64(b, c.componentIDEx);
	Wire::putU32(b, (uint32_t)c.globalOrder);
	Wire::putU32(b, (uint32_t)c.groupOrder);
	Wire::putU16(b, c.gus);
	Wire::putU32(b, c.intaddr);
	Wire::putU16(b, c.intport);
	Wire::putU32(b, c.extaddr);
	Wire::putU16(b, c.extport);
	Wire::putString(b, c.extaddrEx);
	Wire::putU32(b, c.pid);
	Wire::putFloat(b, c.cpu);
	Wire::putFloat(b, c.mem);
	Wire::putU32(b, c.usedmem);
	Wire::putI8(b, c.state);
	Wire::putU32(b, c.machineID);
	for(int i = 0; i < 4; ++i) Wire::putU64(b, c.extradata[i]);
	return b;
}

inline bool decodeComponentData(const char* d, size_t size, size_t& off, ComponentData& c)
{
	uint8_t ver = 0;
	if(!Wire::getU8(d, size, off, ver) || ver != PROTOCOL_VERSION) return false;
	uint32_t t32 = 0;
	if(!Wire::getU32(d, size, off, t32)) return false; c.uid = (int32_t)t32;
	if(!Wire::getString(d, size, off, c.username)) return false;
	if(!Wire::getU32(d, size, off, t32)) return false; c.componentType = (int32_t)t32;
	if(!Wire::getU64(d, size, off, c.componentID)) return false;
	if(!Wire::getU64(d, size, off, c.componentIDEx)) return false;
	if(!Wire::getU32(d, size, off, t32)) return false; c.globalOrder = (int32_t)t32;
	if(!Wire::getU32(d, size, off, t32)) return false; c.groupOrder = (int32_t)t32;
	if(!Wire::getU16(d, size, off, c.gus)) return false;
	if(!Wire::getU32(d, size, off, c.intaddr)) return false;
	if(!Wire::getU16(d, size, off, c.intport)) return false;
	if(!Wire::getU32(d, size, off, c.extaddr)) return false;
	if(!Wire::getU16(d, size, off, c.extport)) return false;
	if(!Wire::getString(d, size, off, c.extaddrEx)) return false;
	if(!Wire::getU32(d, size, off, c.pid)) return false;
	if(!Wire::getFloat(d, size, off, c.cpu)) return false;
	if(!Wire::getFloat(d, size, off, c.mem)) return false;
	if(!Wire::getU32(d, size, off, c.usedmem)) return false;
	if(!Wire::getI8(d, size, off, c.state)) return false;
	if(!Wire::getU32(d, size, off, c.machineID)) return false;
	for(int i = 0; i < 4; ++i)
		if(!Wire::getU64(d, size, off, c.extradata[i])) return false;
	return true;
}

inline std::string encodeServerCommand(const ServerCommandData& cmd)
{
	std::string b;
	Wire::putU8(b, PROTOCOL_VERSION);
	Wire::putU32(b, (uint32_t)cmd.uid);
	Wire::putString(b, cmd.username);
	Wire::putU32(b, (uint32_t)cmd.componentType);
	Wire::putU64(b, cmd.cid);
	Wire::putU16(b, cmd.gus);
	Wire::putString(b, cmd.targetHost);
	Wire::putString(b, cmd.rootPath);
	Wire::putString(b, cmd.resPath);
	Wire::putString(b, cmd.binPath);
	return b;
}

inline bool decodeServerCommand(const char* d, size_t size, size_t& off, ServerCommandData& cmd)
{
	uint8_t ver = 0;
	if(!Wire::getU8(d, size, off, ver) || ver != PROTOCOL_VERSION) return false;
	uint32_t t32 = 0;
	if(!Wire::getU32(d, size, off, t32)) return false; cmd.uid = (int32_t)t32;
	if(!Wire::getString(d, size, off, cmd.username)) return false;
	if(!Wire::getU32(d, size, off, t32)) return false; cmd.componentType = (int32_t)t32;
	if(!Wire::getU64(d, size, off, cmd.cid)) return false;
	if(!Wire::getU16(d, size, off, cmd.gus)) return false;
	if(!Wire::getString(d, size, off, cmd.targetHost)) return false;
	if(!Wire::getString(d, size, off, cmd.rootPath)) return false;
	if(!Wire::getString(d, size, off, cmd.resPath)) return false;
	if(!Wire::getString(d, size, off, cmd.binPath)) return false;
	return true;
}

} // ClusterInterface
} // KBEngine

#endif // KBE_CLUSTER_INTERFACE_H
