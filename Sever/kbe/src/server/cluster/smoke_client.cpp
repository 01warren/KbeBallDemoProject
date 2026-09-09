// Copyright 2008-2025 Yolo Technologies, Inc. All Rights Reserved. https://www.comblockengine.com
/*
	cluster 服务面协议冒烟客户端（验证/演示工具）。

	仅依赖自包含的 cluster_interface.h 与系统 socket，无需链接引擎库。
	用法(Windows):
	    cl /nologo /EHsc /I<kbe src> smoke_client.cpp /link ws2_32.lib
	    或 MSBuild/Visual Studio 开发者命令行直接 cl。

	命令:
	    smoke_client <host> <port> leader
	    smoke_client <host> <port> reg <uid> <type> <cid> <pid> [gus]
	    smoke_client <host> <port> renew <uid> <type> <cid> <pid>
	    smoke_client <host> <port> unreg <uid> <type> <cid>
	    smoke_client <host> <port> find <uid> <type> [cid]
	    smoke_client <host> <port> queryall <uid>

	响应: 打印收到的消息类型并解析(leader 地址 / 注册结果 / 组件列表)。
	非 leader 副本会返回 MSG_RESP_NOT_LEADER(含 leader 地址)，用于故障切换观察。
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
	WSADATA wsa;
	WSAStartup(MAKEWORD(2, 2), &wsa);
#endif

	uint32_t addr = ::inet_addr(host);
	if(addr == INADDR_NONE)
	{
		// hostname 解析
		struct hostent* he = ::gethostbyname(host);
		if(!he) { std::printf("[error] cannot resolve host %s\n", host); return INVALID_SOCKET; }
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
		std::printf("[error] connect %s:%u failed\n", host, (unsigned)port);
		closesocket(s);
		return INVALID_SOCKET;
	}
	return s;
}

// -------------------------------------------------------------------------------------
// 发送一条消息并读取(至连接关闭), 返回 [type][payload] 原字节。
// 服务端帧格式: [u32 big-endian 长度][type][payload], 请求与响应均如此。
static bool roundtrip(const char* host, uint16_t port, uint8_t type,
	const std::string& payload, std::vector<uint8_t>& resp)
{
	SOCKET s = connectTo(host, port);
	if(s == INVALID_SOCKET) return false;

	std::string msg = encodeMessage(type, payload);
	std::string frame;
	Wire::putU32(frame, (uint32_t)msg.size());	// 长度前缀(网络字节序)
	frame += msg;

	int sent = 0;
	while(sent < (int)frame.size())
	{
		int n = ::send(s, frame.data() + sent, (int)frame.size() - sent, 0);
		if(n <= 0)
		{
			std::printf("[error] send failed (sent %d/%d)\n", sent, (int)frame.size());
			closesocket(s);
			return false;
		}
		sent += n;
	}

	resp.clear();
	char buf[4096];
	for(;;)
	{
		int n = ::recv(s, buf, sizeof(buf), 0);
		if(n <= 0) break;
		resp.insert(resp.end(), buf, buf + n);
	}
	closesocket(s);

	// 剥掉响应长度前缀后交由调用方按 [type][payload] 解析
	if(resp.size() >= 4)
		resp.erase(resp.begin(), resp.begin() + 4);
	return !resp.empty();
}

// -------------------------------------------------------------------------------------
static void printLeaderPayload(const std::vector<uint8_t>& p)
{
	size_t off = 0;
	uint32_t ip = 0; uint16_t port = 0;
	if(p.size() >= 6)
	{
		Wire::getU32((const char*)p.data(), p.size(), off, ip);
		Wire::getU16((const char*)p.data(), p.size(), off, port);
	}
	struct in_addr a; a.s_addr = ip;
	std::printf("    leader=%s:%u\n", inet_ntoa(a), (unsigned)port);
}

static void printComponents(const std::vector<uint8_t>& p)
{
	// p 即 MSG_RESP_ENTRY_LIST 的载荷: [u16 count][ver+data 组件]...
	size_t off = 0;
	uint16_t count = 0;
	if(!Wire::getU16((const char*)p.data(), p.size(), off, count)) return;
	std::printf("    count=%u\n", (unsigned)count);
	for(int i = 0; i < count; ++i)
	{
		ComponentData c;
		if(!decodeComponentData((const char*)p.data(), p.size(), off, c)) break;
		std::printf("      #%d uid=%d type=%d cid=%llu pid=%u gus=%u state=%d int=%s:%u\n",
			i, c.uid, c.componentType, (unsigned long long)c.componentID,
			(unsigned)c.pid, (unsigned)c.gus, (int)c.state,
			inet_ntoa(*(struct in_addr*)&c.intaddr), (unsigned)c.intport);
	}
}

static void describeResp(uint8_t type, const std::vector<uint8_t>& p)
{
	switch(type)
	{
	case MSG_RESP_LEADER:
		std::printf("    [MSG_RESP_LEADER]\n"); printLeaderPayload(p); break;
	case MSG_RESP_NOT_LEADER:
		std::printf("    [MSG_RESP_NOT_LEADER]\n"); printLeaderPayload(p); break;
	case MSG_RESP_OK:
		std::printf("    [MSG_RESP_OK]\n"); break;
	case MSG_RESP_NOT_FOUND:
		std::printf("    [MSG_RESP_NOT_FOUND]\n"); break;
	case MSG_RESP_IDENTITY_CONFLICT:
		std::printf("    [MSG_RESP_IDENTITY_CONFLICT]\n"); break;
	case MSG_RESP_ENTRY:
		std::printf("    [MSG_RESP_ENTRY]\n"); break;
	case MSG_RESP_ENTRY_LIST:
		std::printf("    [MSG_RESP_ENTRY_LIST]\n"); printComponents(p); break;
	case MSG_RESP_CMD_RESULT:
	{
		size_t off = 0;
		int32_t code = 0; std::string m;
		Wire::getI32((const char*)p.data(), p.size(), off, code);
		Wire::getString((const char*)p.data(), p.size(), off, m);
		std::printf("    [MSG_RESP_CMD_RESULT] code=%d msg=%s\n", code, m.c_str());
		break;
	}
	case MSG_RESP_ERROR:
	{
		size_t off = 0;
		std::string m;
		Wire::getString((const char*)p.data(), p.size(), off, m);
		std::printf("    [MSG_RESP_ERROR] %s\n", m.c_str());
		break;
	}
	default:
		std::printf("    [type=0x%02X size=%d]\n", type, (int)p.size()); break;
	}
}

// -------------------------------------------------------------------------------------
static int usage(const char* argv0)
{
	std::printf("usage: %s <host> <port> <cmd> [args]\n"
		"  cmd: leader | reg <uid> <type> <cid> <pid> [gus]\n"
		"       | renew <uid> <type> <cid> <pid> | unreg <uid> <type> <cid>\n"
		"       | find <uid> <type> [cid] | queryall <uid>\n", argv0);
	return 1;
}

int main(int argc, char** argv)
{
	if(argc < 4) return usage(argv[0]);

	const char* host = argv[1];
	uint16_t port = (uint16_t)std::atoi(argv[2]);
	const char* cmd = argv[3];
	std::vector<uint8_t> resp;

	int need = 0;
	if(std::strcmp(cmd, "leader") == 0) need = 4;
	else if(std::strcmp(cmd, "reg") == 0) need = 8;
	else if(std::strcmp(cmd, "renew") == 0) need = 8;
	else if(std::strcmp(cmd, "unreg") == 0) need = 7;
	else if(std::strcmp(cmd, "find") == 0) need = 6;
	else if(std::strcmp(cmd, "queryall") == 0) need = 5;
	else return usage(argv[0]);
	if(argc < need) return usage(argv[0]);

	uint8_t mtype = 0;
	std::string payload;

	if(std::strcmp(cmd, "leader") == 0)
	{
		mtype = MSG_QUERY_LEADER;
	}
	else if(std::strcmp(cmd, "queryall") == 0)
	{
		mtype = MSG_QUERY_ALL;
		Wire::putI32(payload, std::atoi(argv[4]));
	}
	else if(std::strcmp(cmd, "reg") == 0)
	{
		mtype = MSG_REGISTER;
		ComponentData c;
		c.uid = std::atoi(argv[4]);
		c.componentType = std::atoi(argv[5]);
		c.componentID = (uint64_t)std::atoll(argv[6]);
		c.componentIDEx = c.componentID;
		c.pid = (uint32_t)std::atoi(argv[7]);
		c.gus = (argc >= 9) ? (uint16_t)std::atoi(argv[8]) : 0;
		c.intaddr = htonl(inet_addr(host)); // 模拟组件内网地址
		c.intport = (uint16_t)(10000 + c.componentType);
		c.username = "smoke";
		c.state = 1;
		c.cpu = 0.1f; c.mem = 0.2f; c.usedmem = 100;
		payload = encodeComponentData(c);
	}
	else if(std::strcmp(cmd, "renew") == 0)
	{
		mtype = MSG_RENEW;
		Wire::putI32(payload, std::atoi(argv[4]));
		Wire::putI32(payload, std::atoi(argv[5]));
		Wire::putU64(payload, (uint64_t)std::atoll(argv[6]));
		Wire::putU32(payload, (uint32_t)std::atoi(argv[7]));
	}
	else if(std::strcmp(cmd, "unreg") == 0)
	{
		mtype = MSG_UNREGISTER;
		Wire::putI32(payload, std::atoi(argv[4]));
		Wire::putI32(payload, std::atoi(argv[5]));
		Wire::putU64(payload, (uint64_t)std::atoll(argv[6]));
	}
	else if(std::strcmp(cmd, "find") == 0)
	{
		mtype = MSG_FIND;
		Wire::putI32(payload, std::atoi(argv[4]));
		Wire::putI32(payload, std::atoi(argv[5]));
		Wire::putU64(payload, (argc >= 7) ? (uint64_t)std::atoll(argv[6]) : 0);
	}

	std::printf("[send] %s %s:%u\n", cmd, host, (unsigned)port);
	if(!roundtrip(host, port, mtype, payload, resp))
	{
		std::printf("[error] no response\n");
		return 1;
	}
	uint8_t rtype = resp[0];
	std::vector<uint8_t> rp(resp.begin() + 1, resp.end());
	describeResp(rtype, rp);

	// 业务语义成功判定
	switch(mtype)
	{
	case MSG_QUERY_LEADER: return (rtype == MSG_RESP_LEADER) ? 0 : 2;
	case MSG_REGISTER:     return (rtype == MSG_RESP_OK) ? 0 : 2;
	case MSG_RENEW:        return (rtype == MSG_RESP_OK) ? 0 : 2;
	case MSG_UNREGISTER:   return (rtype == MSG_RESP_OK) ? 0 : 2;
	case MSG_FIND:
		return (rtype == MSG_RESP_ENTRY || rtype == MSG_RESP_ENTRY_LIST) ? 0 : 2;
	case MSG_QUERY_ALL:    return (rtype == MSG_RESP_ENTRY_LIST) ? 0 : 2;
	default: return 0;
	}
}
