// Copyright 2008-2018 Yolo Technologies, Inc. All Rights Reserved. https://www.comblockengine.com

/*
	cluster 一致性核心实现（详见 docs/kbe_cluster_design.md 第 4/5/6 节）

	- 一致性：极简 Raft 子集（选举/心跳/日志复制/majority 提交/快照追平）
	- 状态机：注册表纯内存, 所有变更以日志命令顺序 apply
	- 线程模型：网络线程只负责收/发并投递事件, 状态机与日志仅在主循环 tick 中推进
*/

#include "cluster.h"
#include "cluster_core.h"
#include "cluster_interface.h"
#include "cluster_process.h"

#if KBE_PLATFORM == PLATFORM_WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#endif

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <algorithm>

namespace KBEngine {

// 与对端(副本)之间传输的帧: u32(len) + [u8 消息类型][payload]
namespace PeerProto
{
	enum MsgType : uint8_t
	{
		PM_VOTE_REQ		= 0x01,
		PM_VOTE_RESP	= 0x02,
		PM_APPEND		= 0x03,	// kind=0 增量日志 / kind=1 快照
		PM_APPEND_RESP	= 0x04
	};
}

// 帧最大长度(1MB)
static const uint32 FRAME_MAX = 1024 * 1024;

//-------------------------------------------------------------------------------------
static uint64 nowWallMS()
{
	return (uint64)std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::system_clock::now().time_since_epoch()).count();
}

//-------------------------------------------------------------------------------------
// leader -> 指定主机副本(本机代理) 的短连接同步转发。
// 帧格式与 lib/server/components.cpp 的 clusterRoundTrip 一致: u32(len) + [u8 type][payload]。
// EndPoint::connect(networkPort, ...) 要求端口为网络字节序, 这里显式 htons。
static bool clusterCtlSelectWait(const Network::EndPoint& ep, bool writable, uint32 timeoutMS)
{
	fd_set fds;
	FD_ZERO(&fds);
	FD_SET((int)ep, &fds);

	struct timeval tv;
	tv.tv_sec = (long)(timeoutMS / 1000);
	tv.tv_usec = (long)(timeoutMS % 1000) * 1000;

	if(writable)
		return select((int)ep + 1, NULL, &fds, NULL, &tv) > 0;

	return select((int)ep + 1, &fds, NULL, NULL, &tv) > 0;
}

static int clusterCtlRoundTrip(const std::string& requestMsg, std::string& response,
	uint32 ip, uint16 servicePort, uint32 timeoutMS)
{
	Network::EndPoint ep;
	ep.socket(SOCK_STREAM);
	if(!ep.good())
		return -1;

	ep.setnonblocking(true);

	if(ep.connect(htons(servicePort), ip) == -1)
	{
		if(!clusterCtlSelectWait(ep, true, timeoutMS))
		{
			ep.close();
			return -1;
		}
	}

	ep.setnodelay(true);

	std::string frame;
	ClusterInterface::Wire::putU32(frame, (uint32_t)requestMsg.size());
	frame += requestMsg;

	size_t sent = 0;
	while(sent < frame.size())
	{
		int n = ep.send(frame.c_str() + sent, (int)(frame.size() - sent));
		if(n > 0)
		{
			sent += (size_t)n;
			continue;
		}

		if(n < 0)
		{
			if(!clusterCtlSelectWait(ep, true, 50))
			{
				ep.close();
				return -1;
			}
			continue;
		}

		ep.close();
		return -1;
	}

	char lenbuf[4];
	size_t got = 0;
	while(got < 4)
	{
		int n = ep.recv(lenbuf + got, 4 - (int)got);
		if(n > 0) { got += (size_t)n; continue; }
		if(n == 0) { ep.close(); return -1; }
		if(!clusterCtlSelectWait(ep, false, 100)) { ep.close(); return -1; }
	}

	uint32_t plen = ((uint32_t)(uint8_t)lenbuf[0] << 24) | ((uint32_t)(uint8_t)lenbuf[1] << 16) |
		((uint32_t)(uint8_t)lenbuf[2] << 8) | (uint32_t)(uint8_t)lenbuf[3];
	if(plen == 0 || plen > FRAME_MAX)
	{
		ep.close();
		return -1;
	}

	response.assign(plen, '\0');
	got = 0;
	while(got < plen)
	{
		int n = ep.recv(&response[0] + got, (int)(plen - got));
		if(n > 0) { got += (size_t)n; continue; }
		if(n == 0) { ep.close(); return -1; }
		if(!clusterCtlSelectWait(ep, false, 100)) { ep.close(); return -1; }
	}

	ep.close();
	return 0;
}

// 单跳直达指定副本(不做 leader 重定向); 成功且 resp 非空返回0。
static int clusterCtlRequest(const std::string& msg, std::string& resp,
	uint32 ip, uint16 servicePort, uint32 timeoutMS)
{
	resp.clear();
	if(clusterCtlRoundTrip(msg, resp, ip, servicePort, timeoutMS) != 0)
		return -1;

	if(resp.size() < 1)
		return -1;

	return 0;
}

//-------------------------------------------------------------------------------------
// 简单 xorshift64 随机（选举超时抖动用）
static uint64 g_randState = 0x9E3779B97F4A7C15ULL;

static uint64 fastRand()
{
	uint64 x = g_randState;
	x ^= x << 13;
	x ^= x >> 7;
	x ^= x << 17;
	g_randState = x;
	return x;
}

//-------------------------------------------------------------------------------------
static bool sendAll(Network::EndPoint* ep, const char* data, int len)
{
	int sent = 0;
	while(sent < len)
	{
		int n = ep->send(data + sent, len - sent);
		if(n <= 0)
			return false;
		sent += n;
	}
	return true;
}

//-------------------------------------------------------------------------------------
static bool recvFrame(Network::EndPoint* ep, std::string& out)
{
	uint8_t hdr[4];
	if(!ep->recvAll(hdr, 4))
		return false;

	size_t off = 0;
	uint32_t len = 0;
	if(!ClusterInterface::Wire::getU32((const char*)hdr, 4, off, len))
		return false;

	if(len > FRAME_MAX || len == 0)
		return false;

	out.resize(len);
	if(!ep->recvAll(&out[0], (int)len))
		return false;

	return true;
}

//-------------------------------------------------------------------------------------
static bool sendFrame(Network::EndPoint* ep, const std::string& payload)
{
	std::string frame;
	ClusterInterface::Wire::putU32(frame, (uint32_t)payload.size());
	frame += payload;
	return sendAll(ep, frame.c_str(), (int)frame.size());
}

//-------------------------------------------------------------------------------------
ClusterCore::ClusterCore(const std::string& selfIP, int selfIdx,
		const std::vector<std::string>& members,
		uint16 servicePort, uint16 peerPort,
		uint32 electionTimeoutMS, uint32 heartbeatIntervalMS,
		uint32 componentHeartbeatInterval, uint32 componentLeaseSeconds):
	selfIP_(selfIP),
	selfIdx_(selfIdx),
	members_(members),
	servicePort_(servicePort),
	peerPort_(peerPort),
	electionTimeoutMS_(electionTimeoutMS),
	heartbeatIntervalMS_(heartbeatIntervalMS),
	componentHeartbeatIntervalMS_(componentHeartbeatInterval * 1000),
	componentLeaseMS_(componentLeaseSeconds * 1000),
	role_(ROLE_FOLLOWER),
	currentTerm_(0),
	votedFor_(-1),
	leaderIdx_(-1),
	commitIndex_(0),
	lastApplied_(0),
	snapshotIndex_(0),
	snapshotTerm_(0),
	electionDeadlineMS_(0),
	lastHeartbeatMS_(0),
	lastTTLScanMS_(0),
	lastSnapshotMS_(0),
	lastTickMS_(nowWallMS()),
	serviceListenEP_(NULL),
	peerListenEP_(NULL),
	running_(false),
	nextConnId_(1)
{
	// 选举抖动种子必须按实例差异化：常量种子会导致所有副本"随机"序列一致,
	// 多个 follower 同时到达选举 deadline 互不投票, 形成活锁(见 test/TESTCASES.md G3-9)。
	g_randState ^= (uint64)nowWallMS()
		^ ((uint64)(uintptr_t)&g_randState << 32)	// ASLR 地址, 进程间不同
		^ ((uint64)(uint64_t)selfIdx_ << 48);
	if(g_randState == 0)
		g_randState = 0x9E3779B97F4A7C15ULL;

	peers_.resize(members_.size());
	for(size_t i = 0; i < peers_.size(); ++i)
	{
		peers_[i].connected = false;
		peers_[i].outbound = (selfIdx_ >= 0 && (int)i > selfIdx_);
		peers_[i].ep = NULL;
		peers_[i].lastAcked = 0;
		peers_[i].needSnapshot = false;
	}
}

//-------------------------------------------------------------------------------------
ClusterCore::~ClusterCore()
{
	stop();
}

