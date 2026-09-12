// Copyright 2008-2018 Yolo Technologies, Inc. All Rights Reserved. https://www.comblockengine.com

/*
	router_proto_client.cpp —— L3：真实 router 进程的零依赖协议客户端(II 段用例)。

	目的
	----
	进程内用例(I01–I06)用 driver 复刻了 RouterCore 的分派顺序，但复刻版无法证明
	"真实进程里的实现与复刻一致"。本程序直接连真实 router 的 serverPort，按线格式
	收发报文，覆盖 PENDING 冲刷 / 溢出 / TTL、SUSPEND-RESUME 迁移、寻址隔离、
	CLIENT_TYPE 快速失败、畸形报文等真实网络路径。

	依赖
	----
	零 KBE 依赖：只 include lib/server/router_interface.h(自包含) + 系统 socket。
	Windows 链 ws2_32.lib；Linux 用 POSIX socket。与 cluster/test/test_client.cpp 同一范式。

	重要：本程序**未在开发机运行验证**
	----------------------------------
	本机只有 VS2022(v143)，而 kbe/src/libs 下的预编译库是 v142 + /GL 构建的，
	链接必报 C1905(LTCG 前后端版本不兼容)，因此 router.exe 在本机构建不出来
	(详见 TESTCASES.md §6)。本程序保证"可编译 + 用例逻辑自洽"，
	端到端验证需在有 v142 工具集(或 Linux 构建)的机器上执行：
		run_tests.ps1 -WithRouter     /     sh run_tests.sh --with-router

	若连接不上 router，所有用例会打印失败原因而不是挂死：
	每个等待都有超时(--timeout-ms，默认 5000)，失败时打印期望/实际消息类型。

	用法
	----
		router_proto_client.exe [--host=127.0.0.1] [--port=20096]
		                        [--pending-max=512] [--pending-timeout-ms=30000]
		                        [--timeout-ms=5000] [--connect-timeout-ms=5000]
	退出码：0 = 全部通过；1 = 有用例失败；2 = router 不可达(环境不可用，编排器据此 SKIP)。
*/

#include "lib/server/router_interface.h"
#include "test_common.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#ifdef _WIN32
	#ifndef WIN32_LEAN_AND_MEAN
		#define WIN32_LEAN_AND_MEAN
	#endif
	#include <winsock2.h>
	#include <ws2tcpip.h>
	typedef SOCKET kbe_socket_t;
	static const kbe_socket_t KBE_SOCK_INVALID = INVALID_SOCKET;
#else
	#include <sys/types.h>
	#include <sys/socket.h>
	#include <sys/time.h>
	#include <netinet/in.h>
	#include <arpa/inet.h>
	#include <netdb.h>
	#include <unistd.h>
	#include <errno.h>
	typedef int kbe_socket_t;
	static const kbe_socket_t KBE_SOCK_INVALID = -1;
#endif

TestRunner g_runner;

using namespace KBEngine;
using namespace KBEngine::RouterInterface;

//=====================================================================================
// 组件类型：与 lib/common/common.h 的 COMPONENT_TYPE 保持一致。
// 这里刻意不包含 common.h(那会把 router_interface.h 从"自包含头"拖进引擎依赖)。
// 两边的一致性由 converge_static_check.ps1 的 S21–S23 等值守护。
//=====================================================================================
static const uint32_t CT_CELLAPP		= 5;
static const uint32_t CT_BASEAPP		= 6;
static const uint32_t CT_CLIENT			= 7;

//=====================================================================================
// 运行参数(由 main 解析)
//=====================================================================================
static const char*	g_host				= "127.0.0.1";
static uint16_t		g_port				= DEFAULT_ROUTER_SERVER_PORT;
static size_t		g_pendingMax		= 512;		// <router><pendingBufferMax>
static uint64_t		g_pendingTimeoutMS	= 30000;	// <router><pendingTimeout>
static int			g_timeoutMS			= 5000;		// 单次等待超时
static int			g_connectTimeoutMS	= 5000;		// 启动后等待 router 就绪的上限
static uint64_t		g_nextMsgId			= 1;

//=====================================================================================
// socket 基础
//=====================================================================================
static uint64_t nowMS()
{
#ifdef _WIN32
	return (uint64_t)::GetTickCount64();
#else
	struct timeval tv;
	::gettimeofday(&tv, NULL);
	return (uint64_t)tv.tv_sec * 1000ULL + (uint64_t)(tv.tv_usec / 1000);
#endif
}

