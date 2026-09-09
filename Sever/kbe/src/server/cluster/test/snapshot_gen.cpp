// Copyright 2008-2025 Yolo Technologies, Inc. All Rights Reserved. https://www.comblockengine.com
/*
	snapshot_gen — cluster 状态快照文件生成器(测试工具)。

	用途: 构造确定性的 cluster_state_<idx>.bin 文件, 用于回归验证
	      ClusterCore::loadSnapshotFromDisk() 对文件头 magic 的处理
	      (曾出现 4 字节偏移 bug: decodeSnapshot 从 magic 起始解析导致注册表为空)。

	文件布局(与 cluster_core.cpp 的 saveSnapshot/loadSnapshotFromDisk 完全一致):
	    [u32 magic=0x4B42E533]
	    其余为 registrySnapshotBytes:
	        [u64 snapshotIndex][u64 snapshotTerm][u64 commitIndex][u64 currentTerm]
	        [u32 count]
	        每个注册条目: [string key=uid(4B)+type(4B)+cid(8B) 大端]
	                     [string encodeComponentData(ComponentData)]
	                     [u64 lastRenewal(epoch ms)]

	用法:
	    snapshot_gen <outfile> <uid> <type> <cid> <pid> [<uid> <type> <cid> <pid> ...]

	示例(生成含 2 条注册的 state 文件):
	    snapshot_gen cluster_state_0.bin 1 6 42001 1001 1 5 42002 1002
*/
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <fstream>
#include <string>

#include "cluster_interface.h"

#ifdef _WIN32
	#include <winsock2.h>
	#pragma comment(lib, "ws2_32.lib")
	#ifndef htonl
		#define htonl(x) (((x & 0xFF) << 24) | ((x & 0xFF00) << 8) | ((x & 0xFF0000) >> 8) | ((x >> 24) & 0xFF))
	#endif
#else
	#include <arpa/inet.h>
#endif

using namespace KBEngine::ClusterInterface;

static uint64_t nowEpochMS()
{
	return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::system_clock::now().time_since_epoch()).count();
}

int main(int argc, char** argv)
{
	if(argc < 5 || (argc - 2) % 4 != 0)
	{
		std::printf("usage: %s <outfile> <uid> <type> <cid> <pid> [uid type cid pid]...\n", argv[0]);
		return 1;
	}

	const char* outFile = argv[1];
	int count = (argc - 2) / 4;
	uint64_t now = nowEpochMS();

	std::string body;
	Wire::putU64(body, (uint64_t)count);	// snapshotIndex(测试值: =条数)
	Wire::putU64(body, 1);					// snapshotTerm
	Wire::putU64(body, (uint64_t)count);	// commitIndex
	Wire::putU64(body, 1);					// currentTerm
	Wire::putU32(body, (uint32_t)count);	// 条目数

	for(int i = 0; i < count; ++i)
	{
		int base = 2 + i * 4;
		int32_t uid = std::atoi(argv[base]);
		int32_t type = std::atoi(argv[base + 1]);
		uint64_t cid = (uint64_t)std::atoll(argv[base + 2]);
		uint32_t pid = (uint32_t)std::atoi(argv[base + 3]);

		// 注册表 key = encodeKey(uid,type,cid)
		std::string key;
		Wire::putU32(key, (uint32_t)uid);
		Wire::putU32(key, (uint32_t)type);
		Wire::putU64(key, cid);

		ComponentData c;
		c.uid = uid;
		c.username = "tester";
		c.componentType = type;
		c.componentID = cid;
		c.componentIDEx = cid;
		c.pid = pid;
		c.gus = 0;
		c.intaddr = htonl(0x7F000001);		// 127.0.0.1
		c.intport = (uint16_t)(10000 + type);
		c.state = 1;
		c.cpu = 0.05f; c.mem = 0.1f; c.usedmem = 64;
		std::string data = encodeComponentData(c);

		Wire::putString(body, key);
		Wire::putString(body, data);
		Wire::putU64(body, now);			// lastRenewal = 当前时间, 避免一启动即被 TTL 清理
	}

	std::string out;
	Wire::putU32(out, 0x4B42E533);			// magic 头
	out += body;

	std::ofstream fout(outFile, std::ios::binary | std::ios::trunc);
	if(!fout.is_open())
	{
		std::printf("[error] cannot open %s for write\n", outFile);
		return 1;
	}
	fout.write(out.c_str(), (std::streamsize)out.size());
	fout.close();

	std::printf("wrote %s: %d entries, %d bytes\n", outFile, count, (int)out.size());
	return 0;
}
