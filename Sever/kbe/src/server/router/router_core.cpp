// Copyright 2008-2018 Yolo Technologies, Inc. All Rights Reserved. https://www.comblockengine.com

#include "router_core.h"

#include "common/common.h"
#include "common/objectpool.h"

#if KBE_PLATFORM == PLATFORM_WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

#include <chrono>
#include <cstring>

namespace KBEngine {

using namespace RouterInterface;

/*
	目标尚未注册时，是否值得为它开启"待定缓冲"(设计文档 §6.2 / §14.4)。

	唯一刻意排除的是客户端 Proxy Actor —— baseapp 把 proxyId 以
	entityMailbox(eid, CLIENT_TYPE) 注册到客户端连接上，客户端断开时会被注销。
	此时消息应当**快速失败**而不是被缓冲 30s，否则业务侧的"客户端已下线"判断
	会被无谓地推迟。其余(服务器实体 Actor / 组件 Actor / 服务 Actor)都可能
	处于"进程正在启动 / 实体 cell 部分尚未创建"的短暂不可达窗口，正是待定缓冲的用武之地。
*/
static bool shouldBufferPending(const MailboxAddress& dst)
{
	return dst.componentType != (uint32_t)CLIENT_TYPE;
}

//-------------------------------------------------------------------------------------
// 工具
//-------------------------------------------------------------------------------------

static uint64 nowWallMS()
{
	return (uint64)std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::system_clock::now().time_since_epoch()).count();
}

static bool sendAll(Network::EndPoint* ep, const char* data, int len)
{
	int sent = 0;
	while(sent < len)
	{
		int r = ep->send(data + sent, len - sent);
		if(r <= 0)
			return false;

		sent += r;
	}
	return true;
}

// 帧格式：[u32 长度(网络字节序)][载荷]
static bool sendFrameRaw(Network::EndPoint* ep, const std::string& payload)
{
	if(ep == NULL)
		return false;

	uint32 len = (uint32)payload.size();

	char hdr[4];
	hdr[0] = (char)((len >> 24) & 0xFF);
	hdr[1] = (char)((len >> 16) & 0xFF);
	hdr[2] = (char)((len >> 8) & 0xFF);
	hdr[3] = (char)(len & 0xFF);

	if(!sendAll(ep, hdr, 4))
		return false;

	if(len > 0 && !sendAll(ep, payload.data(), (int)len))
		return false;

	return true;
}

static bool recvFrame(Network::EndPoint* ep, std::string& out)
{
	if(ep == NULL)
		return false;

	uint8 lenBuf[4];
	if(!ep->recvAll(lenBuf, 4))
		return false;

	uint32 len = ((uint32)lenBuf[0] << 24) | ((uint32)lenBuf[1] << 16) |
				 ((uint32)lenBuf[2] << 8) | (uint32)lenBuf[3];

	if(len == 0 || len > ROUTER_FRAME_MAX)
		return false;

	out.resize(len);
	if(!ep->recvAll(&out[0], (int)len))
		return false;

	return true;
}

//-------------------------------------------------------------------------------------
RouterCore::RouterCore(const std::string& selfIP, uint16 clientPort, uint16 serverPort,
	uint32 suspendTimeoutMS, uint32 actorBufferMax,
	uint32 pendingTimeoutMS, uint32 pendingBufferMax)
	: selfIP_(selfIP),
	  clientPort_(clientPort),
	  serverPort_(serverPort),
	  suspendTimeoutMS_(suspendTimeoutMS),
	  actorBufferMax_(actorBufferMax),
	  pendingTimeoutMS_(pendingTimeoutMS),
	  pendingBufferMax_(pendingBufferMax),
	  running_(false),
	  clientListenEP_(NULL),
	  serverListenEP_(NULL),
	  nextConnId_(1),
	  counterSent_(0),
	  counterForwarded_(0),
	  counterFailed_(0),
	  counterPending_(0),
	  counterFlushed_(0)
{
}

//-------------------------------------------------------------------------------------
RouterCore::~RouterCore()
{
	stop();
}