static void closeSocket(kbe_socket_t s)
{
	if(s == KBE_SOCK_INVALID)
		return;

#ifdef _WIN32
	::closesocket(s);
#else
	::close(s);
#endif
}

static void sleepMS(int ms)
{
#ifdef _WIN32
	::Sleep((DWORD)ms);
#else
	::usleep((useconds_t)ms * 1000);
#endif
}

static kbe_socket_t connectHost(const char* host, uint16_t port)
{
	struct addrinfo hints;
	struct addrinfo* res = NULL;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;

	char portStr[16];
	snprintf(portStr, sizeof(portStr), "%u", (unsigned)port);

	if(::getaddrinfo(host, portStr, &hints, &res) != 0 || res == NULL)
		return KBE_SOCK_INVALID;

	kbe_socket_t s = ::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
	if(s == KBE_SOCK_INVALID)
	{
		::freeaddrinfo(res);
		return KBE_SOCK_INVALID;
	}

	if(::connect(s, res->ai_addr, (int)res->ai_addrlen) != 0)
	{
		::freeaddrinfo(res);
		closeSocket(s);
		return KBE_SOCK_INVALID;
	}

	::freeaddrinfo(res);
	return s;
}

static bool sendAll(kbe_socket_t s, const char* data, size_t len)
{
	size_t sent = 0;

	while(sent < len)
	{
		int n = (int)::send(s, data + sent, (int)(len - sent), 0);
		if(n <= 0)
			return false;

		sent += (size_t)n;
	}

	return true;
}

/** 在 deadline 前读满 want 字节；false = 超时或对端关闭。 */
static bool recvSome(kbe_socket_t s, char* buf, size_t want, uint64_t deadline)
{
	size_t got = 0;

	while(got < want)
	{
		uint64_t now = nowMS();
		if(now >= deadline)
			return false;

		uint64_t remain = deadline - now;

		fd_set rfds;
		FD_ZERO(&rfds);
		FD_SET(s, &rfds);

		struct timeval tv;
		tv.tv_sec = (long)(remain / 1000);
		tv.tv_usec = (long)((remain % 1000) * 1000);

		int r = ::select((int)(s + 1), &rfds, NULL, NULL, &tv);
		if(r <= 0)
			return false;

		int n = (int)::recv(s, buf + got, (int)(want - got), 0);
		if(n <= 0)
			return false;

		got += (size_t)n;
	}

	return true;
}

//=====================================================================================
// 帧与连接
//=====================================================================================
struct Message
{
	uint8_t		type;
	std::string	payload;

	Message() : type(0) {}
};

class Conn
{
public:
	Conn() : sock_(KBE_SOCK_INVALID), connId_(0) {}
	~Conn() { close(); }

	bool connectTo(const char* host, uint16_t port)
	{
		close();
		sock_ = connectHost(host, port);
		return sock_ != KBE_SOCK_INVALID;
	}

	bool isOpen() const { return sock_ != KBE_SOCK_INVALID; }

	uint64_t connId() const { return connId_; }
	void setConnId(uint64_t id) { connId_ = id; }

	void close()
	{
		closeSocket(sock_);
		sock_ = KBE_SOCK_INVALID;
		connId_ = 0;
	}

	/** 帧格式：[u32 大端长度][u8 类型][载荷] */
	bool send(uint8_t type, const std::string& payload)
	{
		if(!isOpen())
			return false;

		const std::string msg = encodeMessage(type, payload);

		std::string frame;
		Wire::putU32(frame, (uint32_t)msg.size());
		frame += msg;

		return sendAll(sock_, frame.data(), frame.size());
	}

	/** 1 = 收到一帧，0 = 超时/无数据，-1 = 协议错误或连接关闭 */
	int recv(Message& out, int timeoutMS)
	{
		if(!isOpen())
			return -1;

		const uint64_t deadline = nowMS() + (uint64_t)timeoutMS;

		char lenBuf[4];
		if(!recvSome(sock_, lenBuf, 4, deadline))
			return 0;

		uint32_t bodyLen = 0;
		{
			size_t off = 0;
			if(!Wire::getU32(lenBuf, 4, off, bodyLen))
				return -1;
		}

		if(bodyLen == 0 || bodyLen > ROUTER_FRAME_MAX)
		{
			close();
			return -1;
		}

		std::string body;
		body.resize(bodyLen);

		if(!recvSome(sock_, &body[0], bodyLen, deadline))
			return 0;

		out.type = (uint8_t)body[0];
		out.payload.assign(body, 1, bodyLen - 1);
		return 1;
	}

