// Copyright 2008-2025 Yolo Technologies, Inc. All Rights Reserved. https://www.comblockengine.com
/*
	cluster 测试客户端(test_client)。

	测试工程专用协议客户端：仅依赖自包含的 cluster_interface.h 与系统 socket。
	相对冒烟客户端新增:
		- ctl start/stop/kill (MSG_START_SERVER/STOP/KILL, 覆盖本机代理起停链路);
		- 每条命令可设响应超时(--timeout=ms, 默认 8000), 用于"多数派丢失后写请求不提交"
		  等预期超时场景;
		- 机读输出行 RESP=/COUNT=/LEADER=/CODE=/ELAPSED_MS=, 供 run_tests.ps1 断言;
		- 退出码: 0=业务成功; 1=网络错误/超时; 2=收到负向应答(如 NOT_LEADER/NOT_FOUND/
		  IDENTITY_CONFLICT/CMD_RESULT code!=0); 3=用法错误。

	构建(Windows, VS 开发者命令行):
	    cl /nologo /EHsc /I<cluster 目录> test_client.cpp /Fe:test_client.exe /link ws2_32.lib

	命令:
	    test_client <host> <port> leader
	    test_client <host> <port> reg <uid> <type> <cid> <pid> [gus]
	    test_client <host> <port> renew <uid> <type> <cid> <pid>
	    test_client <host> <port> unreg <uid> <type> <cid>
	    test_client <host> <port> find <uid> <type> [cid]         (cid=0=按类型找全部)
	    test_client <host> <port> queryall <uid>
	    test_client <host> <port> ctl <start|stop|kill> <uid> <type> <cid> <gus> [targetHost]
	    (可选尾参) --timeout=MS
*/
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "cluster_interface.h"

#ifdef _WIN32
	#include <winsock2.h>
	#include <ws2tcpip.h>
	#pragma comment(lib, "ws2_32.lib")
	typedef int socklen_t;
#else
	#include <sys/socket.h>
	#include <arpa/inet.h>
	#include <netinet/in.h>
	#include <unistd.h>
	#include <netdb.h>
	#include <sys/time.h>
	#define INVALID_SOCKET (-1)
	#define SOCKET_ERROR (-1)
	typedef int SOCKET;
	#define closesocket ::close
#endif

using namespace KBEngine::ClusterInterface;

// -------------------------------------------------------------------------------------
static SOCKET connectTo(const char* host, uint16_t port)
{
#ifdef _WIN32
	static bool wsaReady = false;
	if(!wsaReady) { WSADATA wsa; WSAStartup(MAKEWORD(2, 2), &wsa); wsaReady = true; }
#endif

	uint32_t addr = ::inet_addr(host);
	if(addr == INADDR_NONE)
	{
		struct hostent* he = ::gethostbyname(host);
		if(!he) { std::printf("RESP=NETWORK_ERROR detail=resolve_failed\n"); return INVALID_SOCKET; }
		std::memcpy(&addr, he->h_addr, 4);
	}

	SOCKET s = ::socket(AF_INET, SOCK_STREAM, 0);
	if(s == INVALID_SOCKET) return s;

	struct sockaddr_in sa;
	std::memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_port = htons(port);
	sa.sin_addr.s_addr = addr;
	if(::connect(s, (struct sockaddr*)&sa, sizeof(sa)) == SOCKET_ERROR)
	{
		std::printf("RESP=NETWORK_ERROR detail=connect_failed\n");
		closesocket(s);
		return INVALID_SOCKET;
	}
	return s;
}

// 以毫秒计的单调时间(用于超时)
static long long nowMS()
{
#ifdef _WIN32
	return (long long)::GetTickCount64();
#else
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return (long long)tv.tv_sec * 1000 + tv.tv_usec / 1000;
#endif
}

// 在 deadline 前尽量收满 n 字节; 返回实际收到字节数
static int recvSome(SOCKET s, char* buf, int want, long long deadlineMS)
{
	int got = 0;
	while(got < want)
	{
		long long remain = deadlineMS - nowMS();
		if(remain <= 0) break;

#ifdef _WIN32
		fd_set fds; FD_ZERO(&fds); FD_SET(s, &fds);
		struct timeval tv; tv.tv_sec = 0;
		tv.tv_usec = (long)(remain > 1000 ? 1000 : remain) * 1000;
#else
		fd_set fds; FD_ZERO(&fds); FD_SET(s, &fds);
		struct timeval tv; tv.tv_sec = 0;
		tv.tv_usec = (long)(remain > 1000 ? 1000 : remain) * 1000;
#endif
		int sel = ::select(s + 1, &fds, NULL, NULL, &tv);
		if(sel <= 0) continue;	// 超时或被打断则继续等到总 deadline

		int n = ::recv(s, buf + got, want - got, 0);
		if(n <= 0) break;		// 连接关闭/错误
		got += n;
	}
	return got;
}