//-------------------------------------------------------------------------------------
void ClusterCore::stop()
{
	if(!running_.exchange(false))
		return;

	// 关闭所有端点以唤醒阻塞中的网络线程
	if(serviceListenEP_)
	{
		serviceListenEP_->close();
		serviceListenEP_ = NULL;
	}

	if(peerListenEP_)
	{
		peerListenEP_->close();
		peerListenEP_ = NULL;
	}

	{
		std::lock_guard<std::mutex> lock(peersMutex_);
		for(size_t i = 0; i < peers_.size(); ++i)
		{
			if(peers_[i].ep)
			{
				peers_[i].ep->close();
				peers_[i].ep = NULL;
				peers_[i].connected = false;
			}
		}
	}

	{
		std::lock_guard<std::mutex> lock(replyMutex_);
		replyCv_.notify_all();
	}

	// 收集所有线程句柄后释放互斥再 join(避免与网络线程的 spawn 互相阻塞)
	std::vector<std::thread> joinList;
	{
		std::lock_guard<std::mutex> lock(threadsMutex_);
		joinList.swap(threads_);
	}

	for(size_t i = 0; i < joinList.size(); ++i)
	{
		if(joinList[i].joinable())
			joinList[i].join();
	}
}

//-------------------------------------------------------------------------------------
bool ClusterCore::spawn(const std::function<void()>& fn)
{
	if(!running_.load())
		return false;

	try
	{
		std::lock_guard<std::mutex> lock(threadsMutex_);
		if(!running_.load())
			return false;

		threads_.push_back(std::thread(fn));
		return true;
	}
	catch(...)
	{
		return false;
	}
}

//-------------------------------------------------------------------------------------
bool ClusterCore::start()
{
	if(running_)
		return false;

	running_ = true;

	// 解析本机服务端 IP
	uint32_t hostAddr = ::inet_addr(selfIP_.c_str());
	if(hostAddr == (uint32_t)INADDR_NONE)
	{
		ERROR_MSG(fmt::format("ClusterCore::start(): invalid self ip: {}\n", selfIP_));
		running_ = false;
		return false;
	}

	// service 监听（组件/工具连接）
	serviceListenEP_ = Network::EndPoint::createPoolObject(OBJECTPOOL_POINT);
	serviceListenEP_->socket(SOCK_STREAM);
	if(!serviceListenEP_->good())
	{
		ERROR_MSG(fmt::format("ClusterCore::start(): failed to create service socket.\n"));
		serviceListenEP_ = NULL;
		running_ = false;
		return false;
	}

	if(serviceListenEP_->bind(htons(servicePort_), hostAddr) == -1)
	{
		ERROR_MSG(fmt::format("ClusterCore::start(): bind service port {} on {} failed.\n",
			servicePort_, selfIP_));
		serviceListenEP_->close();
		serviceListenEP_ = NULL;
		running_ = false;
		return false;
	}

	if(serviceListenEP_->listen(SOMAXCONN) == -1)
	{
		ERROR_MSG(fmt::format("ClusterCore::start(): listen service port {} failed.\n", servicePort_));
		serviceListenEP_->close();
		serviceListenEP_ = NULL;
		running_ = false;
		return false;
	}

	// peer 监听（副本之间）
	peerListenEP_ = Network::EndPoint::createPoolObject(OBJECTPOOL_POINT);
	peerListenEP_->socket(SOCK_STREAM);
	if(!peerListenEP_->good())
	{
		peerListenEP_ = NULL;
		serviceListenEP_->close();
		serviceListenEP_ = NULL;
		running_ = false;
		return false;
	}

	if(peerListenEP_->bind(htons(peerPort_), hostAddr) == -1 ||
		peerListenEP_->listen(SOMAXCONN) == -1)
	{
		ERROR_MSG(fmt::format("ClusterCore::start(): bind/listen peer port {} on {} failed.\n",
			peerPort_, selfIP_));
		peerListenEP_->close();
		peerListenEP_ = NULL;
		serviceListenEP_->close();
		serviceListenEP_ = NULL;
		running_ = false;
		return false;
	}

	// 从磁盘恢复已提交状态(注册表快照)
	loadSnapshotFromDisk();

	// 单成员部署：无需选举，直接成为 leader
	if(members_.size() == 1)
	{
		role_ = ROLE_LEADER;
		leaderIdx_ = selfIdx_;
		electionDeadlineMS_ = 0;
		lastHeartbeatMS_ = 0;
		lastTTLScanMS_ = nowWallMS();
		lastSnapshotMS_ = nowWallMS();

		INFO_MSG(fmt::format("ClusterCore::start(): single-member mode, acting as leader.\n"));
	}
	else
	{
		// 设置选举计时：启动后先等待一段时间再参与选举，给已有 leader 留出心跳时间
		electionDeadlineMS_ = nowWallMS() + electionTimeoutMS_ + 500;
		lastHeartbeatMS_ = 0;
		lastTTLScanMS_ = nowWallMS();
		lastSnapshotMS_ = nowWallMS();
	}

	spawn([this]() { serviceAcceptorLoop(); });
	spawn([this]() { peerAcceptorLoop(); });
	spawn([this]() { peerConnectorLoop(); });

	INFO_MSG(fmt::format("ClusterCore::start(): self={}({}) servicePort={} peerPort={} members={} started.\n",
		selfIdx_, selfIP_, servicePort_, peerPort_, members_.size()));

	return true;
}

//-------------------------------------------------------------------------------------
void ClusterCore::loadSnapshotFromDisk()
{
	char fname[128] = {0};
	kbe_snprintf(fname, 128, "cluster_state_%d.bin", selfIdx_);

	std::ifstream fin(fname, std::ios::binary);
	if(!fin.is_open())
		return;

	std::string bytes((std::istreambuf_iterator<char>(fin)), std::istreambuf_iterator<char>());
	fin.close();

	if(bytes.size() < 8)
		return;

	// 快照文件布局: [u32 magic][registrySnapshotBytes]
	size_t off = 0;
	uint32_t magic = 0;
	if(!ClusterInterface::Wire::getU32(bytes.data(), bytes.size(), off, magic) || magic != 0x4B42E533)
		return;

	// 跳过 4 字节 magic 头后解析(registrySnapshotBytes 不含 magic)
	decodeSnapshot(bytes.substr(4));
	INFO_MSG(fmt::format("ClusterCore::loadSnapshotFromDisk(): restored from {}, registry={}, snapshotIndex={}\n",
		fname, registry_.size(), snapshotIndex_));
}

//-------------------------------------------------------------------------------------
std::string ClusterCore::registrySnapshotBytes() const
{
	std::string b;
	ClusterInterface::Wire::putU64(b, snapshotIndex_);
	ClusterInterface::Wire::putU64(b, snapshotTerm_);
	ClusterInterface::Wire::putU64(b, commitIndex_);
	ClusterInterface::Wire::putU64(b, currentTerm_);

	ClusterInterface::Wire::putU32(b, (uint32_t)registry_.size());
	for(std::map<std::string, RegistryEntry>::const_iterator it = registry_.begin();
		it != registry_.end(); ++it)
	{
		ClusterInterface::Wire::putString(b, it->first);
		// ComponentData
		std::string d = ClusterInterface::encodeComponentData(it->second.data);
		ClusterInterface::Wire::putString(b, d);
		ClusterInterface::Wire::putU64(b, it->second.lastRenewal);
	}

	return b;
}

//-------------------------------------------------------------------------------------
void ClusterCore::decodeSnapshot(const std::string& bytes)
{
	size_t off = 0;
	uint32_t count = 0;

	uint64 idx = 0, tm = 0, cm = 0, ct = 0;
	if(!ClusterInterface::Wire::getU64(bytes.data(), bytes.size(), off, idx) ||
		!ClusterInterface::Wire::getU64(bytes.data(), bytes.size(), off, tm) ||
		!ClusterInterface::Wire::getU64(bytes.data(), bytes.size(), off, cm) ||
		!ClusterInterface::Wire::getU64(bytes.data(), bytes.size(), off, ct))
		return;

	if(!ClusterInterface::Wire::getU32(bytes.data(), bytes.size(), off, count))
		return;

	snapshotIndex_ = idx;
	snapshotTerm_ = tm;
	commitIndex_ = cm;
	lastApplied_ = cm;
	currentTerm_ = ct;
	registry_.clear();
	log_.clear();

	for(uint32_t i = 0; i < count; ++i)
	{
		std::string key, compData;
		uint64_t renewal = 0;
		if(!ClusterInterface::Wire::getString(bytes.data(), bytes.size(), off, key) ||
			!ClusterInterface::Wire::getString(bytes.data(), bytes.size(), off, compData) ||
			!ClusterInterface::Wire::getU64(bytes.data(), bytes.size(), off, renewal))
			break;

		RegistryEntry e;
		size_t doff = 0;
		if(!ClusterInterface::decodeComponentData(compData.data(), compData.size(), doff, e.data))
			continue;

		e.lastRenewal = renewal;
		registry_[key] = e;
	}
}

//-------------------------------------------------------------------------------------
void ClusterCore::saveSnapshot()
{
	if(lastApplied_ <= snapshotIndex_ && registry_.size() == 0)
		return;

	std::string bytes;
	ClusterInterface::Wire::putU32(bytes, 0x4B42E533);
	bytes += registrySnapshotBytes();

	char fname[128] = {0};
	kbe_snprintf(fname, 128, "cluster_state_%d.bin", selfIdx_);

	std::ofstream fout(fname, std::ios::binary | std::ios::trunc);
	if(!fout.is_open())
	{
		ERROR_MSG(fmt::format("ClusterCore::saveSnapshot(): cannot write {}\n", fname));
		return;
	}

	fout.write(bytes.c_str(), (std::streamsize)bytes.size());
	fout.close();

	lastSnapshotMS_ = nowWallMS();

	INFO_MSG(fmt::format("ClusterCore::saveSnapshot(): saved {} registry={} snapshotIndex={}.\n",
		fname, registry_.size(), snapshotIndex_));
}