	/** 发一条请求并等待期望的应答；失败时打印期望/实际类型便于定位。 */
	bool request(uint8_t type, const std::string& payload, uint8_t expectType, Message* out = NULL)
	{
		if(!send(type, payload))
		{
			printf("             [FAIL] send msgType=0x%02X failed (socket error)\n", (unsigned)type);
			return false;
		}

		Message m;
		const int r = recv(m, g_timeoutMS);

		if(r <= 0)
		{
			printf("             [FAIL] wait msgType=0x%02X: %s\n", (unsigned)expectType,
				(r == 0) ? "timeout" : "connection closed");
			return false;
		}

		if(m.type != expectType)
		{
			printf("             [FAIL] expect msgType=0x%02X, got 0x%02X\n",
				(unsigned)expectType, (unsigned)m.type);
			return false;
		}

		if(out != NULL)
			*out = m;

		return true;
	}

	/** 等待一条主动下发的消息(MSG_SEND / MSG_DELIVERY_FAILED)。 */
	bool expect(uint8_t expectType, Message& out)
	{
		const int r = recv(out, g_timeoutMS);

		if(r <= 0)
		{
			printf("             [FAIL] wait msgType=0x%02X: %s\n", (unsigned)expectType,
				(r == 0) ? "timeout" : "connection closed");
			return false;
		}

		if(out.type != expectType)
		{
			printf("             [FAIL] expect msgType=0x%02X, got 0x%02X\n",
				(unsigned)expectType, (unsigned)out.type);
			return false;
		}

		return true;
	}

	/** 确认在 timeoutMS 内**没有**任何消息到达(用于"不应收到失败回报")。 */
	bool expectSilence(int timeoutMS)
	{
		Message m;
		const int r = recv(m, timeoutMS);

		if(r == 1)
		{
			printf("             [FAIL] unexpected msgType=0x%02X (expected silence)\n",
				(unsigned)m.type);
			return false;
		}

		return r == 0;
	}

private:
	kbe_socket_t	sock_;
	uint64_t		connId_;
};

//=====================================================================================
// 载荷构造 / 解析
//=====================================================================================
static std::string buildMailboxPayload(const MailboxAddress& a)
{
	std::string b;
	putMailbox(b, a);
	return b;
}

static std::string buildRegisterPayload(const MailboxAddress& a, uint64_t targetConnId)
{
	std::string b;
	putMailbox(b, a);
	Wire::putU64(b, targetConnId);
	return b;
}

static std::string buildSendPayload(const MailboxAddress& dst, const MailboxAddress& src,
	uint64_t msgId, const std::string& body)
{
	std::string b;
	putMailbox(b, dst);
	putMailbox(b, src);
	Wire::putU64(b, msgId);
	b += body;
	return b;
}

static bool parseSendPayload(const std::string& payload, MailboxAddress& dst,
	MailboxAddress& src, uint64_t& msgId, std::string& body)
{
	const char* d = payload.data();
	const size_t n = payload.size();
	size_t off = 0;

	if(!getMailbox(d, n, off, dst)) return false;
	if(!getMailbox(d, n, off, src)) return false;
	if(!Wire::getU64(d, n, off, msgId)) return false;

	body.assign(d + off, n - off);
	return true;
}

static bool parseDeliveryFailed(const std::string& payload, MailboxAddress& src,
	MailboxAddress& dst, uint64_t& msgId, uint32_t& code)
{
	const char* d = payload.data();
	const size_t n = payload.size();
	size_t off = 0;

	if(!getMailbox(d, n, off, src)) return false;
	if(!getMailbox(d, n, off, dst)) return false;
	if(!Wire::getU64(d, n, off, msgId)) return false;
	if(!Wire::getU32(d, n, off, code)) return false;

	return true;
}