// -------------------------------------------------------------------------------------
// 发送一条消息并读取完整响应帧。
// 服务端帧格式: [u32 大端长度][type][payload]。
// 成功返回 true 并填充 resp=[type][payload]; 超时/断开返回 false。
static bool roundtrip(const char* host, uint16_t port, uint8_t type,
	const std::string& payload, std::vector<uint8_t>& resp, int timeoutMS)
{
	long long deadline = nowMS() + timeoutMS;

	SOCKET s = connectTo(host, port);
	if(s == INVALID_SOCKET) return false;

	std::string msg = encodeMessage(type, payload);
	std::string frame;
	Wire::putU32(frame, (uint32_t)msg.size());
	frame += msg;

	int sent = 0;
	while(sent < (int)frame.size())
	{
		long long remain = deadline - nowMS();
		if(remain <= 0) { closesocket(s); return false; }
		int n = ::send(s, frame.data() + sent, (int)frame.size() - sent, 0);
		if(n <= 0) { closesocket(s); return false; }
		sent += n;
	}

	// 先收 4 字节长度前缀
	char lenBuf[4] = { 0 };
	if(recvSome(s, lenBuf, 4, deadline) < 4)
	{
		closesocket(s);
		return false;
	}

	uint32_t bodyLen = 0;
	{
		size_t o = 0;
		Wire::getU32(lenBuf, 4, o, bodyLen);
	}
	if(bodyLen == 0 || bodyLen > (16 * 1024 * 1024))
	{
		closesocket(s);
		return false;
	}

	resp.resize(bodyLen);
	int got = recvSome(s, (char*)resp.data(), (int)bodyLen, deadline);
	closesocket(s);

	if(got < (int)bodyLen)
		return false;

	// resp 已是 [type][payload]
	return true;
}

// -------------------------------------------------------------------------------------
static const char* typeName(uint8_t t)
{
	switch(t)
	{
	case MSG_RESP_LEADER: return "MSG_RESP_LEADER";
	case MSG_RESP_NOT_LEADER: return "MSG_RESP_NOT_LEADER";
	case MSG_RESP_OK: return "MSG_RESP_OK";
	case MSG_RESP_NOT_FOUND: return "MSG_RESP_NOT_FOUND";
	case MSG_RESP_IDENTITY_CONFLICT: return "MSG_RESP_IDENTITY_CONFLICT";
	case MSG_RESP_ENTRY: return "MSG_RESP_ENTRY";
	case MSG_RESP_ENTRY_LIST: return "MSG_RESP_ENTRY_LIST";
	case MSG_RESP_CMD_RESULT: return "MSG_RESP_CMD_RESULT";
	case MSG_RESP_ERROR: return "MSG_RESP_ERROR";
	default: return "UNKNOWN";
	}
}

static void printLeader(const std::vector<uint8_t>& p)
{
	size_t off = 0;
	uint32_t ip = 0; uint16_t port = 0;
	if(p.size() >= 6)
	{
		Wire::getU32((const char*)p.data(), p.size(), off, ip);
		Wire::getU16((const char*)p.data(), p.size(), off, port);
	}
	struct in_addr a; a.s_addr = ip;
	std::printf("LEADER=%s:%u\n", inet_ntoa(a), (unsigned)port);
}

static void printComponents(const std::vector<uint8_t>& p)
{
	size_t off = 0;
	uint16_t count = 0;
	if(!Wire::getU16((const char*)p.data(), p.size(), off, count)) return;
	std::printf("COUNT=%u\n", (unsigned)count);
	for(int i = 0; i < count; ++i)
	{
		ComponentData c;
		if(!decodeComponentData((const char*)p.data(), p.size(), off, c)) break;
		struct in_addr a; a.s_addr = c.intaddr;
		std::printf("  #%d uid=%d type=%d cid=%llu pid=%u gus=%u state=%d int=%s:%u\n",
			i, c.uid, c.componentType, (unsigned long long)c.componentID,
			(unsigned)c.pid, (unsigned)c.gus, (int)c.state,
			inet_ntoa(a), (unsigned)c.intport);
	}
}