//-------------------------------------------------------------------------------------
void ClusterCore::saveSnapshot(const std::string& bytes)
{
	char fname[128] = {0};
	kbe_snprintf(fname, 128, "cluster_state_%d.bin", selfIdx_);

	// 落盘统一带 magic 头(与 loadSnapshotFromDisk 一致)
	std::string out;
	ClusterInterface::Wire::putU32(out, 0x4B42E533);
	out += bytes;

	std::ofstream fout(fname, std::ios::binary | std::ios::trunc);
	if(!fout.is_open())
		return;

	fout.write(out.c_str(), (std::streamsize)out.size());
	fout.close();
}

//-------------------------------------------------------------------------------------
void ClusterCore::serviceAcceptorLoop()
{
	INFO_MSG(fmt::format("ClusterCore::serviceAcceptorLoop(): thread start.\n"));

	while(running_.load())
	{
		u_int16_t port = 0;
		u_int32_t addr = 0;
		Network::EndPoint* newEP = serviceListenEP_->accept(&port, &addr, false);
		if(newEP == NULL)
		{
			if(!running_.load())
				break;
			std::this_thread::sleep_for(std::chrono::milliseconds(50));
			continue;
		}

		uint64_t connId = nextConnId_.fetch_add(1);
		Network::EndPoint* accepted = newEP;
		if(!spawn([this, accepted, connId]() { serviceConnThread(accepted, connId); }))
		{
			accepted->close();
			Network::EndPoint::reclaimPoolObject(accepted);
		}
	}

	INFO_MSG(fmt::format("ClusterCore::serviceAcceptorLoop(): thread exit.\n"));
}

//-------------------------------------------------------------------------------------
void ClusterCore::serviceConnThread(Network::EndPoint* ep, uint64 connId)
{
	std::string req;
	bool wait = false;

	{
		std::lock_guard<std::mutex> lock(queueMutex_);
		// 占位确保 replyMutex_ 映射已建立
	}

	while(running_.load())
	{
		if(!recvFrame(ep, req))
			break;

		Event ev;
		ev.type = EVENT_SERVICE_REQ;
		ev.connId = connId;
		ev.peerIdx = -1;
		ev.data = req;

		{
			std::lock_guard<std::mutex> lock(queueMutex_);
			queue_.push_back(ev);
		}

		wait = true;
		break; // 短连接：一次请求即等待响应
	}

	if(wait)
	{
		// 等待主循环应答（限时）
		std::unique_lock<std::mutex> lock(replyMutex_);
		replyCv_.wait_for(lock, std::chrono::milliseconds(4000),
			[this, connId]() { return !running_.load() || replies_.find(connId) != replies_.end(); });

		if(replies_.find(connId) != replies_.end())
		{
			sendFrame(ep, replies_[connId]);
			replies_.erase(connId);
		}

		lock.unlock();
	}

	if(ep)
	{
		ep->close();
		Network::EndPoint::reclaimPoolObject(ep);
	}
}

//-------------------------------------------------------------------------------------
void ClusterCore::peerAcceptorLoop()
{
	INFO_MSG(fmt::format("ClusterCore::peerAcceptorLoop(): thread start.\n"));

	while(running_.load())
	{
		u_int16_t port = 0;
		u_int32_t addr = 0;
		Network::EndPoint* newEP = peerListenEP_->accept(&port, &addr, false);
		if(newEP == NULL)
		{
			if(!running_.load())
				break;
			std::this_thread::sleep_for(std::chrono::milliseconds(50));
			continue;
		}

		// 根据对端IP确定成员序号（地址必须与配置一致）
		int peerIdx = -1;
		uint32_t netAddr = addr;
		for(size_t i = 0; i < members_.size(); ++i)
		{
			if(i == (size_t)selfIdx_)
				continue;

			uint32_t a = ::inet_addr(members_[i].c_str());
			if(a == netAddr)
			{
				peerIdx = (int)i;
				break;
			}
		}

		if(peerIdx < 0)
		{
			ERROR_MSG(fmt::format("ClusterCore::peerAcceptorLoop(): unknown peer addr ignored.\n"));
			newEP->close();
			Network::EndPoint::reclaimPoolObject(newEP);
			continue;
		}

		std::lock_guard<std::mutex> lock(peersMutex_);
		if(peers_[peerIdx].connected)
		{
			// 重复连接(异常)，关闭新连接
			newEP->close();
			Network::EndPoint::reclaimPoolObject(newEP);
			continue;
		}

		peers_[peerIdx].connected = true;
		peers_[peerIdx].ep = newEP;
		peers_[peerIdx].outbound = false;
		if(!spawn([this, newEP, peerIdx]() { peerConnThread(newEP, peerIdx, false); }))
		{
			peers_[peerIdx].connected = false;
			peers_[peerIdx].ep = NULL;
			newEP->close();
			Network::EndPoint::reclaimPoolObject(newEP);
		}
	}

	INFO_MSG(fmt::format("ClusterCore::peerAcceptorLoop(): thread exit.\n"));
}

//-------------------------------------------------------------------------------------
void ClusterCore::peerConnectorLoop()
{
	INFO_MSG(fmt::format("ClusterCore::peerConnectorLoop(): thread start.\n"));

	while(running_.load())
	{
		for(size_t i = 0; i < members_.size(); ++i)
		{
			if(!running_.load())
				break;

			if((int)i == selfIdx_ || (int)i < selfIdx_)
				continue;	// 主动连接方 = 成员序号小者

			bool connected = false;
			{
				std::lock_guard<std::mutex> lock(peersMutex_);
				connected = peers_[i].connected;
			}

			if(connected)
				continue;

			// 尝试连接
			Network::EndPoint* ep = Network::EndPoint::createPoolObject(OBJECTPOOL_POINT);
			ep->socket(SOCK_STREAM);
			if(!ep->good())
			{
				Network::EndPoint::reclaimPoolObject(ep);
				break;
			}

			// 出站连接必须显式 bind 到本机成员地址 selfIP_, 否则多网卡/多回环地址
			// 环境下 OS 可能挑选其它源 IP(如 Windows 上全部回环连接源地址恒为
			// 127.0.0.1), 导致对端按源 IP 识别成员时张冠李戴、链路反复被当作
			// "重复连接"关闭, 引发选主乒乓。bind 失败(如地址确实不属于本机)时
			// 退回系统默认选路并告警, 避免在误配置时完全无法组网。
			uint32_t srcAddr = ::inet_addr(selfIP_.c_str());
			if(srcAddr == INADDR_NONE || ep->bind(htons(0), srcAddr) == -1)
			{
				ERROR_MSG(fmt::format("ClusterCore::peerConnectorLoop(): bind outbound to {} "
					"failed, fallback to OS routing (member identity may be wrong!).\n",
					selfIP_));
			}

			uint32_t peerAddr = ::inet_addr(members_[i].c_str());
			if(ep->connect(htons(peerPort_), peerAddr, false) != -1)
			{
				std::lock_guard<std::mutex> lock(peersMutex_);
				if(!running_.load() || peers_[i].connected)
				{
					ep->close();
					Network::EndPoint::reclaimPoolObject(ep);
				}
				else
				{
					peers_[i].connected = true;
					peers_[i].ep = ep;
					peers_[i].outbound = true;
					int peerIdx = (int)i;
					if(!spawn([this, ep, peerIdx]() { peerConnThread(ep, peerIdx, true); }))
					{
						peers_[i].connected = false;
						peers_[i].ep = NULL;
						ep->close();
						Network::EndPoint::reclaimPoolObject(ep);
					}
				}
			}
			else
			{
				ep->close();
				Network::EndPoint::reclaimPoolObject(ep);
			}
		}

		for(int k = 0; k < 10 && running_.load(); ++k)
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
	}

	INFO_MSG(fmt::format("ClusterCore::peerConnectorLoop(): thread exit.\n"));
}

//-------------------------------------------------------------------------------------
void ClusterCore::peerConnThread(Network::EndPoint* ep, int peerIdx, bool outbound)
{
	(void)outbound;

	INFO_MSG(fmt::format("ClusterCore::peerConnThread(): connected with member {}. thread start.\n", peerIdx));

	std::string frame;
	while(running_.load())
	{
		if(!recvFrame(ep, frame))
			break;

		Event ev;
		ev.type = EVENT_PEER_MSG;
		ev.connId = 0;
		ev.peerIdx = peerIdx;
		ev.data = frame;

		{
			std::lock_guard<std::mutex> lock(queueMutex_);
			queue_.push_back(ev);
		}
	}

	// 连接断开
	{
		std::lock_guard<std::mutex> lock(peersMutex_);
		if(peers_[peerIdx].ep == ep)
		{
			peers_[peerIdx].connected = false;
			peers_[peerIdx].ep = NULL;
			peers_[peerIdx].lastAcked = 0;
			peers_[peerIdx].needSnapshot = true;
		}
	}

	if(ep)
	{
		ep->close();
		Network::EndPoint::reclaimPoolObject(ep);
	}

	INFO_MSG(fmt::format("ClusterCore::peerConnThread(): member {} disconnected.\n", peerIdx));
}