//-------------------------------------------------------------------------------------
bool RouterCore::start()
{
	if(running_.load())
		return false;

	uint32 hostAddr = ::inet_addr(selfIP_.c_str());
	if(hostAddr == (uint32)INADDR_NONE)
	{
		ERROR_MSG(fmt::format("RouterCore::start(): bad self ip '{}'\n", selfIP_));
		return false;
	}

	running_ = true;

	// ---- 客户端接入监听 ----
	clientListenEP_ = Network::EndPoint::createPoolObject(OBJECTPOOL_POINT);
	clientListenEP_->socket(SOCK_STREAM);

	if(!clientListenEP_->good())
	{
		ERROR_MSG(fmt::format("RouterCore::start(): create client listener failed!\n"));
		clientListenEP_->close();
		Network::EndPoint::reclaimPoolObject(clientListenEP_);
		clientListenEP_ = NULL;
		running_ = false;
		return false;
	}

	if(clientListenEP_->bind(htons(clientPort_), hostAddr) == -1)
	{
		ERROR_MSG(fmt::format("RouterCore::start(): bind clientPort({}) failed!\n", (int)clientPort_));
		clientListenEP_->close();
		Network::EndPoint::reclaimPoolObject(clientListenEP_);
		clientListenEP_ = NULL;
		running_ = false;
		return false;
	}

	if(clientListenEP_->listen(SOMAXCONN) == -1)
	{
		ERROR_MSG(fmt::format("RouterCore::start(): listen clientPort({}) failed!\n", (int)clientPort_));
		clientListenEP_->close();
		Network::EndPoint::reclaimPoolObject(clientListenEP_);
		clientListenEP_ = NULL;
		running_ = false;
		return false;
	}

	// ---- 服务器组件接入监听 ----
	serverListenEP_ = Network::EndPoint::createPoolObject(OBJECTPOOL_POINT);
	serverListenEP_->socket(SOCK_STREAM);

	if(!serverListenEP_->good() ||
		serverListenEP_->bind(htons(serverPort_), hostAddr) == -1 ||
		serverListenEP_->listen(SOMAXCONN) == -1)
	{
		ERROR_MSG(fmt::format("RouterCore::start(): listen serverPort({}) failed!\n", (int)serverPort_));
		serverListenEP_->close();
		Network::EndPoint::reclaimPoolObject(serverListenEP_);
		serverListenEP_ = NULL;

		clientListenEP_->close();
		Network::EndPoint::reclaimPoolObject(clientListenEP_);
		clientListenEP_ = NULL;

		running_ = false;
		return false;
	}

	// ---- 启动接入线程 ----
	Network::EndPoint* clientEP = clientListenEP_;
	Network::EndPoint* serverEP = serverListenEP_;

	if(!spawn([this, clientEP]() { acceptorLoop(clientEP, "client", PEER_CLIENT); }))
	{
		ERROR_MSG(fmt::format("RouterCore::start(): spawn client acceptor failed!\n"));
		stop();
		return false;
	}

	if(!spawn([this, serverEP]() { acceptorLoop(serverEP, "server", PEER_COMPONENT); }))
	{
		ERROR_MSG(fmt::format("RouterCore::start(): spawn server acceptor failed!\n"));
		stop();
		return false;
	}

	INFO_MSG(fmt::format("RouterCore::start(): ok, self={}, clientPort={}, serverPort={}, "
		"suspendTimeoutMS={}, actorBufferMax={}, pendingTimeoutMS={}, pendingBufferMax={}\n",
		selfIP_, (int)clientPort_, (int)serverPort_, suspendTimeoutMS_, actorBufferMax_,
		pendingTimeoutMS_, pendingBufferMax_));

	return true;
}