/** 建立一条已握手的组件连接(HELLO -> MSG_RESP_HELLO_OK)。 */
static bool openPeer(Conn& c, uint32_t componentType, uint64_t componentId)
{
	if(!c.connectTo(g_host, g_port))
	{
		printf("             [FAIL] cannot connect %s:%u (router not running?)\n",
			g_host, (unsigned)g_port);

		// 必须记账：否则"连不上"会让用例空跑却报 OK，形成静默通过。
		g_runner.check(false, "connect router", __FILE__, __LINE__);
		return false;
	}

	std::string payload;
	Wire::putU8(payload, PEER_COMPONENT);
	Wire::putU32(payload, 7001);						// uid(测试固定值)
	Wire::putU32(payload, componentType);
	Wire::putU64(payload, componentId);
	Wire::putU16(payload, ROUTER_PROTOCOL_VERSION);

	Message resp;
	if(!c.request(MSG_HELLO, payload, MSG_RESP_HELLO_OK, &resp))
		return false;

	size_t off = 0;
	uint64_t cid = 0;

	if(!Wire::getU64(resp.payload.data(), resp.payload.size(), off, cid) || cid == 0)
	{
		printf("             [FAIL] MSG_RESP_HELLO_OK carries no connId\n");
		return false;
	}

	c.setConnId(cid);
	return true;
}

/** 发送一条普通业务消息，返回 msgId(便于回报断言)。 */
static bool sendMail(Conn& from, const MailboxAddress& dst, const MailboxAddress& src,
	const std::string& body, uint64_t* outMsgId = NULL)
{
	const uint64_t msgId = g_nextMsgId++;

	if(outMsgId != NULL)
		*outMsgId = msgId;

	return from.send(MSG_SEND, buildSendPayload(dst, src, msgId, body));
}

//=====================================================================================
// I11 HELLO 握手 + Actor 注册
//=====================================================================================
static void case_I11_hello_and_register()
{
	Conn a, b;

	if(!openPeer(a, CT_CELLAPP, 9001) || !openPeer(b, CT_BASEAPP, 9002))
		return;

	CHECK(a.connId() != 0);
	CHECK(b.connId() != 0);
	CHECK(a.connId() != b.connId());

	const MailboxAddress comp = mailboxComponent(9001, CT_CELLAPP);

	// targetConnId = 0 -> 绑定到本连接
	CHECK(a.request(MSG_REGISTER_ACTOR, buildRegisterPayload(comp, 0), MSG_RESP_OK));

	// 重复注册应幂等地返回 OK(宿主变更语义由 R01/R02 在进程内覆盖)
	CHECK(a.request(MSG_REGISTER_ACTOR, buildRegisterPayload(comp, 0), MSG_RESP_OK));

	CHECK(a.request(MSG_UNREGISTER_ACTOR, buildMailboxPayload(comp), MSG_RESP_OK));
}

//=====================================================================================
// I12 组件 Actor 直达投递
//=====================================================================================
static void case_I12_component_direct_delivery()
{
	Conn target, source;

	if(!openPeer(target, CT_CELLAPP, 9003) || !openPeer(source, CT_BASEAPP, 9004))
		return;

	const MailboxAddress dst = mailboxComponent(9003, CT_CELLAPP);
	const MailboxAddress src = mailboxComponent(9004, CT_BASEAPP);

	CHECK(target.request(MSG_REGISTER_ACTOR, buildRegisterPayload(dst, 0), MSG_RESP_OK));

	uint64_t sentMsgId = 0;
	CHECK(sendMail(source, dst, src, "component-hello", &sentMsgId));

	Message m;
	if(!target.expect(MSG_SEND, m))
		return;

	MailboxAddress outDst, outSrc;
	uint64_t msgId = 0;
	std::string body;

	CHECK(parseSendPayload(m.payload, outDst, outSrc, msgId, body));
	CHECK_EQ_INT(outDst.actorId, dst.actorId);
	CHECK_EQ_INT((int)outDst.kind, (int)MAILBOX_COMPONENT);
	CHECK_EQ_INT(outSrc.actorId, src.actorId);
	CHECK_EQ_INT(msgId, sentMsgId);
	CHECK_EQ_STR(body, "component-hello");
}