//-------------------------------------------------------------------------------------
void ClusterCore::tick()
{
	uint64 now = nowWallMS();
	lastTickMS_ = now;

	// 1. 处理事件
	drainEvents();

	// 2. 选举超时(仅 follower/candidate)
	if(role_ != ROLE_LEADER)
	{
		if(now >= electionDeadlineMS_)
		{
			// 成为候选人
			role_ = ROLE_CANDIDATE;
			currentTerm_ += 1;
			votedFor_ = selfIdx_;
			leaderIdx_ = -1;
			grantVotes_.clear();
			grantVotes_[selfIdx_] = true;

			uint32 jitter = (electionTimeoutMS_ * 2 / 3) + 1;
			electionDeadlineMS_ = now + electionTimeoutMS_ + (fastRand() % jitter);

			DEBUG_MSG(fmt::format("ClusterCore::tick(): start election term={} self={}.\n",
				currentTerm_, selfIdx_));

			// 候选人的最新日志(投票者按 raft 规则做"日志新旧"比较)
			uint64 myLastIdx = lastIndex();
			uint64 myLastTerm = snapshotTerm_;
			if(myLastIdx > snapshotIndex_)
				myLastTerm = log_.back().term;

			// 广播投票请求
			for(size_t i = 0; i < members_.size(); ++i)
			{
				if((int)i == selfIdx_)
					continue;

				std::string msg;
				ClusterInterface::Wire::putU8(msg, PeerProto::PM_VOTE_REQ);
				ClusterInterface::Wire::putU64(msg, currentTerm_);
				ClusterInterface::Wire::putI32(msg, (int32_t)selfIdx_);
				ClusterInterface::Wire::putU64(msg, myLastIdx);
				ClusterInterface::Wire::putU64(msg, myLastTerm);
				sendFrameToPeer((int)i, msg);
			}
		}
	}
	else
	{
		// 3. leader 心跳 / 复制
		if(now - lastHeartbeatMS_ >= heartbeatIntervalMS_)
		{
			lastHeartbeatMS_ = now;
			for(size_t i = 0; i < members_.size(); ++i)
			{
				if((int)i == selfIdx_)
					continue;
				sendReplicateToPeer((int)i);
			}
		}
	}

	// 4. TTL 扫描(仅 leader)
	if(isLeader() && now - lastTTLScanMS_ >= 1000)
	{
		lastTTLScanMS_ = now;
		ttlScan();
	}

	// 5. 定期快照/日志归档(leader/follower 都压缩本机日志)
	if(now - lastSnapshotMS_ >= 60000 && log_.size() > 64 &&
		commitIndex_ > snapshotIndex_)
	{
		// 快照覆盖到 commitIndex_, snapshotTerm 取被归档的最后一条日志的 term
		uint64 newSnap = commitIndex_;
		uint64 drop = newSnap - snapshotIndex_;
		if(drop > log_.size())
			drop = log_.size();

		if(drop > 0)
			snapshotTerm_ = log_[(size_t)drop - 1].term;

		snapshotIndex_ = newSnap;
		saveSnapshot();		// 先落盘(registrySnapshotBytes 只依赖 registry/snapshotIndex)

		// 落盘成功后移除已被快照覆盖的日志
		if(drop >= log_.size())
			log_.clear();
		else
			log_.erase(log_.begin(), log_.begin() + (size_t)drop);
	}
}

//-------------------------------------------------------------------------------------
void ClusterCore::drainEvents()
{
	std::vector<Event> evts;
	{
		std::lock_guard<std::mutex> lock(queueMutex_);
		evts.swap(queue_);
	}

	for(size_t i = 0; i < evts.size(); ++i)
	{
		if(evts[i].type == EVENT_SERVICE_REQ)
			handleServiceRequest(evts[i]);
		else
			handlePeerMessage(evts[i]);
	}
}

//-------------------------------------------------------------------------------------
void ClusterCore::sendFrameToPeer(int idx, const std::string& msg)
{
	Network::EndPoint* ep = NULL;
	{
		std::lock_guard<std::mutex> lock(peersMutex_);
		if(idx >= 0 && idx < (int)peers_.size() && peers_[idx].connected)
			ep = peers_[idx].ep;
	}

	if(ep == NULL)
		return;

	if(!sendFrame(ep, msg))
	{
		// 发送失败, 交由读线程检测断开
	}
}

//-------------------------------------------------------------------------------------
void ClusterCore::sendReplicateToPeer(int idx)
{
	uint64 leaderCommit = commitIndex_;
	uint64 fromIndex = 0;
	bool needSnap = false;

	{
		std::lock_guard<std::mutex> lock(peersMutex_);
		if(idx < 0 || idx >= (int)peers_.size() || !peers_[idx].connected)
			return;

		if(peers_[idx].needSnapshot)
		{
			needSnap = true;
		}
		else
		{
			uint64 ack = peers_[idx].lastAcked;
			if(ack < snapshotIndex_)
				needSnap = true;
			else
				fromIndex = ack + 1;
		}
	}

	uint64 last = lastIndex();

	// 落后于已归档的日志起点(或显式 needSnapshot) => 全量快照同步
	if(needSnap || (fromIndex <= snapshotIndex_ && fromIndex <= last + 1))
	{
		std::string snap = registrySnapshotBytes();

		std::string msg;
		ClusterInterface::Wire::putU8(msg, PeerProto::PM_APPEND);
		ClusterInterface::Wire::putU64(msg, currentTerm_);
		ClusterInterface::Wire::putI32(msg, (int32_t)selfIdx_);
		ClusterInterface::Wire::putU64(msg, leaderCommit);
		ClusterInterface::Wire::putU8(msg, 1);	// kind=snapshot
		ClusterInterface::Wire::putU32(msg, (uint32_t)snap.size());
		msg += snap;
		sendFrameToPeer(idx, msg);
		return;
	}

	std::string msg;
	ClusterInterface::Wire::putU8(msg, PeerProto::PM_APPEND);
	ClusterInterface::Wire::putU64(msg, currentTerm_);
	ClusterInterface::Wire::putI32(msg, (int32_t)selfIdx_);
	ClusterInterface::Wire::putU64(msg, leaderCommit);
	ClusterInterface::Wire::putU8(msg, 0);	// kind=incremental(含 count=0 的心跳)

	// prev 指向 fromIndex-1；若已追平则 prev 指向 last(心跳, count=0)
	uint64 prevIndex = (fromIndex > last) ? last : fromIndex - 1;
	uint64 prevTerm = snapshotTerm_;
	if(prevIndex > snapshotIndex_)
		prevTerm = log_[prevIndex - snapshotIndex_ - 1].term;

	ClusterInterface::Wire::putU64(msg, prevIndex);
	ClusterInterface::Wire::putU64(msg, prevTerm);

	uint16_t count = 0;
	uint64_t end = last;
	if(fromIndex <= end)
		count = (uint16_t)(end - fromIndex + 1);

	ClusterInterface::Wire::putU16(msg, count);

	for(uint64_t i = fromIndex; i <= end; ++i)
	{
		const RaftLogEntry& e = log_[i - snapshotIndex_ - 1];
		ClusterInterface::Wire::putU64(msg, e.index);
		ClusterInterface::Wire::putU64(msg, e.term);
		ClusterInterface::Wire::putU8(msg, e.type);
		ClusterInterface::Wire::putU32(msg, (uint32_t)e.data.size());
		msg += e.data;
	}

	sendFrameToPeer(idx, msg);
}