//-------------------------------------------------------------------------------------
void RouterCore::stop()
{
	if(!running_.exchange(false))
		return;

	// 1) 关闭监听 -> accept 返回失败 -> acceptor 线程退出
	if(clientListenEP_) clientListenEP_->close();
	if(serverListenEP_) serverListenEP_->close();

	// 2) 关闭所有连接 -> recvAll 返回失败 -> 连接线程退出
	//    (在 sendMutex 内置 alive=false，保证主线程不会向已关闭的 socket 发送)
	for(std::map<uint64_t, ConnectionPtr>::iterator it = conns_.begin(); it != conns_.end(); ++it)
	{
		ConnectionPtr conn = it->second;
		if(!conn) continue;

		std::lock_guard<std::mutex> lock(conn->sendMutex);
		if(conn->alive && conn->ep)
		{
			conn->alive = false;
			conn->ep->close();
		}
	}

	// 3) 回收线程
	{
		std::lock_guard<std::mutex> lock(threadsMutex_);
		for(size_t i = 0; i < threads_.size(); ++i)
		{
			if(threads_[i].joinable())
				threads_[i].join();
		}
		threads_.clear();
	}

	// 4) 回收监听端点
	if(clientListenEP_)
	{
		Network::EndPoint::reclaimPoolObject(clientListenEP_);
		clientListenEP_ = NULL;
	}

	if(serverListenEP_)
	{
		Network::EndPoint::reclaimPoolObject(serverListenEP_);
		serverListenEP_ = NULL;
	}

	conns_.clear();

	if(registry_.pendingMailCount() > 0)
	{
		WARNING_MSG(fmt::format("RouterCore::stop(): dropping {} pending mail(s) for {} unreachable actor(s).\n",
			registry_.pendingMailCount(), registry_.pendingCount()));
	}

	registry_.clear();

	INFO_MSG(fmt::format("RouterCore::stop(): done.\n"));
}

//-------------------------------------------------------------------------------------
bool RouterCore::spawn(const std::function<void()>& fn)
{
	std::lock_guard<std::mutex> lock(threadsMutex_);

	if(!running_.load())
		return false;

	threads_.push_back(std::thread(fn));
	return true;
}

//-------------------------------------------------------------------------------------
void RouterCore::acceptorLoop(Network::EndPoint* listenEP, const char* tag, uint8_t defaultPeerKind)
{
	INFO_MSG(fmt::format("RouterCore::acceptorLoop({}): thread started.\n", tag));

	while(running_.load())
	{
		if(listenEP == NULL)
			break;

		u_int16_t port = 0;
		u_int32_t addr = 0;

		Network::EndPoint* newEP = listenEP->accept(&port, &addr, false);
		if(newEP == NULL)
		{
			if(!running_.load())
				break;

			std::this_thread::sleep_for(std::chrono::milliseconds(50));
			continue;
		}

		uint64_t connId = nextConnId_.fetch_add(1);

		ConnectionPtr conn(new Connection());
		conn->connId = connId;
		conn->peerKind = defaultPeerKind;
		conn->ep = newEP;
		conn->alive = true;
		conn->peerName = tag;

		{
			Event ev;
			ev.type = EVENT_CONN_OPENED;
			ev.connId = connId;
			ev.conn = conn;

			std::lock_guard<std::mutex> lock(queueMutex_);
			queue_.push_back(ev);
		}

		if(!spawn([this, conn]() { connThread(conn); }))
		{
			std::lock_guard<std::mutex> lock(conn->sendMutex);
			conn->alive = false;
			if(conn->ep)
			{
				conn->ep->close();
				Network::EndPoint::reclaimPoolObject(conn->ep);
				conn->ep = NULL;
			}
		}
	}

	INFO_MSG(fmt::format("RouterCore::acceptorLoop({}): thread exit.\n", tag));
}

//-------------------------------------------------------------------------------------
void RouterCore::connThread(ConnectionPtr conn)
{
	std::string data;

	while(running_.load())
	{
		if(!recvFrame(conn->ep, data))
			break;

		Event ev;
		ev.type = EVENT_INBOUND;
		ev.connId = conn->connId;
		ev.data.swap(data);

		{
			std::lock_guard<std::mutex> lock(queueMutex_);
			queue_.push_back(ev);
		}
	}

	// 连接结束：锁内标记死亡并关闭端点，保证主线程不会向已关闭的 socket 发送
	{
		std::lock_guard<std::mutex> lock(conn->sendMutex);
		conn->alive = false;
		if(conn->ep)
		{
			conn->ep->close();
			Network::EndPoint::reclaimPoolObject(conn->ep);
			conn->ep = NULL;
		}
	}

	{
		Event ev;
		ev.type = EVENT_CONN_CLOSED;
		ev.connId = conn->connId;

		std::lock_guard<std::mutex> lock(queueMutex_);
		queue_.push_back(ev);
	}
}

//-------------------------------------------------------------------------------------
void RouterCore::tick()
{
	drainEvents();
	checkSuspendTimeouts();
	checkPendingTimeouts();
}

