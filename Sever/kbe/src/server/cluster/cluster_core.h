// Copyright 2008-2018 Yolo Technologies, Inc. All Rights Reserved. https://www.comblockengine.com

/*
	cluster 一致性核心：
	  - 极简 Raft 子集(选举/心跳/日志复制/majority提交/快照同步)
	  - 注册表纯内存状态机(写入走 leader 日志, 组件续租 TTL 由 leader 生成 Expire 命令)
	  - servicePort/peerPort 两个 TCP 监听 + 网络线程(消息入队, 主循环 tick 统一推进)
	参考: docs/kbe_cluster_design.md
*/

#ifndef KBE_CLUSTER_CORE_H
#define KBE_CLUSTER_CORE_H

#include "cluster_interface.h"
#include "network/endpoint.h"

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace KBEngine {

// 日志(复制)项
struct RaftLogEntry
{
	uint64 index;		// 从 1 开始
	uint64 term;
	uint8  type;		// ClusterCore::CmdType
	std::string data;	// Wire 编码的命令载荷(含命令时间戳)
};

// 注册表条目
struct RegistryEntry
{
	ClusterInterface::ComponentData data;
	uint64 lastRenewal;		// leader 时钟(epoch ms)
};

class ClusterCore
{
public:
	enum Role : uint8_t
	{
		ROLE_FOLLOWER = 0,
		ROLE_CANDIDATE,
		ROLE_LEADER
	};

	// 写入日志的命令类型(状态机唯一事实来源)
	enum CmdType : uint8_t
	{
		CMD_REGISTER	= 1,
		CMD_RENEW		= 2,
		CMD_UNREGISTER	= 3,
		CMD_EXPIRE		= 4
	};

	ClusterCore(const std::string& selfIP, int selfIdx,
		const std::vector<std::string>& members,
		uint16 servicePort, uint16 peerPort,
		uint32 electionTimeoutMS, uint32 heartbeatIntervalMS,
		uint32 componentHeartbeatInterval, uint32 componentLeaseSeconds);

	~ClusterCore();

	/** 绑定 service/peer 两个监听端口并启动网络线程 */
	bool start();
	void stop();
	/** 主循环(ServerApp 帧)推进：处理入队事件/选举/心跳/日志/TTL/快照 */
	void tick();

	// ---- 供 Cluster 调用的查询(服务面读请求可由 Cluster 直接转 core) ----
	uint64 nowMS() const { return lastTickMS_; }

protected:
	void loadSnapshotFromDisk();
	void saveSnapshot();
	void saveSnapshot(const std::string& bytes);

	// ---- 事件(网络线程投递, 主循环消费) ----
	enum EventType : uint8_t
	{
		EVENT_SERVICE_REQ	= 1,	// data: 服务面原始请求(含消息类型)
		EVENT_PEER_MSG		= 2		// data: 对端消息; peerIdx: 来源成员
	};

	struct Event
	{
		uint8_t type;
		uint64 connId;			// EVENT_SERVICE_REQ 的连接ID
		int peerIdx;			// EVENT_PEER_MSG 的来源成员
		std::string data;
	};

	// ---- 网络线程入口 ----
	void serviceAcceptorLoop();
	void peerAcceptorLoop();
	void peerConnectorLoop();
	void serviceConnThread(Network::EndPoint* ep, uint64 connId);
	void peerConnThread(Network::EndPoint* ep, int peerIdx, bool outbound);

	// ---- 主循环内部 ----
	void drainEvents();
	void handleServiceRequest(const Event& ev);
	void handlePeerMessage(const Event& ev);
	void advanceCommit();
	void applyCommitted();
	void ttlScan();
	bool isLeader() const { return role_ == ROLE_LEADER; }

	// 日志操作(仅主循环/leader)
	uint64 lastIndex() const;
	uint64 appendLog(uint8_t type, const std::string& payload);
	void registerWaiter(uint64 index, uint64 connId);

	// 对端收发
	void sendFrameToPeer(int idx, const std::string& msg);
	void sendReplicateToPeer(int idx);

	void onVoteRequest(int from, const std::string& d, size_t off);
	void onVoteResponse(int from, const std::string& d, size_t off);
	void onAppend(int from, const std::string& d, size_t off);
	void onAppendResponse(int from, const std::string& d, size_t off);

	// ---- 状态机(仅主循环调用) ----
	void applyEntry(const RaftLogEntry& e);

	// ---- 服务面回复 ----
	void redirectNotLeader(uint64 connId);

	// 远程起停: op 为 MSG_START_SERVER/MSG_STOP_SERVER/MSG_KILL_SERVER
	// localOnly=true 时表示收到 leader 的代理指令(MSG_PROXY_*), 只在本机执行并直接应答。
	void handleCtlCommand(const Event& ev, uint8_t op, size_t off, bool localOnly);

	// ---- 序列化 ----
	static std::string encodeKey(int32 uid, int32 type, uint64 cid);
	std::string registrySnapshotBytes() const;
	void decodeSnapshot(const std::string& bytes);

protected:
	// 网络线程变量(生命周期管理)
	std::vector<std::thread> threads_;
	std::mutex threadsMutex_;		// 保护 threads_ 的并发 push
	std::atomic<bool> running_;

	// 安全地注册一个网络线程(停止阶段已开始退出则返回false)
	bool spawn(const std::function<void()>& fn);

	// 监听端点(主线程创建, acceptor 线程使用)
	Network::EndPoint* serviceListenEP_;
	Network::EndPoint* peerListenEP_;

	// ---- 配置 ----
	std::string selfIP_;
	int selfIdx_;
	std::vector<std::string> members_;
	uint16 servicePort_;
	uint16 peerPort_;
	uint32 electionTimeoutMS_;
	uint32 heartbeatIntervalMS_;
	uint32 componentHeartbeatIntervalMS_;	// 秒转为毫秒
	uint32 componentLeaseMS_;				// 秒转为毫秒

	// ---- Raft 状态(主循环独占) ----
	Role role_;
	uint64 currentTerm_;
	int votedFor_;			// -1 未投
	int leaderIdx_;			// -1 未知
	uint64 commitIndex_;
	uint64 lastApplied_;
	uint64 snapshotIndex_;
	uint64 snapshotTerm_;

	std::vector<RaftLogEntry> log_;			// 覆盖 (snapshotIndex_, lastIndex_]

	// ---- 选举/心跳计时(ms) ----
	uint64 electionDeadlineMS_;
	uint64 lastHeartbeatMS_;
	uint64 lastTTLScanMS_;
	uint64 lastSnapshotMS_;
	uint64 lastTickMS_;

	// ---- 注册表状态机 ----
	std::map<std::string, RegistryEntry> registry_;

	// ---- 对端状态(每成员) ----
	struct PeerCtx
	{
		bool connected;
		bool outbound;			// 本方是否为主动连接方(index小端)
		Network::EndPoint* ep;
		uint64 lastAcked;		// 已确认连续日志 index
		bool needSnapshot;
	};
	std::vector<PeerCtx> peers_;
	std::mutex peersMutex_;		// 保护 peers_ 中 connected/ep 的读写

	// ---- 事件入队(网络线程) ----
	std::mutex queueMutex_;
	std::vector<Event> queue_;

	// ---- 服务连接等待者/应答 ----
	std::mutex replyMutex_;
	std::condition_variable replyCv_;
	std::map<uint64, std::string> replies_;			// connId -> 响应字节
	std::map<uint64, uint64> cmdWaiter_;			// 日志index -> connId(等待提交)
	std::map<uint64, uint64> revWaiter_;			// connId -> 日志index(等待提交)
	std::map<int, bool> grantVotes_;				// 本 term 内已同意给票的成员(仅 candidate)
	std::atomic<uint64> nextConnId_;
};

}

#endif // KBE_CLUSTER_CORE_H