//=====================================================================================
// I13 目标未注册 -> PENDING -> 注册即冲刷(顺序)
//=====================================================================================
static void case_I13_pending_then_flush_on_register()
{
	Conn target, source;

	if(!openPeer(target, CT_CELLAPP, 9005) || !openPeer(source, CT_BASEAPP, 9006))
		return;

	const MailboxAddress dst = mailboxEntity(9100, CT_CELLAPP);
	const MailboxAddress src = mailboxComponent(9006, CT_BASEAPP);

	CHECK(sendMail(source, dst, src, "pending-1"));
	CHECK(sendMail(source, dst, src, "pending-2"));
	CHECK(sendMail(source, dst, src, "pending-3"));

	// 目标尚未注册：必须**不**立即回报 TARGET_NOT_FOUND(进待定缓冲等首次注册)
	CHECK(source.expectSilence(500));

	// 首次注册 -> takePending 后按到达顺序冲刷
	CHECK(target.request(MSG_REGISTER_ACTOR, buildRegisterPayload(dst, 0), MSG_RESP_OK));

	const char* expects[3] = { "pending-1", "pending-2", "pending-3" };

	for(int i = 0; i < 3; ++i)
	{
		Message m;
		if(!target.expect(MSG_SEND, m))
			return;

		MailboxAddress outDst, outSrc;
		uint64_t msgId = 0;
		std::string body;

		CHECK(parseSendPayload(m.payload, outDst, outSrc, msgId, body));
		CHECK_EQ_INT(outDst.actorId, dst.actorId);
		CHECK_EQ_STR(body, expects[i]);
	}

	// 注册之后的新消息直达(不再走缓冲)
	CHECK(sendMail(source, dst, src, "after-register"));

	Message m;
	if(!target.expect(MSG_SEND, m))
		return;

	MailboxAddress d2, s2;
	uint64_t id2 = 0;
	std::string body2;

	CHECK(parseSendPayload(m.payload, d2, s2, id2, body2));
	CHECK_EQ_STR(body2, "after-register");
}

//=====================================================================================
// I14 待定缓冲溢出：丢弃最旧一条并回报 DROPPED_PENDING
//=====================================================================================
static void case_I14_pending_overflow_reports_dropped()
{
	// pendingBufferMax=0 表示关闭待定缓冲：此时溢出用例无意义(会立即 TARGET_NOT_FOUND)
	if(g_pendingMax == 0)
	{
		g_runner.skipCase("pendingBufferMax=0 disables pending buffering");
		return;
	}

	Conn source;

	if(!openPeer(source, CT_BASEAPP, 9007))
		return;

	const MailboxAddress dst = mailboxEntity(9200, CT_CELLAPP);
	const MailboxAddress src = mailboxComponent(9007, CT_BASEAPP);

	uint64_t firstMsgId = 0;

	// 发 pendingBufferMax + 1 条：第 1 条(最旧)会被溢出丢弃
	for(size_t i = 0; i <= g_pendingMax; ++i)
	{
		if(!sendMail(source, dst, src, "overflow", (i == 0) ? &firstMsgId : NULL))
		{
			CHECK(false);
			return;
		}
	}

	// 收集失败回报(溢出可能有多条，至少一条)
	int dropped = 0;
	uint64_t droppedMsgId = 0;

	const uint64_t deadline = nowMS() + 3000;

	while(nowMS() < deadline)
	{
		Message m;
		const int r = source.recv(m, 300);

		if(r <= 0)
			break;

		if(m.type != MSG_DELIVERY_FAILED)
		{
			printf("             [FAIL] unexpected msgType=0x%02X\n", (unsigned)m.type);
			CHECK(false);
			return;
		}

		MailboxAddress fSrc, fDst;
		uint64_t fMsgId = 0;
		uint32_t code = 0;

		if(!parseDeliveryFailed(m.payload, fSrc, fDst, fMsgId, code))
		{
			CHECK(false);
			return;
		}

		if(code == DELIVERY_DROPPED_PENDING)
		{
			if(dropped == 0)
				droppedMsgId = fMsgId;

			++dropped;
		}
	}

	CHECK(dropped >= 1);
	// 丢弃的是队列里最旧的一条，即我们发的第一条
	CHECK_EQ_INT(droppedMsgId, firstMsgId);
}