//-------------------------------------------------------------------------------------
void RouterCore::drainEvents()
{
	std::vector<Event> local;
	{
		std::lock_guard<std::mutex> lock(queueMutex_);
		local.swap(queue_);
	}

	for(size_t i = 0; i < local.size(); ++i)
	{
		Event& ev = local[i];

		switch(ev.type)
		{
		case EVENT_CONN_OPENED:
			handleConnOpened(ev.conn);
			break;
		case EVENT_INBOUND:
			{
				ConnectionPtr conn = findConn(ev.connId);
				if(conn)
					handleInbound(conn, ev.data);
			}
			break;
		case EVENT_CONN_CLOSED:
			handleConnClosed(ev.connId);
			break;
		default:
			break;
		}
	}
}

//-------------------------------------------------------------------------------------
void RouterCore::handleConnOpened(const ConnectionPtr& conn)
{
	if(!conn)
		return;

	conns_[conn->connId] = conn;

	INFO_MSG(fmt::format("RouterCore: connection opened, connId={}, from={} (waiting HELLO).\n",
		conn->connId, conn->peerName));
}

//-------------------------------------------------------------------------------------
void RouterCore::handleConnClosed(uint64_t connId)
{
	std::map<uint64_t, ConnectionPtr>::iterator it = conns_.find(connId);
	if(it == conns_.end())
		return;

	std::string peerName = it->second ? it->second->peerName : "unknown";
	conns_.erase(it);

	// 兜底清理：删除该连接名下所有 Actor / Service 注册，避免路由表残留死映射
	std::vector<ActorKey> removedActors;
	std::vector<std::string> removedServices;
	registry_.removeConn(connId, removedActors, removedServices);

	WARNING_MSG(fmt::format("RouterCore: connection closed, connId={}, peer={}, "
		"unregistered {} actor(s), {} service(s).\n",
		connId, peerName, removedActors.size(), removedServices.size()));
}

//-------------------------------------------------------------------------------------
void RouterCore::handleInbound(ConnectionPtr conn, const std::string& data)
{
	if(data.empty())
		return;

	uint8_t type = (uint8_t)data[0];
	std::string payload = data.substr(1);

	switch(type)
	{
	case MSG_HELLO:				onHello(conn, payload); break;
	case MSG_PING:				replyPong(conn); break;
	case MSG_REGISTER_ACTOR:	onRegisterActor(conn, payload); break;
	case MSG_UNREGISTER_ACTOR:	onUnregisterActor(conn, payload); break;
	case MSG_SUSPEND_ACTOR:		onSuspendActor(conn, payload); break;
	case MSG_RESUME_ACTOR:		onResumeActor(conn, payload); break;
	case MSG_REGISTER_SERVICE:	onRegisterService(conn, payload); break;
	case MSG_FIND_SERVICE:		onFindService(conn, payload); break;
	case MSG_SEND:				onSend(conn, payload); break;
	default:
		WARNING_MSG(fmt::format("RouterCore: unknown message type {}, connId={}.\n",
			(int)type, conn->connId));
		replyError(conn, "unknown message type");
		break;
	}
}

//-------------------------------------------------------------------------------------
void RouterCore::checkSuspendTimeouts()
{
	std::vector<FlushResult> expired;
	registry_.takeExpiredSuspends(nowWallMS(), expired);

	for(size_t i = 0; i < expired.size(); ++i)
	{
		WARNING_MSG(fmt::format("RouterCore: actor(kind={}, id={}) suspend timed out, "
			"rollback to connId={} with {} buffered mail(s).\n",
			(int)expired[i].addr.kind, expired[i].addr.actorId,
			expired[i].connId, expired[i].mails.size()));

		flushMails(expired[i]);
	}
}

//-------------------------------------------------------------------------------------
void RouterCore::checkPendingTimeouts()
{
	std::vector<ExpiredPendingMail> expired;
	registry_.takeExpiredPendings(nowWallMS(), expired);

	if(expired.empty())
		return;

	// 待定缓冲里的每条 Mail 可能有不同的发送方，因此逐条回报
	// (设计文档 §8.3：失败可见是业务侧自建可靠性的硬前提)
	for(size_t i = 0; i < expired.size(); ++i)
	{
		++counterFailed_;
		reportDeliveryFailedTo(expired[i].srcConnId, expired[i].src, expired[i].dst,
			expired[i].msgId, DELIVERY_DROPPED_PENDING);
	}

	WARNING_MSG(fmt::format("RouterCore: {} pending mail(s) expired (target never registered within {}ms).\n",
		expired.size(), pendingTimeoutMS_));
}