//-------------------------------------------------------------------------------------
void ClusterCore::handleServiceRequest(const Event& ev)
{
	const std::string& d = ev.data;
	if(d.size() < 1)
		return;

	size_t off = 0;
	uint8_t mt = 0;
	if(!ClusterInterface::Wire::getU8(d.data(), d.size(), off, mt))
		return;

	// 统一响应
	auto sendResp = [this, ev](uint8_t type, const std::string& payload)
	{
		std::string msg;
		ClusterInterface::Wire::putU8(msg, type);
		msg += payload;

		std::lock_guard<std::mutex> lock(replyMutex_);
		replies_[ev.connId] = msg;
		replyCv_.notify_all();
	};

	// 查询 leader
	if(mt == ClusterInterface::MSG_QUERY_LEADER)
	{
		std::string payload;
		if(role_ == ROLE_LEADER)
		{
			ClusterInterface::Wire::putU32(payload, ::inet_addr(selfIP_.c_str()));
			ClusterInterface::Wire::putU16(payload, servicePort_);
			sendResp(ClusterInterface::MSG_RESP_LEADER, payload);
		}
		else
		{
			ClusterInterface::Wire::putU32(payload, leaderIdx_ >= 0 ?
				::inet_addr(members_[leaderIdx_].c_str()) : 0);
			ClusterInterface::Wire::putU16(payload, leaderIdx_ >= 0 ? servicePort_ : 0);
			sendResp(ClusterInterface::MSG_RESP_NOT_LEADER, payload);
		}
		return;
	}

	// 内部代理指令(MSG_PROXY_*): leader 已把目标定为本机, 由本机代理执行并直接应答。
	// 不受副本角色限制(即使本机不是 leader 也不重定向, 避免循环转发)。
	if(mt == ClusterInterface::MSG_PROXY_START_SERVER ||
		mt == ClusterInterface::MSG_PROXY_STOP_SERVER ||
		mt == ClusterInterface::MSG_PROXY_KILL_SERVER)
	{
		uint8_t baseOp = (uint8_t)(mt -
			(ClusterInterface::MSG_PROXY_START_SERVER - ClusterInterface::MSG_START_SERVER));
		handleCtlCommand(ev, baseOp, off, true);
		return;
	}

	// 非 leader：返回重定向
	if(role_ != ROLE_LEADER)
	{
		std::string payload;
		ClusterInterface::Wire::putU32(payload, leaderIdx_ >= 0 ?
			::inet_addr(members_[leaderIdx_].c_str()) : 0);
		ClusterInterface::Wire::putU16(payload, leaderIdx_ >= 0 ? servicePort_ : 0);
		sendResp(ClusterInterface::MSG_RESP_NOT_LEADER, payload);
		return;
	}

	switch(mt)
	{
	case ClusterInterface::MSG_REGISTER:
	{
		ClusterInterface::ComponentData comp;
		if(!ClusterInterface::decodeComponentData(d.data(), d.size(), off, comp))
			return;

		std::string key = encodeKey(comp.uid, comp.componentType, (int32)comp.componentID);
		uint64 now = nowWallMS();

		bool conflict = false;
		std::map<std::string, RegistryEntry>::iterator it = registry_.find(key);
		if(it != registry_.end())
		{
			bool expired = now - it->second.lastRenewal >= componentLeaseMS_ &&
				it->second.lastRenewal > 0;

			if(!expired && it->second.data.pid != comp.pid)
			{
				// 同 uid/type/cid 已注册且租约未过期、进程不同 -> 身份冲突
				conflict = true;
			}
		}

		if(conflict)
		{
			std::string payload;
			// 已存在进程信息
			payload += ClusterInterface::encodeComponentData(it->second.data);
			sendResp(ClusterInterface::MSG_RESP_IDENTITY_CONFLICT, payload);
			return;
		}

		// 写入日志
		std::string payload;
		ClusterInterface::Wire::putU64(payload, now);
		payload += ClusterInterface::encodeComponentData(comp);

		uint64 idx = appendLog(ClusterCore::CMD_REGISTER, payload);
		registerWaiter(idx, ev.connId);
		break;
	}

	case ClusterInterface::MSG_RENEW:
	{
		// 客户端(Components::process/guiconsole)编码为 [i32 uid][i32 type][u64 cid][u32 pid],
		// 这里不能像 CMD 日志那样在头部附加时间戳, 否则字段全部错位。
		uint32_t uid = 0, type = 0, pid = 0;
		uint64_t cid = 0;
		uint8_t tmp8 = 0;
		uint32_t tmp32 = 0;
		if(!ClusterInterface::Wire::getU32(d.data(), d.size(), off, tmp32)) return; uid = tmp32;
		if(!ClusterInterface::Wire::getU32(d.data(), d.size(), off, tmp32)) return; type = tmp32;
		if(!ClusterInterface::Wire::getU64(d.data(), d.size(), off, cid)) return;
		if(!ClusterInterface::Wire::getU32(d.data(), d.size(), off, pid)) return;
		(void)tmp8;

		std::string key = encodeKey((int32)uid, (int32)type, (int64)cid);
		std::map<std::string, RegistryEntry>::iterator it = registry_.find(key);
		if(it == registry_.end())
		{
			sendResp(ClusterInterface::MSG_RESP_NOT_FOUND, std::string());
			return;
		}

		std::string payload;
		ClusterInterface::Wire::putU64(payload, nowWallMS());
		ClusterInterface::Wire::putU32(payload, uid);
		ClusterInterface::Wire::putU32(payload, type);
		ClusterInterface::Wire::putU64(payload, cid);
		ClusterInterface::Wire::putU32(payload, pid);

		uint64 idx = appendLog(ClusterCore::CMD_RENEW, payload);
		registerWaiter(idx, ev.connId);
		break;
	}

	case ClusterInterface::MSG_UNREGISTER:
	{
		uint32_t uid = 0, type = 0;
		uint64_t cid = 0;
		uint32_t tmp32 = 0;
		if(!ClusterInterface::Wire::getU32(d.data(), d.size(), off, tmp32)) return; uid = tmp32;
		if(!ClusterInterface::Wire::getU32(d.data(), d.size(), off, tmp32)) return; type = tmp32;
		if(!ClusterInterface::Wire::getU64(d.data(), d.size(), off, cid)) return;

		std::string payload;
		ClusterInterface::Wire::putU32(payload, uid);
		ClusterInterface::Wire::putU32(payload, type);
		ClusterInterface::Wire::putU64(payload, cid);

		uint64 idx = appendLog(ClusterCore::CMD_UNREGISTER, payload);
		registerWaiter(idx, ev.connId);
		break;
	}

	case ClusterInterface::MSG_FIND:
	{
		uint32_t uid = 0;
		int32_t findType = -1;
		uint64_t findCid = 0;
		uint32_t tmp32 = 0;
		if(!ClusterInterface::Wire::getU32(d.data(), d.size(), off, tmp32)) return; uid = tmp32;
		uint32_t ft = 0;
		if(!ClusterInterface::Wire::getU32(d.data(), d.size(), off, ft)) return; findType = (int32_t)ft;
		if(!ClusterInterface::Wire::getU64(d.data(), d.size(), off, findCid)) return;

		std::string payload;
		uint16_t found = 0;
		ClusterInterface::ComponentData target;
		bool exact = (findCid != 0);

		for(std::map<std::string, RegistryEntry>::iterator it = registry_.begin();
			it != registry_.end(); ++it)
		{
			const ClusterInterface::ComponentData& c = it->second.data;
			if((int32)c.uid != (int32)uid || (int32)c.componentType != findType)
				continue;

			if(exact && (uint64)c.componentID != findCid)
				continue;

			if(exact)
			{
				target = c;
				found = 1;
				break;
			}

			payload += ClusterInterface::encodeComponentData(c);
			found++;
		}

		if(exact)
		{
			if(found == 0)
			{
				sendResp(ClusterInterface::MSG_RESP_NOT_FOUND, std::string());
			}
			else
			{
				sendResp(ClusterInterface::MSG_RESP_ENTRY,
					ClusterInterface::encodeComponentData(target));
			}
		}
		else
		{
			std::string body;
			ClusterInterface::Wire::putU16(body, found);
			body += payload;
			sendResp(ClusterInterface::MSG_RESP_ENTRY_LIST, body);
		}
		break;
	}

	case ClusterInterface::MSG_QUERY_ALL:
	{
		uint32_t uid = 0;
		uint32_t tmp32 = 0;
		if(!ClusterInterface::Wire::getU32(d.data(), d.size(), off, tmp32)) return; uid = tmp32;

		std::string payload;
		uint16_t found = 0;
		for(std::map<std::string, RegistryEntry>::iterator it = registry_.begin();
			it != registry_.end(); ++it)
		{
			const ClusterInterface::ComponentData& c = it->second.data;
			if((int32)c.uid != (int32)uid)
				continue;

			payload += ClusterInterface::encodeComponentData(c);
			found++;
		}

		std::string body;
		ClusterInterface::Wire::putU16(body, found);
		body += payload;
		sendResp(ClusterInterface::MSG_RESP_ENTRY_LIST, body);
		break;
	}

	case ClusterInterface::MSG_START_SERVER:
	case ClusterInterface::MSG_STOP_SERVER:
	case ClusterInterface::MSG_KILL_SERVER:
	{
		// 远程起停: leader 依据注册表/目标主机聚合, 本机部分由本机代理执行,
		// 其它主机经 MSG_PROXY_* 转发(见 handleCtlCommand)
		handleCtlCommand(ev, mt, off, false);
		break;
	}

	default:
		break;
	}
}