//=====================================================================================
// I15 待定缓冲 TTL 到期：逐条回报 DROPPED_PENDING
//=====================================================================================
static void case_I15_pending_ttl_reports_each_mail()
{
	// 默认 pendingTimeout=30000ms，等待成本过高 -> 需要测试配置(见 assets/router_test.xml)
	if(g_pendingTimeoutMS > 10000)
	{
		g_runner.skipCase("pendingTimeout too large; run with the test config (--pending-timeout-ms=2000)");
		return;
	}

	Conn source;

	if(!openPeer(source, CT_BASEAPP, 9008))
		return;

	const MailboxAddress dst = mailboxEntity(9300, CT_CELLAPP);
	const MailboxAddress src = mailboxComponent(9008, CT_BASEAPP);

	CHECK(sendMail(source, dst, src, "ttl-1"));
	CHECK(sendMail(source, dst, src, "ttl-2"));

	// 到期后两条各自回报一次(发送方可能不同，故必须逐条)
	const uint64_t deadline = nowMS() + g_pendingTimeoutMS + 5000;
	int failedCount = 0;

	while(nowMS() < deadline && failedCount < 2)
	{
		Message m;
		const int r = source.recv(m, 500);

		if(r <= 0)
			continue;

		if(m.type != MSG_DELIVERY_FAILED)
		{
			printf("             [FAIL] unexpected msgType=0x%02X\n", (unsigned)m.type);
			CHECK(false);
			return;
		}

		MailboxAddress fSrc, fDst;
		uint64_t fMsgId = 0;
		uint32_t code = 0;

		if(!parseDeliveryFailed(m.payload, fSrc, fDst, fMsgId, code))
		{
			CHECK(false);
			return;
		}

		CHECK_EQ_INT(code, (int)DELIVERY_DROPPED_PENDING);
		CHECK_EQ_INT(fDst.actorId, dst.actorId);
		++failedCount;
	}

	CHECK_EQ_INT(failedCount, 2);

	// 到期丢弃后再注册：不应补投已丢弃的消息
	Conn target;
	if(!openPeer(target, CT_CELLAPP, 9009))
		return;

	CHECK(target.request(MSG_REGISTER_ACTOR, buildRegisterPayload(dst, 0), MSG_RESP_OK));
	CHECK(target.expectSilence(300));
}

//=====================================================================================
// I16 迁移：SUSPEND -> 缓冲 -> 新宿主注册(RESUME) -> 按序冲刷
//=====================================================================================
static void case_I16_migration_suspend_resume()
{
	Conn hostA, hostB, source;

	if(!openPeer(hostA, CT_BASEAPP, 9010) ||
		!openPeer(hostB, CT_BASEAPP, 9011) ||
		!openPeer(source, CT_BASEAPP, 9012))
		return;

	const MailboxAddress entity = mailboxEntity(9400, CT_BASEAPP);
	const MailboxAddress src = mailboxComponent(9012, CT_BASEAPP);

	// hostA 注册并确认可达
	CHECK(hostA.request(MSG_REGISTER_ACTOR, buildRegisterPayload(entity, 0), MSG_RESP_OK));
	CHECK(sendMail(source, entity, src, "before-move"));

	Message m;
	if(!hostA.expect(MSG_SEND, m))
		return;

	// 迁移开始：旧宿主挂起
	CHECK(hostA.request(MSG_SUSPEND_ACTOR, buildMailboxPayload(entity), MSG_RESP_OK));

	CHECK(sendMail(source, entity, src, "during-move-1"));
	CHECK(sendMail(source, entity, src, "during-move-2"));

	// 挂起期间旧宿主不应收到(消息进 suspended_ 缓冲)
	CHECK(hostA.expectSilence(400));

	// 迁移完成：新宿主注册 -> RouterCore 走 isSuspended 分支做 RESUME + 冲刷
	CHECK(hostB.request(MSG_REGISTER_ACTOR, buildRegisterPayload(entity, 0), MSG_RESP_OK));

	const char* expects[2] = { "during-move-1", "during-move-2" };

	for(int i = 0; i < 2; ++i)
	{
		Message m2;
		if(!hostB.expect(MSG_SEND, m2))
			return;

		MailboxAddress d, s;
		uint64_t id = 0;
		std::string body;

		CHECK(parseSendPayload(m2.payload, d, s, id, body));
		CHECK_EQ_STR(body, expects[i]);
	}

	// 迁移后的新消息直达新宿主
	CHECK(sendMail(source, entity, src, "after-move"));

	Message m3;
	if(!hostB.expect(MSG_SEND, m3))
		return;

	MailboxAddress d3, s3;
	uint64_t id3 = 0;
	std::string body3;

	CHECK(parseSendPayload(m3.payload, d3, s3, id3, body3));
	CHECK_EQ_STR(body3, "after-move");
}