//-------------------------------------------------------------------------------------
// 消息处理
//-------------------------------------------------------------------------------------

void RouterCore::onHello(ConnectionPtr conn, const std::string& payload)
{
	size_t off = 0;
	uint8_t peerKind = 0;
	uint32_t uid = 0;
	uint32_t componentType = 0;
	uint64_t componentId = 0;
	uint16_t version = 0;

	const char* d = payload.data();
	size_t n = payload.size();

	if(!Wire::getU8(d, n, off, peerKind) ||
		!Wire::getU32(d, n, off, uid) ||
		!Wire::getU32(d, n, off, componentType) ||
		!Wire::getU64(d, n, off, componentId) ||
		!Wire::getU16(d, n, off, version))
	{
		replyError(conn, "bad HELLO");
		return;
	}

	if(version != ROUTER_PROTOCOL_VERSION)
	{
		replyError(conn, "protocol version mismatch");
		return;
	}

	conn->peerKind = peerKind;
	conn->uid = (int32_t)uid;
	conn->componentType = (int32_t)componentType;
	conn->componentId = componentId;

	char namebuf[128];
	kbe_snprintf(namebuf, 128, "%s#%llu",
		COMPONENT_NAME_EX((COMPONENT_TYPE)conn->componentType),
		(unsigned long long)componentId);
	conn->peerName = namebuf;

	std::string resp;
	Wire::putU64(resp, conn->connId);
	sendFrame(conn, encodeMessage(MSG_RESP_HELLO_OK, resp));

	INFO_MSG(fmt::format("RouterCore: HELLO ok, connId={}, peerKind={}, peer={}, uid={}.\n",
		conn->connId, (int)peerKind, conn->peerName, (int)uid));
}

//-------------------------------------------------------------------------------------
void RouterCore::onRegisterActor(ConnectionPtr conn, const std::string& payload)
{
	size_t off = 0;
	MailboxAddress addr;
	uint64_t targetConnId = 0;

	if(!getMailbox(payload.data(), payload.size(), off, addr) ||
		!Wire::getU64(payload.data(), payload.size(), off, targetConnId))
	{
		replyError(conn, "bad REGISTER_ACTOR");
		return;
	}

	// targetConnId != 0：把该 Actor 绑定到指定连接(典型场景：baseapp 把 proxyId 绑定到客户端连接)
	uint64_t ownerConn = (targetConnId != 0) ? targetConnId : conn->connId;
	uint64_t now = nowWallMS();

	if(targetConnId != 0 && findConn(targetConnId) == NULL)
	{
		replyError(conn, "target connection not found");
		return;
	}

	// 该 Actor 正处于迁移挂起时，"注册"等价于 RESUME：保留并冲刷缓冲
	if(registry_.isSuspended(addr))
	{
		FlushResult fr;
		registry_.resumeActor(addr, ownerConn, now, fr);
		replyOk(conn);
		flushMails(fr);
		return;
	}

	// 该 Actor 此前从未注册、但有 Mail 被暂存在待定缓冲里：
	// "注册"就是它变为可达的时刻，按到达顺序冲刷(设计文档 §6.2 / §14.4)。
	// 注意顺序：必须先 takePending() 再 bindActor()，因为 bindActor() 会清理待定状态。
	if(registry_.hasPending(addr))
	{
		FlushResult fr;
		registry_.takePending(addr, ownerConn, fr);

		registry_.bindActor(addr, ownerConn, addr.componentType, now);
		replyOk(conn);

		INFO_MSG(fmt::format("RouterCore: actor(kind={}, id={}) registered on connId={}, "
			"flushing {} pending mail(s).\n",
			(int)addr.kind, addr.actorId, ownerConn, fr.mails.size()));

		flushMails(fr);
		return;
	}

	registry_.bindActor(addr, ownerConn, addr.componentType, now);
	replyOk(conn);
}