//-------------------------------------------------------------------------------------
void ClusterCore::handleCtlCommand(const Event& ev, uint8_t op, size_t off, bool localOnly)
{
	// 统一应答: code(0=成功/<0失败) + message
	auto reply = [this, ev](int32 code, const std::string& message)
	{
		std::string payload;
		ClusterInterface::Wire::putI32(payload, code);
		ClusterInterface::Wire::putString(payload, message);

		std::string msg;
		ClusterInterface::Wire::putU8(msg, ClusterInterface::MSG_RESP_CMD_RESULT);
		msg += payload;

		std::lock_guard<std::mutex> lock(replyMutex_);
		replies_[ev.connId] = msg;
		replyCv_.notify_all();
	};

	const std::string& d = ev.data;

	ClusterInterface::ServerCommandData cmd;
	if(!ClusterInterface::decodeServerCommand(d.data(), d.size(), off, cmd))
	{
		reply(-1, "invalid server command payload");
		return;
	}

	// 只允许起/停真正的服务组件(不允许 console/cluster/machine 等内部类型)
	auto componentValid = [](int32 t) -> bool
	{
		return t > (int32)UNKNOWN_COMPONENT_TYPE && t < (int32)COMPONENT_END_TYPE &&
			t != (int32)CLIENT_TYPE && t != (int32)MACHINE_TYPE &&
			t != (int32)CONSOLE_TYPE && t != (int32)TOOL_TYPE &&
			t != (int32)CLUSTER_TYPE;
	};

	if(!componentValid(cmd.componentType))
	{
		reply(-1, fmt::format("invalid componentType: {}", cmd.componentType));
		return;
	}

	const char* cname = COMPONENT_NAME_EX((COMPONENT_TYPE)cmd.componentType);

	auto memberIndexByAddr = [this](uint32 addr) -> int
	{
		for(size_t i = 0; i < members_.size(); ++i)
		{
			if((uint32)::inet_addr(members_[i].c_str()) == addr)
				return (int)i;
		}
		return -1;
	};

	auto memberIP = [this](int idx) -> uint32
	{
		return (uint32)::inet_addr(members_[idx].c_str());
	};

	// ================= START =================
	if(op == ClusterInterface::MSG_START_SERVER)
	{
		int execHost = selfIdx_;
		if(!cmd.targetHost.empty() && !localOnly)
		{
			uint32 tip = (uint32)::inet_addr(cmd.targetHost.c_str());
			if(tip == (uint32)INADDR_NONE)
			{
				reply(-1, fmt::format("invalid targetHost: {}", cmd.targetHost));
				return;
			}

			execHost = memberIndexByAddr(tip);
			if(execHost < 0)
			{
				reply(-1, fmt::format("targetHost {} is not a cluster member", cmd.targetHost));
				return;
			}
		}

		if(localOnly)
			execHost = selfIdx_;

		if(execHost != selfIdx_)
		{
			// 转发给目标主机副本的“本机代理”执行
			std::string req = d;
			if(req.size() > 0)
				req[0] = (char)ClusterInterface::MSG_PROXY_START_SERVER;

			std::string resp;
			if(clusterCtlRequest(req, resp, memberIP(execHost), servicePort_, 3000) != 0)
			{
				reply(-1, fmt::format("start {}: member {} unreachable", cname, members_[execHost]));
				return;
			}

			if(resp.size() < 1 || (uint8_t)resp[0] != ClusterInterface::MSG_RESP_CMD_RESULT)
			{
				reply(-1, fmt::format("start {}: bad proxy response from {}", cname, members_[execHost]));
				return;
			}

			size_t o = 1;
			int32_t rcode = 0;
			std::string rmsg;
			ClusterInterface::Wire::getI32(resp.data(), resp.size(), o, rcode);
			ClusterInterface::Wire::getString(resp.data(), resp.size(), o, rmsg);
			reply(rcode, rmsg);
			return;
		}

		// 本机代理直接拉起(组件启动参数 --cid/--gus 沿用原 machine startserver 语义;
		// cid=0 时组件进程按 kbemain.h 本地确定性规则自生成)
		std::string err;
		if(ClusterProcess::startProcess(cmd.uid, (COMPONENT_TYPE)cmd.componentType,
			cmd.cid, cmd.gus, err))
		{
			INFO_MSG(fmt::format("ClusterCore::handleCtlCommand: start {} ok (uid={}, cid={}, gus={})\n",
				cname, cmd.uid, cmd.cid, cmd.gus));
			reply(0, fmt::format("start {} ok", cname));
		}
		else
		{
			ERROR_MSG(fmt::format("ClusterCore::handleCtlCommand: start {} failed: {}\n",
				cname, err));
			reply(-1, fmt::format("start {} failed: {}", cname, err));
		}
		return;
	}

	// ================= STOP / KILL =================
	const bool killOp = (op == ClusterInterface::MSG_KILL_SERVER);

	std::vector<ClusterProcess::CtlTarget> localTargets;
	std::map<int, bool> remoteHosts;
	int matched = 0;

	// targetHost 非空: 指令仅作用于指定主机上的注册条目(原 guiconsole 按布局逐主机
	// 停止的语义, leader 只对目标主机执行/转发, 其它主机上的同类型组件不受影响)
	int targetIdx = -1;
	if(!cmd.targetHost.empty())
	{
		uint32 tip = (uint32)::inet_addr(cmd.targetHost.c_str());
		if(tip == (uint32)INADDR_NONE)
		{
			reply(-1, fmt::format("invalid targetHost: {}", cmd.targetHost));
			return;
		}

		targetIdx = memberIndexByAddr(tip);
		if(targetIdx < 0)
		{
			reply(-1, fmt::format("targetHost {} is not a cluster member", cmd.targetHost));
			return;
		}
	}

	for(std::map<std::string, RegistryEntry>::iterator it = registry_.begin();
		it != registry_.end(); ++it)
	{
		const ClusterInterface::ComponentData& c = it->second.data;
		if((int32)c.uid != cmd.uid)
			continue;
		if((int32)c.componentType != cmd.componentType)
			continue;
		if(cmd.cid != 0 && (uint64)c.componentID != cmd.cid)
			continue;

		++matched;

		// 条目所属主机: 以组件 internal 地址解析到成员; 解析不到(如绑定回环/配置差异)
		// 视为本机并告警。代理执行(localOnly)同样做主机过滤, 避免跨主机误杀(pid 不同主机可复用)。
		int host = memberIndexByAddr(c.intaddr);
		if(host < 0)
		{
			WARNING_MSG(fmt::format(
				"ClusterCore::handleCtlCommand: component (uid={}, type={}, cid={}) "
				"intaddr {:#010x} not matched to members, treat as local.\n",
				(int32)c.uid, (int32)c.componentType, (uint64)c.componentID, c.intaddr));
			host = selfIdx_;
		}

		if(targetIdx >= 0 && host != targetIdx)
			continue;

		if(host == selfIdx_)
		{
			ClusterProcess::CtlTarget t;
			t.pid = c.pid;
			t.intaddr = c.intaddr;
			t.intport = c.intport;
			localTargets.push_back(t);
		}
		else if(localOnly)
		{
			// 代理执行: 不属于本机的条目跳过(leader 只把本机条目对应的指令转发过来)
			continue;
		}
		else
		{
			remoteHosts[host] = true;
		}
	}

	if(matched == 0)
	{
		reply(0, fmt::format("no running {} component found (nothing to do)", cname));
		return;
	}

	if(localTargets.empty() && remoteHosts.empty())
	{
		// 集群中存在同类型组件, 但指定主机上当前没有运行实例
		reply(0, fmt::format("no running {} component on {}", cname,
			targetIdx >= 0 ? members_[targetIdx] : std::string("any member")));
		return;
	}

	int32 code = 0;
	std::string detail;

	// 本机部分
	if(localTargets.empty())
	{
		detail += "local:none ";
	}
	else
	{
		std::string err;
		bool ok = killOp ?
			ClusterProcess::killProcess(localTargets, err) :
			ClusterProcess::stopProcess((COMPONENT_TYPE)cmd.componentType, localTargets, err);

		if(!ok)
		{
			code = -1;
			detail += "local:" + err + " ";
		}
		else
		{
			detail += fmt::format("local:{} ", killOp ? "killed" : "stopped");
		}
	}

	// 其它主机: 逐个转发给对应副本执行(leader 本机阻塞, 每个主机上界约 4s)
	for(std::map<int, bool>::iterator it = remoteHosts.begin(); it != remoteHosts.end(); ++it)
	{
		int idx = it->first;
		std::string req = d;
		if(req.size() > 0)
			req[0] = (char)(killOp ?
				ClusterInterface::MSG_PROXY_KILL_SERVER : ClusterInterface::MSG_PROXY_STOP_SERVER);

		std::string resp;
		if(clusterCtlRequest(req, resp, memberIP(idx), servicePort_, 4000) != 0)
		{
			code = -1;
			detail += fmt::format("host {}:unreachable ", members_[idx]);
			continue;
		}

		if(resp.size() < 1 || (uint8_t)resp[0] != ClusterInterface::MSG_RESP_CMD_RESULT)
		{
			code = -1;
			detail += fmt::format("host {}:bad response ", members_[idx]);
			continue;
		}

		size_t o = 1;
		int32_t rcode = 0;
		std::string rmsg;
		ClusterInterface::Wire::getI32(resp.data(), resp.size(), o, rcode);
		ClusterInterface::Wire::getString(resp.data(), resp.size(), o, rmsg);

		if(rcode != 0)
		{
			code = -1;
			detail += fmt::format("host {}:{} ", members_[idx], rmsg);
		}
		else
		{
			detail += fmt::format("host {}:{} ", members_[idx],
				killOp ? "killed" : "stopped");
		}
	}

	if(!detail.empty() && detail[detail.size() - 1] == ' ')
		detail.erase(detail.size() - 1);

	reply(code, detail);
}

//-------------------------------------------------------------------------------------
void ClusterCore::handlePeerMessage(const Event& ev)
{
	const std::string& d = ev.data;
	if(d.size() < 1)
		return;

	size_t off = 0;
	uint8_t type = 0;
	if(!ClusterInterface::Wire::getU8(d.data(), d.size(), off, type))
		return;

	switch(type)
	{
	case PeerProto::PM_VOTE_REQ:
		onVoteRequest(ev.peerIdx, d, off);
		break;
	case PeerProto::PM_VOTE_RESP:
		onVoteResponse(ev.peerIdx, d, off);
		break;
	case PeerProto::PM_APPEND:
		onAppend(ev.peerIdx, d, off);
		break;
	case PeerProto::PM_APPEND_RESP:
		onAppendResponse(ev.peerIdx, d, off);
		break;
	default:
		break;
	}
}