//=====================================================================================
// I17 寻址隔离：同 actorId 的 entity 与 component 不互串
//=====================================================================================
static void case_I17_entity_component_isolation()
{
	Conn targetComp, targetEnt, source;

	if(!openPeer(targetComp, CT_CELLAPP, 9013) ||
		!openPeer(targetEnt, CT_CELLAPP, 9014) ||
		!openPeer(source, CT_BASEAPP, 9015))
		return;

	const MailboxAddress comp = mailboxComponent(9500, CT_CELLAPP);
	const MailboxAddress ent = mailboxEntity(9500, CT_CELLAPP);
	const MailboxAddress src = mailboxComponent(9015, CT_BASEAPP);

	CHECK(targetComp.request(MSG_REGISTER_ACTOR, buildRegisterPayload(comp, 0), MSG_RESP_OK));
	CHECK(targetEnt.request(MSG_REGISTER_ACTOR, buildRegisterPayload(ent, 0), MSG_RESP_OK));

	CHECK(sendMail(source, comp, src, "to-component"));

	Message mc;
	if(!targetComp.expect(MSG_SEND, mc))
		return;

	MailboxAddress dc, sc;
	uint64_t idc = 0;
	std::string bodyc;

	CHECK(parseSendPayload(mc.payload, dc, sc, idc, bodyc));
	CHECK_EQ_STR(bodyc, "to-component");
	CHECK_EQ_INT((int)dc.kind, (int)MAILBOX_COMPONENT);

	// 实体连接不应收到组件地址的消息
	CHECK(targetEnt.expectSilence(300));

	CHECK(sendMail(source, ent, src, "to-entity"));

	Message me;
	if(!targetEnt.expect(MSG_SEND, me))
		return;

	MailboxAddress de, se;
	uint64_t ide = 0;
	std::string bodye;

	CHECK(parseSendPayload(me.payload, de, se, ide, bodye));
	CHECK_EQ_STR(bodye, "to-entity");
	CHECK_EQ_INT((int)de.kind, (int)MAILBOX_ENTITY);
}

//=====================================================================================
// I18 PING/PONG 与畸形报文
//=====================================================================================
static void case_I18_ping_and_malformed()
{
	Conn c;

	if(!openPeer(c, CT_CELLAPP, 9016))
		return;

	CHECK(c.request(MSG_PING, std::string(), MSG_RESP_PONG));

	// 载荷不足 SEND_HEADER_WIRE_SIZE：必须回 DELIVERY_BAD_REQUEST 而不是断开连接
	Message m;
	if(!c.request(MSG_SEND, std::string("short"), MSG_DELIVERY_FAILED, &m))
		return;

	MailboxAddress src, dst;
	uint64_t msgId = 0;
	uint32_t code = 0;

	CHECK(parseDeliveryFailed(m.payload, src, dst, msgId, code));
	CHECK_EQ_INT(code, (int)DELIVERY_BAD_REQUEST);

	// 连接仍然可用
	CHECK(c.request(MSG_PING, std::string(), MSG_RESP_PONG));
}

//=====================================================================================
// I19 CLIENT_TYPE 未注册：快速失败(不进待定缓冲)
//=====================================================================================
static void case_I19_client_type_fails_fast()
{
	Conn source;

	if(!openPeer(source, CT_BASEAPP, 9017))
		return;

	// Proxy Actor 是 entityMailbox(eid, CLIENT_TYPE)：客户端断开即被注销，
	// 此时消息应快速失败，而不是被缓冲 pendingTimeout 那么久
	// (见 router_core.cpp 的 shouldBufferPending)。
	const MailboxAddress dst = mailboxEntity(9600, CT_CLIENT);
	const MailboxAddress src = mailboxComponent(9017, CT_BASEAPP);

	CHECK(sendMail(source, dst, src, "to-offline-client"));

	Message m;
	if(!source.expect(MSG_DELIVERY_FAILED, m))
		return;

	MailboxAddress fSrc, fDst;
	uint64_t fMsgId = 0;
	uint32_t code = 0;

	CHECK(parseDeliveryFailed(m.payload, fSrc, fDst, fMsgId, code));
	CHECK_EQ_INT(code, (int)DELIVERY_TARGET_NOT_FOUND);
	CHECK_EQ_INT(fDst.actorId, dst.actorId);
}

