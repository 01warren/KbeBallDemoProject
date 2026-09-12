// Copyright 2008-2018 Yolo Technologies, Inc. All Rights Reserved. https://www.comblockengine.com

/*
	router 路由中心进程与 peer(客户端 / 服务器组件)之间的通信协议。

	位置说明：本头文件是**唯一权威定义**，放在 lib/server 下是为了让
	"lib/server 的接入层 RouterClient" 与 "server/router 的进程实现" 共用同一份协议
	(两者都包含 lib 作为头文件根目录，而 server/router 目录无法被 lib 反向包含)。

	设计要点(详见 docs/kbe_router_design.md 第 5 节)：
	1. 自包含的二进制协议(不依赖 lib/network 的 MessageHandler digest 机制)，
	   使 router 无需包含所有组件的 *_interface.h 即可与任意 peer 通信；
	2. 帧格式与 cluster 一致：u32 长度(网络字节序) + [u8 消息类型][载荷]；
	3. 字节序统一为网络字节序(大端)，字符串为 uint32 长度前缀 + UTF-8 原始字节；
	4. MSG_SEND 的载荷 = dst + src + msgId + body。router 只解析 dst(前 13 字节)，
	   然后**原样转发整个载荷**，因此 router 是"TCP 的延伸"，不理解业务体(body)；
	5. router 不做可靠性：只在无法投递时回 MSG_DELIVERY_FAILED(见设计文档 §8 投递语义)。
*/