//-------------------------------------------------------------------------------------
void RouterCore::onUnregisterActor(ConnectionPtr conn, const std::string& payload)
{
	size_t off = 0;
	MailboxAddress addr;

	if(!getMailbox(payload.data(), payload.size(), off, addr))
	{
		replyError(conn, "bad UNREGISTER_ACTOR");
		return;
	}

	// Owner 校验：只允许当前宿主注销自己的 Actor。
	//
	// 为什么必须校验：实体跨 cellapp 迁移成功时，"目标 cellapp 注册同一地址"必然发生在
	// "源 cellapp 销毁实体并注销"之前。若无条件注销，源进程会随后把目标进程刚建立的路由
	// 抹掉，导致该实体之后的所有消息全部 TARGET_NOT_FOUND。
	// 由非 Owner 发起的注销不是错误(是迁移的正常时序)，因此静默忽略并正常应答。
	const ActorEntry* entry = registry_.findActor(addr);
	if(entry != NULL && entry->connId != conn->connId)
	{
		INFO_MSG(fmt::format("RouterCore: ignore UNREGISTER_ACTOR from non-owner, "
			"actor(kind={}, id={}) is owned by connId={}, requester connId={}.\n",
			(int)addr.kind, addr.actorId, entry->connId, conn->connId));

		replyOk(conn);
		return;
	}

	registry_.unbindActor(addr);
	registry_.dropSuspended(addr);
	replyOk(conn);
}

//-------------------------------------------------------------------------------------
void RouterCore::onSuspendActor(ConnectionPtr conn, const std::string& payload)
{
	size_t off = 0;
	MailboxAddress addr;

	if(!getMailbox(payload.data(), payload.size(), off, addr))
	{
		replyError(conn, "bad SUSPEND_ACTOR");
		return;
	}

	// Owner 校验：只有当前宿主才可能在迁移开始时挂起自己的 Actor。
	// 与 UNREGISTER 同理，防止非 Owner 误挂起把别人的路由变成"只缓冲不投递"。
	const ActorEntry* entry = registry_.findActor(addr);
	if(entry == NULL || entry->connId != conn->connId)
	{
		replyError(conn, "suspend failed: not owner");
		return;
	}

	if(!registry_.suspendActor(addr, nowWallMS(), suspendTimeoutMS_))
	{
		replyError(conn, "suspend failed: actor not registered");
		return;
	}

	INFO_MSG(fmt::format("RouterCore: actor(kind={}, id={}) suspended, mails will be buffered.\n",
		(int)addr.kind, addr.actorId));

	replyOk(conn);
}

//-------------------------------------------------------------------------------------
void RouterCore::onResumeActor(ConnectionPtr conn, const std::string& payload)
{
	size_t off = 0;
	MailboxAddress addr;

	if(!getMailbox(payload.data(), payload.size(), off, addr))
	{
		replyError(conn, "bad RESUME_ACTOR");
		return;
	}

	FlushResult fr;
	registry_.resumeActor(addr, conn->connId, nowWallMS(), fr);

	INFO_MSG(fmt::format("RouterCore: actor(kind={}, id={}) resumed on connId={}, flushing {} mail(s).\n",
		(int)addr.kind, addr.actorId, conn->connId, fr.mails.size()));

	replyOk(conn);
	flushMails(fr);
}

//-------------------------------------------------------------------------------------
void RouterCore::onRegisterService(ConnectionPtr conn, const std::string& payload)
{
	size_t off = 0;
	std::string name;
	MailboxAddress addr;

	if(!Wire::getString(payload.data(), payload.size(), off, name) ||
		!getMailbox(payload.data(), payload.size(), off, addr))
	{
		replyError(conn, "bad REGISTER_SERVICE");
		return;
	}

	registry_.bindService(name, addr);
	replyOk(conn);

	INFO_MSG(fmt::format("RouterCore: service '{}' registered by connId={} -> actor(kind={}, id={}).\n",
		name, conn->connId, (int)addr.kind, addr.actorId));
}

//-------------------------------------------------------------------------------------
void RouterCore::onFindService(ConnectionPtr conn, const std::string& payload)
{
	size_t off = 0;
	std::string name;

	if(!Wire::getString(payload.data(), payload.size(), off, name))
	{
		replyError(conn, "bad FIND_SERVICE");
		return;
	}

	std::vector<MailboxAddress> list = registry_.findService(name);
	if(list.empty())
	{
		replyNotFound(conn);
		return;
	}

	std::string resp;
	Wire::putU16(resp, (uint16_t)list.size());
	for(size_t i = 0; i < list.size(); ++i)
		putMailbox(resp, list[i]);

	sendFrame(conn, encodeMessage(MSG_RESP_SERVICE, resp));
}