//=====================================================================================
// 入口
//=====================================================================================
static void parseArgs(int argc, char** argv)
{
	for(int i = 1; i < argc; ++i)
	{
		const char* a = argv[i];

		if(strncmp(a, "--host=", 7) == 0)
			g_host = a + 7;
		else if(strncmp(a, "--port=", 7) == 0)
			g_port = (uint16_t)atoi(a + 7);
		else if(strncmp(a, "--pending-max=", 14) == 0)
			g_pendingMax = (size_t)atoi(a + 14);
		else if(strncmp(a, "--pending-timeout-ms=", 21) == 0)
			g_pendingTimeoutMS = (uint64_t)atoi(a + 21);
		else if(strncmp(a, "--timeout-ms=", 13) == 0)
			g_timeoutMS = atoi(a + 13);
		else if(strncmp(a, "--connect-timeout-ms=", 21) == 0)
			g_connectTimeoutMS = atoi(a + 21);
	}
}

/** 预检：router 是否可达。不可达属于"环境不可用"，不是用例失败。 */
static bool probeRouter()
{
	kbe_socket_t s = connectHost(g_host, g_port);

	if(s == KBE_SOCK_INVALID)
		return false;

	closeSocket(s);
	return true;
}

/** 轮询等待 router 就绪：编排器刚拉起 router 时立即连接会失败。 */
static bool waitForRouter(uint64_t timeoutMS)
{
	const uint64_t deadline = nowMS() + timeoutMS;

	for(;;)
	{
		if(probeRouter())
			return true;

		if(nowMS() >= deadline)
			return false;

		sleepMS(200);
	}
}

int main(int argc, char** argv)
{
#ifdef _WIN32
	WSADATA wsa;
	if(WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
	{
		printf("[ERROR] WSAStartup failed\n");
		return 2;
	}
#endif

	parseArgs(argc, argv);

	printf("---- router_proto_client: host=%s port=%u pendingMax=%d pendingTimeout=%dms ----\n",
		g_host, (unsigned)g_port, (int)g_pendingMax, (int)g_pendingTimeoutMS);

	// 先探测一次(带重试)：连不上就直接退出(退出码 2 = 环境不可用)，
	// 避免把"没起 router"变成用例全过。编排器据此输出 SKIP 而不是 PASS/FAIL。
	printf("---- waiting for router (up to %dms) ...\n", g_connectTimeoutMS);

	if(!waitForRouter((uint64_t)g_connectTimeoutMS))
	{
		printf("[ERROR] router not reachable at %s:%u, exit=2 (environment unavailable)\n",
			g_host, (unsigned)g_port);

#ifdef _WIN32
		WSACleanup();
#endif

		return 2;
	}

	BEGIN_CASE("I11", "hello handshake + register/unregister actor");
	case_I11_hello_and_register();
	END_CASE();

	BEGIN_CASE("I12", "component actor direct delivery");
	case_I12_component_direct_delivery();
	END_CASE();

	BEGIN_CASE("I13", "unregistered target -> pending -> flush on register");
	case_I13_pending_then_flush_on_register();
	END_CASE();

	BEGIN_CASE("I14", "pending overflow drops oldest and reports");
	case_I14_pending_overflow_reports_dropped();
	END_CASE();

	BEGIN_CASE("I15", "pending TTL expiry reports every mail");
	case_I15_pending_ttl_reports_each_mail();
	END_CASE();

	BEGIN_CASE("I16", "migration suspend -> buffer -> resume flush");
	case_I16_migration_suspend_resume();
	END_CASE();

	BEGIN_CASE("I17", "entity/component addressing isolation");
	case_I17_entity_component_isolation();
	END_CASE();

	BEGIN_CASE("I18", "ping/pong + malformed send");
	case_I18_ping_and_malformed();
	END_CASE();

	BEGIN_CASE("I19", "CLIENT_TYPE target fails fast");
	case_I19_client_type_fails_fast();
	END_CASE();

#ifdef _WIN32
	WSACleanup();
#endif

	return g_runner.summary("router_proto_client");
}