//-------------------------------------------------------------------------------------
void ClusterCore::onVoteRequest(int from, const std::string& d, size_t off)
{
	uint64_t term = 0;
	int32_t candidateIdx = -1;
	uint64_t candLastIdx = 0, candLastTerm = 0;
	if(!ClusterInterface::Wire::getU64(d.data(), d.size(), off, term))
		return;
	if(!ClusterInterface::Wire::getI32(d.data(), d.size(), off, candidateIdx))
		return;
	if(!ClusterInterface::Wire::getU64(d.data(), d.size(), off, candLastIdx))
		return;
	if(!ClusterInterface::Wire::getU64(d.data(), d.size(), off, candLastTerm))
		return;

	if(term > currentTerm_)
	{
		currentTerm_ = term;
		role_ = ROLE_FOLLOWER;
		votedFor_ = -1;
		leaderIdx_ = -1;
		grantVotes_.clear();
	}

	bool grant = false;
	if(term >= currentTerm_ && (votedFor_ == -1 || votedFor_ == candidateIdx))
	{
		// raft "日志新旧"检查: 候选人的日志必须不旧于本机
		uint64 myLastIdx = lastIndex();
		uint64 myLastTerm = snapshotTerm_;
		if(myLastIdx > snapshotIndex_)
			myLastTerm = log_.back().term;

		bool upToDate = candLastTerm > myLastTerm ||
			(candLastTerm == myLastTerm && candLastIdx >= myLastIdx);

		if(upToDate)
		{
			grant = true;
			votedFor_ = candidateIdx;
			electionDeadlineMS_ = nowWallMS() + electionTimeoutMS_ + 500;
		}
	}

	// 发送投票响应
	std::string msg;
	ClusterInterface::Wire::putU8(msg, PeerProto::PM_VOTE_RESP);
	ClusterInterface::Wire::putU64(msg, term);
	ClusterInterface::Wire::putU8(msg, grant ? 1 : 0);
	sendFrameToPeer(from, msg);
}

//-------------------------------------------------------------------------------------
void ClusterCore::onVoteResponse(int from, const std::string& d, size_t off)
{
	uint64_t term = 0;
	uint8_t granted = 0;
	if(!ClusterInterface::Wire::getU64(d.data(), d.size(), off, term))
		return;
	if(!ClusterInterface::Wire::getU8(d.data(), d.size(), off, granted))
		return;

	if(term > currentTerm_)
	{
		currentTerm_ = term;
		role_ = ROLE_FOLLOWER;
		votedFor_ = -1;
		leaderIdx_ = -1;
		grantVotes_.clear();
		return;
	}

	if(role_ != ROLE_CANDIDATE || term != currentTerm_)
		return;

	if(granted != 0)
	{
		// 统计票数(含自己)
		grantVotes_[from] = true;
		int votes = (int)grantVotes_.size();

		if(votes >= (int)(members_.size() / 2 + 1))
		{
			// 当选 leader
			role_ = ROLE_LEADER;
			leaderIdx_ = selfIdx_;
			lastHeartbeatMS_ = 0;
			lastTTLScanMS_ = nowWallMS();

			INFO_MSG(fmt::format("ClusterCore::onVoteResponse(): elected as leader term={} votes={}.\n",
				currentTerm_, votes));

			// 全量广播心跳/复制, 并通知所有成员
			for(size_t i = 0; i < members_.size(); ++i)
			{
				if((int)i == selfIdx_)
					continue;
				sendReplicateToPeer((int)i);
			}
		}
	}
}

//-------------------------------------------------------------------------------------
void ClusterCore::onAppend(int from, const std::string& d, size_t off)
{
	uint64_t term = 0;
	uint64_t leaderCommit = 0;
	uint8_t kind = 0;

	uint32_t tmp32 = 0;
	int32_t leader = -1;

	if(!ClusterInterface::Wire::getU64(d.data(), d.size(), off, term))
		return;
	if(!ClusterInterface::Wire::getU32(d.data(), d.size(), off, tmp32))
		return;
	leader = (int32_t)tmp32;
	if(!ClusterInterface::Wire::getU64(d.data(), d.size(), off, leaderCommit))
		return;
	if(!ClusterInterface::Wire::getU8(d.data(), d.size(), off, kind))
		return;

	if(term < currentTerm_)
	{
		// 拒绝
		std::string msg;
		ClusterInterface::Wire::putU8(msg, PeerProto::PM_APPEND_RESP);
		ClusterInterface::Wire::putU64(msg, currentTerm_);
		ClusterInterface::Wire::putU8(msg, 0);
		ClusterInterface::Wire::putU64(msg, lastIndex());
		sendFrameToPeer(from, msg);
		return;
	}

	// 接受更高/相等 term 的领导
	if(term > currentTerm_ || role_ != ROLE_FOLLOWER)
	{
		currentTerm_ = term;
		role_ = ROLE_FOLLOWER;
		votedFor_ = -1;
	}
	leaderIdx_ = leader;
	electionDeadlineMS_ = nowWallMS() + electionTimeoutMS_ + 500;

	if(kind == 1)
	{
		// 快照
		uint32_t snapLen = 0;
		if(!ClusterInterface::Wire::getU32(d.data(), d.size(), off, snapLen))
			return;

		if((size_t)off + snapLen > d.size())
			return;

		std::string snap = d.substr(off, snapLen);
		decodeSnapshot(snap);
		saveSnapshot(snap);

		INFO_MSG(fmt::format("ClusterCore::onAppend(): snapshot applied, registry={}, index={}.\n",
			registry_.size(), snapshotIndex_));

		std::string msg;
		ClusterInterface::Wire::putU8(msg, PeerProto::PM_APPEND_RESP);
		ClusterInterface::Wire::putU64(msg, currentTerm_);
		ClusterInterface::Wire::putU8(msg, 1);
		ClusterInterface::Wire::putU64(msg, snapshotIndex_);
		sendFrameToPeer(from, msg);
		return;
	}

	// 增量日志
	uint64_t prevIndex = 0, prevTerm = 0;
	if(!ClusterInterface::Wire::getU64(d.data(), d.size(), off, prevIndex))
		return;
	if(!ClusterInterface::Wire::getU64(d.data(), d.size(), off, prevTerm))
		return;

	uint16_t count = 0;
	if(!ClusterInterface::Wire::getU16(d.data(), d.size(), off, count))
		return;

	// 校验 prevLogIndex
	if(prevIndex != lastIndex())
	{
		// 日志缺口 -> 需要 leader 从合适位置重发或给快照
		bool match = false;
		if(prevIndex <= lastIndex() && prevIndex > snapshotIndex_)
		{
			const RaftLogEntry& e = log_[prevIndex - snapshotIndex_ - 1];
			if(e.term == prevTerm)
				match = true;
		}

		if(!match)
		{
			std::string msg;
			ClusterInterface::Wire::putU8(msg, PeerProto::PM_APPEND_RESP);
			ClusterInterface::Wire::putU64(msg, currentTerm_);
			ClusterInterface::Wire::putU8(msg, 0);
			ClusterInterface::Wire::putU64(msg, lastIndex());
			sendFrameToPeer(from, msg);

			// 主动要求快照
			std::lock_guard<std::mutex> lock(peersMutex_);
			// (仅本地标识——leader 侧 by ack=false 会自行判断)
			return;
		}
	}

	uint64_t nowTerm = term;

	// 应用增量
	for(uint16_t i = 0; i < count; ++i)
	{
		uint64_t idx = 0, lterm = 0;
		uint8_t ltype = 0;
		uint32_t len = 0;
		if(!ClusterInterface::Wire::getU64(d.data(), d.size(), off, idx)) return;
		if(!ClusterInterface::Wire::getU64(d.data(), d.size(), off, lterm)) return;
		if(!ClusterInterface::Wire::getU8(d.data(), d.size(), off, ltype)) return;
		if(!ClusterInterface::Wire::getU32(d.data(), d.size(), off, len)) return;
		if((size_t)off + len > d.size()) return;

		RaftLogEntry e;
		e.index = idx;
		e.term = lterm;
		e.type = ltype;
		e.data = d.substr(off, len);
		off += len;

		// 空洞保护：日志必须连续追加，否则让 leader 走快照同步
		if(idx > lastIndex() + 1)
		{
			std::string msg;
			ClusterInterface::Wire::putU8(msg, PeerProto::PM_APPEND_RESP);
			ClusterInterface::Wire::putU64(msg, currentTerm_);
			ClusterInterface::Wire::putU8(msg, 0);
			ClusterInterface::Wire::putU64(msg, lastIndex());
			sendFrameToPeer(from, msg);
			return;
		}

		// 防重复：若本机日志已包含该 index(冲突则截断)
		if(idx <= snapshotIndex_)
		{
			continue;	// 已随快照归档, 无需追加
		}

		if(idx <= lastIndex())
		{
			uint64 pos = idx - snapshotIndex_ - 1;
			if(log_[pos].term != lterm)
			{
				// 冲突，截断自身日志尾部
				log_.resize(pos);
			}
			else
			{
				continue;	// 已存在同 term 日志
			}
		}

		log_.push_back(e);
	}

	(void)nowTerm;

	// 更新 commitIndex(apply 状态机)
	if(leaderCommit > commitIndex_)
	{
		uint64 last = lastIndex();
		commitIndex_ = leaderCommit < last ? leaderCommit : last;
		applyCommitted();
	}

	std::string msg;
	ClusterInterface::Wire::putU8(msg, PeerProto::PM_APPEND_RESP);
	ClusterInterface::Wire::putU64(msg, currentTerm_);
	ClusterInterface::Wire::putU8(msg, 1);
	ClusterInterface::Wire::putU64(msg, lastIndex());
	sendFrameToPeer(from, msg);
}