//-------------------------------------------------------------------------------------
void RouterCore::onSend(ConnectionPtr conn, const std::string& payload)
{
	// 载荷 = dst(13) + src(13) + msgId(8) + body(...)
	if(payload.size() < SEND_HEADER_WIRE_SIZE)
	{
		reportDeliveryFailed(conn, MailboxAddress(), MailboxAddress(), 0, DELIVERY_BAD_REQUEST);
		return;
	}

	const char* d = payload.data();
	size_t n = payload.size();
	size_t off = 0;

	MailboxAddress dst;
	MailboxAddress src;
	uint64_t msgId = 0;

	if(!getMailbox(d, n, off, dst) ||
		!getMailbox(d, n, off, src) ||
		!Wire::getU64(d, n, off, msgId))
	{
		reportDeliveryFailed(conn, src, dst, msgId, DELIVERY_BAD_REQUEST);
		return;
	}

	// 1) 目标处于迁移挂起 -> 进入缓冲(设计文档 §6.3)
	if(registry_.isSuspended(dst))
	{
		bool dropped = false;
		registry_.bufferMail(dst, payload, (size_t)actorBufferMax_, dropped);

		if(dropped)
		{
			++counterFailed_;
			reportDeliveryFailed(conn, src, dst, msgId, DELIVERY_DROPPED_SUSPENDED);
		}

		return;
	}

	// 2) 查路由表
	const ActorEntry* entry = registry_.findActor(dst);
	if(entry == NULL)
	{
		// 2a) 目标尚未注册：暂存到待定缓冲，等它首次注册时冲刷(设计文档 §6.2 / §14.4)。
		//
		// 为什么不能立即可靠地回 TARGET_NOT_FOUND：
		// "组件还没连上"、"实体 cell 部分还没建好" 都是正常的短暂窗口，
		// 旧通路靠 ForwardComponent_MessageBuffer / entity_messages_forward_handler
		// 在业务侧入队等待；收敛到 Router 后由这里统一承担。
		// 代价是"真正不存在的目标"延迟到 TTL 才失败，故用有界队列 + TTL 把风险封顶。
		if(pendingBufferMax_ > 0 && shouldBufferPending(dst))
		{
			PendingMail droppedMail;
			bool dropped = false;

			registry_.bufferPendingMail(dst, payload, src, msgId, conn->connId,
				nowWallMS(), pendingTimeoutMS_, (size_t)pendingBufferMax_,
				droppedMail, dropped);

			++counterPending_;

			if(dropped)
			{
				++counterFailed_;
				// 被丢弃的是队列里最旧的一条，回报给它的发送方(发送方可能已断开，静默忽略)
				reportDeliveryFailedTo(droppedMail.srcConnId, droppedMail.src, dst,
					droppedMail.msgId, DELIVERY_DROPPED_PENDING);
			}

			return;
		}

		++counterFailed_;
		reportDeliveryFailed(conn, src, dst, msgId, DELIVERY_TARGET_NOT_FOUND);
		return;
	}

	// 3) 原样转发(router 不解释 body，是"TCP 的延伸")
	std::string frame = encodeMessage(MSG_SEND, payload);
	if(!deliverTo(entry->connId, frame))
	{
		++counterFailed_;
		reportDeliveryFailed(conn, src, dst, msgId, DELIVERY_TARGET_UNREACHABLE);
		return;
	}

	++counterForwarded_;
}

//-------------------------------------------------------------------------------------
// 收发
//-------------------------------------------------------------------------------------

RouterCore::ConnectionPtr RouterCore::findConn(uint64_t connId)
{
	std::map<uint64_t, ConnectionPtr>::iterator it = conns_.find(connId);
	if(it == conns_.end())
		return ConnectionPtr();

	return it->second;
}