#ifndef KBE_ROUTER_INTERFACE_H
#define KBE_ROUTER_INTERFACE_H

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace KBEngine {
namespace RouterInterface {

// 协议版本(变更不兼容字段时递增)
// 注意：刻意带 ROUTER_ 前缀。MySQL 头文件里定义了同名宏 PROTOCOL_VERSION，
// 而本头文件会被 db_mysql(经 entitydef/entitydef.h) 间接包含，
// 裸名会被预处理期替换成数字，产生 "语法错误:常数"。FRAME_MAX 同理加固。
static const uint8_t ROUTER_PROTOCOL_VERSION = 1;

// 单帧最大长度(1MB，防呆)
static const uint32_t ROUTER_FRAME_MAX = 1024 * 1024;

// 默认端口(与 serverconfig.h / kbengine_defaults.xml 中 <router><clientPort>/<serverPort> 默认值一致)
static const uint16_t DEFAULT_ROUTER_CLIENT_PORT = 20095;
static const uint16_t DEFAULT_ROUTER_SERVER_PORT = 20096;

// peer 类别
enum PeerKind : uint8_t
{
	PEER_CLIENT		= 0,	// 客户端(经 clientPort 接入)
	PEER_COMPONENT	= 1		// 服务器进程(经 serverPort 接入)
};

// Actor(MailBox)类别
enum MailboxKind : uint8_t
{
	MAILBOX_ENTITY		= 0,	// 实体(base/cell 实体)
	MAILBOX_COMPONENT	= 1,	// 进程/组件
	MAILBOX_SERVICE		= 2		// 业务服务(FriendService/TeamService...)
};

// 投递结果(失败时通过 MSG_DELIVERY_FAILED 回报发送方)
enum DeliveryResult : uint32_t
{
	DELIVERY_OK						= 0,
	DELIVERY_TARGET_NOT_FOUND		= 1,	// 路由表无此 Actor(未注册/已注销)
	DELIVERY_TARGET_UNREACHABLE		= 2,	// 目标连接已断开或不可写
	DELIVERY_DROPPED_SUSPENDED		= 3,	// 目标处于迁移挂起且缓冲溢出
	DELIVERY_BAD_REQUEST			= 4,	// 报文不合法
	DELIVERY_DROPPED_PENDING		= 5		// 目标始终未注册，待定缓冲溢出或超时被丢弃(设计文档 §6.2/§14.4)
};

// 消息类型
enum MsgType : uint8_t
{
	// ---- peer -> router ----
	MSG_HELLO				= 0x01,	// 载荷：peerKind(1) uid(4) componentType(4) componentId(8) version(2)
	MSG_PING				= 0x02,	// 载荷：无
	MSG_REGISTER_ACTOR		= 0x03,	// 载荷：mailbox(13) + targetConnId(8, 0=绑定到本连接)
	MSG_UNREGISTER_ACTOR	= 0x04,	// 载荷：mailbox(13)
	MSG_SUSPEND_ACTOR		= 0x05,	// 载荷：mailbox(13)
	MSG_RESUME_ACTOR		= 0x06,	// 载荷：mailbox(13)
	MSG_REGISTER_SERVICE	= 0x07,	// 载荷：serviceName(str) + mailbox(13)
	MSG_FIND_SERVICE		= 0x08,	// 载荷：serviceName(str)
	MSG_SEND				= 0x10,	// 载荷：dst(13) + src(13) + msgId(8) + body(...)

	// ---- router -> peer ----
	MSG_RESP_HELLO_OK		= 0x81,	// 载荷：connId(8)
	MSG_RESP_OK				= 0x82,	// 载荷：无
	MSG_RESP_PONG			= 0x83,	// 载荷：无
	MSG_RESP_NOT_FOUND		= 0x84,	// 载荷：无
	MSG_RESP_SERVICE		= 0x85,	// 载荷：count(2) + count * mailbox(13)
	MSG_RESP_ERROR			= 0x86,	// 载荷：string message
	MSG_DELIVERY_FAILED		= 0x90	// 载荷：src(13) + dst(13) + msgId(8) + code(4)
};

// MailBox：Actor 的逻辑通信地址(不含 IP/端口，因此实体迁移后地址不变)
struct MailboxAddress
{
	uint8_t  kind;				// MailboxKind
	uint32_t componentType;		// 元数据：宿主组件类型(COMPONENT_TYPE)，便于观测
	uint64_t actorId;			// 全局唯一 id(ENTITY_ID / COMPONENT_ID / Service id)

	MailboxAddress()
		: kind(MAILBOX_ENTITY), componentType(0), actorId(0)
	{
	}

	MailboxAddress(uint8_t k, uint32_t ct, uint64_t id)
		: kind(k), componentType(ct), actorId(id)
	{
	}

	bool operator==(const MailboxAddress& o) const
	{
		return kind == o.kind && actorId == o.actorId;
	}

	bool operator!=(const MailboxAddress& o) const
	{
		return !(*this == o);
	}
};

// MailboxAddress 的线格式长度：kind(1) + componentType(4) + actorId(8)
static const size_t MAILBOX_WIRE_SIZE = 1 + 4 + 8;

// MSG_SEND 载荷的最小长度：dst(13) + src(13) + msgId(8)
static const size_t SEND_HEADER_WIRE_SIZE = MAILBOX_WIRE_SIZE * 2 + 8;

// ---- 二进制序列化工具(网络字节序) ----

class Wire
{
public:
	static inline void putU8(std::string& b, uint8_t v)
	{
		b.append(1, (char)v);
	}

	static inline void putU16(std::string& b, uint16_t v)
	{
		b.append(1, (char)((v >> 8) & 0xFF));
		b.append(1, (char)(v & 0xFF));
	}

	static inline void putU32(std::string& b, uint32_t v)
	{
		b.append(1, (char)((v >> 24) & 0xFF));
		b.append(1, (char)((v >> 16) & 0xFF));
		b.append(1, (char)((v >> 8) & 0xFF));
		b.append(1, (char)(v & 0xFF));
	}

	static inline void putU64(std::string& b, uint64_t v)
	{
		for(int i = 7; i >= 0; --i)
			b.append(1, (char)((v >> (i * 8)) & 0xFF));
	}

	static inline void putString(std::string& b, const std::string& s)
	{
		putU32(b, (uint32_t)s.size());
		b.append(s);
	}

	static inline bool getU8(const char* d, size_t size, size_t& off, uint8_t& v)
	{
		if(off + 1 > size) return false;
		v = (uint8_t)d[off];
		off += 1;
		return true;
	}

	static inline bool getU16(const char* d, size_t size, size_t& off, uint16_t& v)
	{
		if(off + 2 > size) return false;
		v = (uint16_t)(((uint8_t)d[off] << 8) | (uint8_t)d[off + 1]);
		off += 2;
		return true;
	}

	static inline bool getU32(const char* d, size_t size, size_t& off, uint32_t& v)
	{
		if(off + 4 > size) return false;
		v = ((uint32_t)(uint8_t)d[off] << 24) | ((uint32_t)(uint8_t)d[off + 1] << 16) |
			((uint32_t)(uint8_t)d[off + 2] << 8) | (uint32_t)(uint8_t)d[off + 3];
		off += 4;
		return true;
	}

	static inline bool getU64(const char* d, size_t size, size_t& off, uint64_t& v)
	{
		if(off + 8 > size) return false;
		v = 0;
		for(int i = 0; i < 8; ++i)
			v = (v << 8) | (uint8_t)d[off + i];
		off += 8;
		return true;
	}

	static inline bool getString(const char* d, size_t size, size_t& off, std::string& v)
	{
		uint32_t len = 0;
		if(!getU32(d, size, off, len)) return false;
		if((size_t)off + len > size) return false;
		v.assign(d + off, len);
		off += len;
		return true;
	}
};

// ---- MailBox 编解码 ----

inline void putMailbox(std::string& b, const MailboxAddress& a)
{
	Wire::putU8(b, a.kind);
	Wire::putU32(b, a.componentType);
	Wire::putU64(b, a.actorId);
}

inline bool getMailbox(const char* d, size_t size, size_t& off, MailboxAddress& a)
{
	if(!Wire::getU8(d, size, off, a.kind)) return false;
	if(!Wire::getU32(d, size, off, a.componentType)) return false;
	if(!Wire::getU64(d, size, off, a.actorId)) return false;
	return true;
}

// 打包一条消息：[type][payload]
inline std::string encodeMessage(uint8_t type, const std::string& payload)
{
	std::string msg;
	Wire::putU8(msg, type);
	msg += payload;
	return msg;
}

// ---- 常用地址构造 ----

// 实体(actorId 用 ENTITY_ID)
inline MailboxAddress mailboxEntity(uint64_t entityId, uint32_t hostComponentType)
{
	return MailboxAddress(MAILBOX_ENTITY, hostComponentType, entityId);
}

// 进程/组件(actorId 用 COMPONENT_ID)
inline MailboxAddress mailboxComponent(uint64_t componentId, uint32_t componentType)
{
	return MailboxAddress(MAILBOX_COMPONENT, componentType, componentId);
}

// 服务名 -> 稳定 id(FNV-1a 64)。注册方与调用方用同一算法，因此地址一致。
inline uint64_t hashServiceName(const std::string& name)
{
	uint64_t h = 1469598103934665603ULL;
	for(size_t i = 0; i < name.size(); ++i)
	{
		h ^= (uint8_t)name[i];
		h *= 1099511628211ULL;
	}
	return h;
}

// 业务服务(actorId 用服务名哈希，便于注册方/调用方得到相同地址)
inline MailboxAddress mailboxService(const std::string& name)
{
	return MailboxAddress(MAILBOX_SERVICE, 0, hashServiceName(name));
}

} // RouterInterface
} // KBEngine

#endif // KBE_ROUTER_INTERFACE_H