// 解析响应并打印机读结果; 返回业务成功与否由调用方按命令语义判断
static bool describeResp(uint8_t type, const std::vector<uint8_t>& p)
{
	std::printf("RESP=%s\n", typeName(type));
	switch(type)
	{
	case MSG_RESP_LEADER:
	case MSG_RESP_NOT_LEADER:
		printLeader(p); break;
	case MSG_RESP_ENTRY:
	{
		size_t off = 0;
		ComponentData c;
		if(decodeComponentData((const char*)p.data(), p.size(), off, c))
			std::printf("CID=%llu\n", (unsigned long long)c.componentID);
		break;
	}
	case MSG_RESP_ENTRY_LIST:
		printComponents(p); break;
	case MSG_RESP_IDENTITY_CONFLICT:
	{
		size_t off = 0;
		ComponentData c;
		if(decodeComponentData((const char*)p.data(), p.size(), off, c))
			std::printf("CONFLICT_WITH cid=%llu pid=%u\n",
				(unsigned long long)c.componentID, (unsigned)c.pid);
		break;
	}
	case MSG_RESP_CMD_RESULT:
	{
		size_t off = 0;
		int32_t code = 0; std::string m;
		Wire::getI32((const char*)p.data(), p.size(), off, code);
		Wire::getString((const char*)p.data(), p.size(), off, m);
		std::printf("CODE=%d\n", (int)code);
		std::printf("MSG=%s\n", m.c_str());
		break;
	}
	case MSG_RESP_ERROR:
	{
		size_t off = 0;
		std::string m;
		Wire::getString((const char*)p.data(), p.size(), off, m);
		std::printf("MSG=%s\n", m.c_str());
		break;
	}
	default:
		break;
	}
	return true;
}

// -------------------------------------------------------------------------------------
static int usage(const char* argv0)
{
	std::printf("usage: %s <host> <port> <cmd> [args] [--timeout=MS]\n"
		"  cmd: leader\n"
		"       | reg <uid> <type> <cid> <pid> [gus]\n"
		"       | renew <uid> <type> <cid> <pid>\n"
		"       | unreg <uid> <type> <cid>\n"
		"       | find <uid> <type> [cid]\n"
		"       | queryall <uid>\n"
		"       | ctl <start|stop|kill> <uid> <type> <cid> <gus> [targetHost]\n", argv0);
	return 3;
}