//-------------------------------------------------------------------------------------
bool RouterCore::sendFrame(const ConnectionPtr& conn, const std::string& payload)
{
	if(!conn)
		return false;

	std::lock_guard<std::mutex> lock(conn->sendMutex);

	if(!conn->alive || conn->ep == NULL)
		return false;

	if(!sendFrameRaw(conn->ep, payload))
	{
		conn->alive = false;
		return false;
	}

	++counterSent_;
	return true;
}

//-------------------------------------------------------------------------------------
bool RouterCore::deliverTo(uint64_t connId, const std::string& payload)
{
	return sendFrame(findConn(connId), payload);
}

//-------------------------------------------------------------------------------------
void RouterCore::replyOk(const ConnectionPtr& conn)
{
	sendFrame(conn, encodeMessage(MSG_RESP_OK, std::string()));
}

//-------------------------------------------------------------------------------------
void RouterCore::replyPong(const ConnectionPtr& conn)
{
	sendFrame(conn, encodeMessage(MSG_RESP_PONG, std::string()));
}

//-------------------------------------------------------------------------------------
void RouterCore::replyNotFound(const ConnectionPtr& conn)
{
	sendFrame(conn, encodeMessage(MSG_RESP_NOT_FOUND, std::string()));
}

//-------------------------------------------------------------------------------------
void RouterCore::replyError(const ConnectionPtr& conn, const std::string& msg)
{
	std::string payload;
	Wire::putString(payload, msg);
	sendFrame(conn, encodeMessage(MSG_RESP_ERROR, payload));
}

//-------------------------------------------------------------------------------------
void RouterCore::reportDeliveryFailed(const ConnectionPtr& conn,
	const MailboxAddress& src, const MailboxAddress& dst,
	uint64_t msgId, uint32_t code)
{
	sendDeliveryFailed(conn, src, dst, msgId, code);
}

//-------------------------------------------------------------------------------------
void RouterCore::reportDeliveryFailedTo(uint64_t connId,
	const MailboxAddress& src, const MailboxAddress& dst,
	uint64_t msgId, uint32_t code)
{
	ConnectionPtr conn = findConn(connId);
	if(!conn)
	{
		// 发送方已断开：失败回报无法送达(符合设计文档 §8 的取舍：业务侧超时兜底)
		WARNING_MSG(fmt::format("RouterCore: delivery failed(code={}), src(kind={}, id={}) -> dst(kind={}, id={}), "
			"but sender connId={} is gone.\n",
			(int)code, (int)src.kind, src.actorId, (int)dst.kind, dst.actorId, connId));
		return;
	}

	sendDeliveryFailed(conn, src, dst, msgId, code);
}

//-------------------------------------------------------------------------------------
void RouterCore::sendDeliveryFailed(const ConnectionPtr& conn,
	const MailboxAddress& src, const MailboxAddress& dst,
	uint64_t msgId, uint32_t code)
{
	// 载荷 = src(13) + dst(13) + msgId(8) + code(4)
	std::string payload;
	putMailbox(payload, src);
	putMailbox(payload, dst);
	Wire::putU64(payload, msgId);
	Wire::putU32(payload, code);

	sendFrame(conn, encodeMessage(MSG_DELIVERY_FAILED, payload));

	if(src.actorId != 0 || dst.actorId != 0)
	{
		WARNING_MSG(fmt::format("RouterCore: delivery failed(code={}), src(kind={}, id={}) -> dst(kind={}, id={}), msgId={}.\n",
			(int)code, (int)src.kind, src.actorId, (int)dst.kind, dst.actorId, msgId));
	}
}

//-------------------------------------------------------------------------------------
void RouterCore::flushMails(const FlushResult& fr)
{
	counterFlushed_ += (uint64_t)fr.mails.size();

	for(size_t i = 0; i < fr.mails.size(); ++i)
	{
		std::string frame = encodeMessage(MSG_SEND, fr.mails[i]);

		if(deliverTo(fr.connId, frame))
		{
			++counterForwarded_;
		}
		else
		{
			// 冲刷失败时无法回报原始发送方(发送方信息在 body 之外，此处不解析)，
			// 符合设计文档 §8 的取舍：可靠性由业务侧的超时重试保证。
			++counterFailed_;
			WARNING_MSG(fmt::format("RouterCore: flush mail to connId={} failed (actor id={}).\n",
				fr.connId, fr.addr.actorId));
		}
	}
}

//-------------------------------------------------------------------------------------
}