//-------------------------------------------------------------------------------------
void ClusterCore::onAppendResponse(int from, const std::string& d, size_t off)
{
	uint64_t term = 0;
	uint8_t ack = 0;
	uint64_t index = 0;
	if(!ClusterInterface::Wire::getU64(d.data(), d.size(), off, term))
		return;
	if(!ClusterInterface::Wire::getU8(d.data(), d.size(), off, ack))
		return;
	if(!ClusterInterface::Wire::getU64(d.data(), d.size(), off, index))
		return;

	if(term > currentTerm_)
	{
		currentTerm_ = term;
		role_ = ROLE_FOLLOWER;
		votedFor_ = -1;
		leaderIdx_ = -1;
		return;
	}

	if(role_ != ROLE_LEADER)
		return;

	{
		std::lock_guard<std::mutex> lock(peersMutex_);
		if(from >= 0 && from < (int)peers_.size() && peers_[from].connected)
		{
			if(ack != 0)
			{
				peers_[from].lastAcked = index;
				peers_[from].needSnapshot = false;
			}
			else
			{
				peers_[from].needSnapshot = true;
			}
		}
	}

	advanceCommit();
}

//-------------------------------------------------------------------------------------
void ClusterCore::advanceCommit()
{
	if(role_ != ROLE_LEADER)
		return;

	// 计算多数派
	int majority = (int)(members_.size() / 2 + 1);
	bool progress = true;
	while(progress)
	{
		uint64_t idx = commitIndex_ + 1;
		if(idx > lastIndex())
			break;

		int acks = 1;	// leader 自身
		{
			std::lock_guard<std::mutex> lock(peersMutex_);
			for(size_t i = 0; i < peers_.size(); ++i)
			{
				if((int)i == selfIdx_)
					continue;
				if(peers_[i].connected && peers_[i].lastAcked >= idx)
					acks++;
			}
		}

		if(acks >= majority)
		{
			commitIndex_ = idx;

			// 完成等待该条日志的客户端连接
			std::lock_guard<std::mutex> lock(replyMutex_);
			std::map<uint64_t, uint64_t>::iterator wit = cmdWaiter_.find(idx);
			if(wit != cmdWaiter_.end())
			{
				std::string msg;
				ClusterInterface::Wire::putU8(msg, ClusterInterface::MSG_RESP_OK);
				replies_[wit->second] = msg;
				revWaiter_.erase(wit->second);
				cmdWaiter_.erase(wit);
			}
		}
		else
		{
			progress = false;
		}
	}

	applyCommitted();

	std::lock_guard<std::mutex> lock(replyMutex_);
	replyCv_.notify_all();
}

//-------------------------------------------------------------------------------------
void ClusterCore::applyCommitted()
{
	while(lastApplied_ < commitIndex_)
	{
		uint64_t idx = lastApplied_ + 1;
		if(idx <= snapshotIndex_)
		{
			lastApplied_ = snapshotIndex_;
			continue;
		}

		if(idx - snapshotIndex_ - 1 >= log_.size())
			break;

		const RaftLogEntry& e = log_[idx - snapshotIndex_ - 1];
		applyEntry(e);
		lastApplied_ = idx;
	}
}

//-------------------------------------------------------------------------------------
void ClusterCore::applyEntry(const RaftLogEntry& e)
{
	const std::string& d = e.data;
	size_t off = 0;

	switch(e.type)
	{
	case CMD_REGISTER:
	{
		uint64_t time = 0;
		ClusterInterface::ComponentData comp;
		if(!ClusterInterface::Wire::getU64(d.data(), d.size(), off, time))
			return;
		if(!ClusterInterface::decodeComponentData(d.data(), d.size(), off, comp))
			return;

		RegistryEntry entry;
		entry.data = comp;
		entry.lastRenewal = time;
		std::string key = encodeKey(comp.uid, comp.componentType, (int32)comp.componentID);
		registry_[key] = entry;
		break;
	}

	case CMD_RENEW:
	{
		uint64_t time = 0;
		uint32_t uid = 0, type = 0, pid = 0;
		uint64_t cid = 0;
		if(!ClusterInterface::Wire::getU64(d.data(), d.size(), off, time)) return;
		if(!ClusterInterface::Wire::getU32(d.data(), d.size(), off, uid)) return;
		if(!ClusterInterface::Wire::getU32(d.data(), d.size(), off, type)) return;
		if(!ClusterInterface::Wire::getU64(d.data(), d.size(), off, cid)) return;
		if(!ClusterInterface::Wire::getU32(d.data(), d.size(), off, pid)) return;

		std::string key = encodeKey((int32)uid, (int32)type, (int64)cid);
		std::map<std::string, RegistryEntry>::iterator it = registry_.find(key);
		if(it != registry_.end())
		{
			it->second.data.pid = pid;
			it->second.lastRenewal = time;
		}
		break;
	}

	case CMD_UNREGISTER:
	case CMD_EXPIRE:
	{
		uint32_t uid = 0, type = 0;
		uint64_t cid = 0;
		if(!ClusterInterface::Wire::getU32(d.data(), d.size(), off, uid)) return;
		if(!ClusterInterface::Wire::getU32(d.data(), d.size(), off, type)) return;
		if(!ClusterInterface::Wire::getU64(d.data(), d.size(), off, cid)) return;

		registry_.erase(encodeKey((int32)uid, (int32)type, (int64)cid));
		break;
	}

	default:
		break;
	}
}

//-------------------------------------------------------------------------------------
uint64_t ClusterCore::lastIndex() const
{
	return snapshotIndex_ + (uint64_t)log_.size();
}

//-------------------------------------------------------------------------------------
uint64_t ClusterCore::appendLog(uint8_t type, const std::string& payload)
{
	RaftLogEntry e;
	e.index = lastIndex() + 1;
	e.term = currentTerm_;
	e.type = type;
	e.data = payload;
	log_.push_back(e);

	// 复制到各对端
	for(size_t i = 0; i < members_.size(); ++i)
	{
		if((int)i == selfIdx_)
			continue;
		sendReplicateToPeer((int)i);
	}

	// leader 侧立即尝试提交(自身一票 + 已 ack 对端；单成员即刻提交)
	advanceCommit();

	return e.index;
}

//-------------------------------------------------------------------------------------
void ClusterCore::registerWaiter(uint64 index, uint64 connId)
{
	std::lock_guard<std::mutex> lock(replyMutex_);

	// 单成员(或多数派已先满足)时 appendLog 内部 advanceCommit 可能已把该日志提交,
	// 等待者登记晚于提交, 此处补一次应答; 否则登记等待后续 advanceCommit 提交。
	if(isLeader() && index <= commitIndex_)
	{
		std::string msg;
		ClusterInterface::Wire::putU8(msg, ClusterInterface::MSG_RESP_OK);
		replies_[connId] = msg;
		replyCv_.notify_all();
		return;
	}

	cmdWaiter_[index] = connId;
	revWaiter_[connId] = index;
}

//-------------------------------------------------------------------------------------
void ClusterCore::ttlScan()
{
	if(role_ != ROLE_LEADER)
		return;

	uint64 now = nowWallMS();
	std::vector<std::string> expiredKeys;

	for(std::map<std::string, RegistryEntry>::iterator it = registry_.begin();
		it != registry_.end(); ++it)
	{
		if(it->second.lastRenewal > 0 &&
			now - it->second.lastRenewal >= componentLeaseMS_)
		{
			expiredKeys.push_back(it->first);
		}
	}

	if(expiredKeys.size() == 0)
		return;

	for(size_t i = 0; i < expiredKeys.size(); ++i)
	{
		// 生成过期日志
		const RegistryEntry& e = registry_[expiredKeys[i]];
		const ClusterInterface::ComponentData& c = e.data;

		std::string payload;
		ClusterInterface::Wire::putU32(payload, (uint32_t)c.uid);
		ClusterInterface::Wire::putU32(payload, (uint32_t)c.componentType);
		ClusterInterface::Wire::putU64(payload, c.componentID);
		appendLog(CMD_EXPIRE, payload);
	}

	INFO_MSG(fmt::format("ClusterCore::ttlScan(): expired {} entries.\n", expiredKeys.size()));
}

//-------------------------------------------------------------------------------------
void ClusterCore::redirectNotLeader(uint64 connId)
{
	std::string payload;
	ClusterInterface::Wire::putU32(payload, leaderIdx_ >= 0 ?
		::inet_addr(members_[leaderIdx_].c_str()) : 0);
	ClusterInterface::Wire::putU16(payload, leaderIdx_ >= 0 ? servicePort_ : 0);

	std::string msg;
	ClusterInterface::Wire::putU8(msg, ClusterInterface::MSG_RESP_NOT_LEADER);
	msg += payload;

	std::lock_guard<std::mutex> lock(replyMutex_);
	replies_[connId] = msg;
	replyCv_.notify_all();
}

//-------------------------------------------------------------------------------------
std::string ClusterCore::encodeKey(int32 uid, int32 type, uint64 cid)
{
	std::string key;
	ClusterInterface::Wire::putU32(key, (uint32_t)uid);
	ClusterInterface::Wire::putU32(key, (uint32_t)type);
	ClusterInterface::Wire::putU64(key, cid);
	return key;
}

}