int main(int argc, char** argv)
{
	if(argc < 4) return usage(argv[0]);

	// 扫描 --timeout=MS
	int timeoutMS = 8000;
	std::vector<std::string> args;
	for(int i = 3; i < argc; ++i)
	{
		if(std::strncmp(argv[i], "--timeout=", 10) == 0)
			timeoutMS = std::atoi(argv[i] + 10);
		else
			args.push_back(argv[i]);
	}
	if(timeoutMS <= 0) timeoutMS = 8000;

	const char* host = argv[1];
	uint16_t port = (uint16_t)std::atoi(argv[2]);
	if(args.empty()) return usage(argv[0]);

	const char* cmd = args[0].c_str();
	std::vector<uint8_t> resp;
	long long t0 = nowMS();

	uint8_t mtype = 0;
	std::string payload;

	if(std::strcmp(cmd, "leader") == 0)
	{
		mtype = MSG_QUERY_LEADER;
	}
	else if(std::strcmp(cmd, "queryall") == 0)
	{
		if(args.size() < 2) return usage(argv[0]);
		mtype = MSG_QUERY_ALL;
		Wire::putI32(payload, std::atoi(args[1].c_str()));
	}
	else if(std::strcmp(cmd, "reg") == 0)
	{
		if(args.size() < 5) return usage(argv[0]);
		mtype = MSG_REGISTER;
		ComponentData c;
		c.uid = std::atoi(args[1].c_str());
		c.componentType = std::atoi(args[2].c_str());
		c.componentID = (uint64_t)std::atoll(args[3].c_str());
		c.componentIDEx = c.componentID;
		c.pid = (uint32_t)std::atoi(args[4].c_str());
		c.gus = (args.size() >= 6) ? (uint16_t)std::atoi(args[5].c_str()) : 0;
		c.intaddr = htonl(::inet_addr(host));	// 组件内网地址=所连 cluster 副本主机
		c.intport = (uint16_t)(10000 + c.componentType);
		c.username = "tester";
		c.state = 1;
		c.cpu = 0.1f; c.mem = 0.2f; c.usedmem = 100;
		payload = encodeComponentData(c);
	}
	else if(std::strcmp(cmd, "renew") == 0)
	{
		if(args.size() < 5) return usage(argv[0]);
		mtype = MSG_RENEW;
		Wire::putI32(payload, std::atoi(args[1].c_str()));
		Wire::putI32(payload, std::atoi(args[2].c_str()));
		Wire::putU64(payload, (uint64_t)std::atoll(args[3].c_str()));
		Wire::putU32(payload, (uint32_t)std::atoi(args[4].c_str()));
	}
	else if(std::strcmp(cmd, "unreg") == 0)
	{
		if(args.size() < 4) return usage(argv[0]);
		mtype = MSG_UNREGISTER;
		Wire::putI32(payload, std::atoi(args[1].c_str()));
		Wire::putI32(payload, std::atoi(args[2].c_str()));
		Wire::putU64(payload, (uint64_t)std::atoll(args[3].c_str()));
	}
	else if(std::strcmp(cmd, "find") == 0)
	{
		if(args.size() < 3) return usage(argv[0]);
		mtype = MSG_FIND;
		Wire::putI32(payload, std::atoi(args[1].c_str()));
		Wire::putI32(payload, std::atoi(args[2].c_str()));
		Wire::putU64(payload, (args.size() >= 4) ? (uint64_t)std::atoll(args[3].c_str()) : 0);
	}
	else if(std::strcmp(cmd, "ctl") == 0)
	{
		if(args.size() < 6) return usage(argv[0]);
		const char* op = args[1].c_str();
		if(std::strcmp(op, "start") == 0)      mtype = MSG_START_SERVER;
		else if(std::strcmp(op, "stop") == 0)  mtype = MSG_STOP_SERVER;
		else if(std::strcmp(op, "kill") == 0)  mtype = MSG_KILL_SERVER;
		else return usage(argv[0]);

		ServerCommandData sc;
		sc.uid = std::atoi(args[2].c_str());
		sc.username = "tester";
		sc.componentType = std::atoi(args[3].c_str());
		sc.cid = (uint64_t)std::atoll(args[4].c_str());
		sc.gus = (uint16_t)std::atoi(args[5].c_str());
		sc.targetHost = (args.size() >= 7) ? args[6] : "";
		payload = encodeServerCommand(sc);
	}
	else
	{
		return usage(argv[0]);
	}

	std::printf("[cmd] %s %s:%u\n", cmd, host, (unsigned)port);
	if(!roundtrip(host, port, mtype, payload, resp, timeoutMS))
	{
		std::printf("RESP=TIMEOUT_OR_CLOSED detail=no_response_within_%dms\n", timeoutMS);
		return 1;
	}

	uint8_t rtype = resp[0];
	std::vector<uint8_t> rp(resp.begin() + 1, resp.end());
	describeResp(rtype, rp);
	std::printf("ELAPSED_MS=%lld\n", nowMS() - t0);

	// 业务语义成功判定
	bool ok = false;
	switch(mtype)
	{
	case MSG_QUERY_LEADER: ok = (rtype == MSG_RESP_LEADER); break;
	case MSG_REGISTER:     ok = (rtype == MSG_RESP_OK); break;
	case MSG_RENEW:        ok = (rtype == MSG_RESP_OK); break;
	case MSG_UNREGISTER:   ok = (rtype == MSG_RESP_OK); break;
	case MSG_FIND:
		if(rtype == MSG_RESP_ENTRY) ok = true;
		else if(rtype == MSG_RESP_ENTRY_LIST)
		{
			// 按类型查找: 空结果也是合法列表(COUNT=0), 视为成功
			size_t o = 0; uint16_t cnt = 0;
			Wire::getU16((const char*)rp.data(), rp.size(), o, cnt);
			ok = true; (void)cnt;
		}
		break;
	case MSG_QUERY_ALL:    ok = (rtype == MSG_RESP_ENTRY_LIST); break;
	case MSG_START_SERVER:
	case MSG_STOP_SERVER:
	case MSG_KILL_SERVER:
	{
		if(rtype == MSG_RESP_CMD_RESULT)
		{
			size_t o = 0; int32_t code = -1;
			Wire::getI32((const char*)rp.data(), rp.size(), o, code);
			ok = (code == 0);
		}
		break;
	}
	default: break;
	}

	return ok ? 0 : 2;
}
