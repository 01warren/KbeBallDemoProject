// Copyright 2008-2018 Yolo Technologies, Inc. All Rights Reserved. https://www.comblockengine.com

#include "router_client.h"

#include "common/common.h"
#include "common/objectpool.h"
#include "helper/debug_helper.h"

#if KBE_PLATFORM == PLATFORM_WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

#include <chrono>
#include <cstdlib>
#include <cstring>

namespace KBEngine{

using namespace RouterInterface;

//-------------------------------------------------------------------------------------
// 帧收发工具(与 server/router/router_core.cpp 保持同一线格式)
//-------------------------------------------------------------------------------------

namespace {

const int RECONNECT_DELAY_MS = 1000;
const int CONNECT_RETRY_DELAY_MS = 500;

inline void sleepMS(int ms)
{
	std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

bool sendAll(Network::EndPoint* ep, const char* data, int len)
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
bool sendFrameRaw(Network::EndPoint* ep, const std::string& payload)
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

bool recvFrame(Network::EndPoint* ep, std::string& out)
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

} // anonymous namespace

//-------------------------------------------------------------------------------------
RouterClient::RouterClient(COMPONENT_TYPE componentType, COMPONENT_ID componentId)
	: componentType_(componentType),
	  componentId_(componentId),
	  running_(false),
	  ready_(false),
	  thread_(NULL),
	  ep_(NULL),
	  alive_(false),
	  connId_(0)
{
}

//-------------------------------------------------------------------------------------
RouterClient::~RouterClient()
{
	stop();
}

//-------------------------------------------------------------------------------------
bool RouterClient::parseRouterAddr(const std::string& s, uint16 defaultPort, RouterAddr& out)
{
	// 手工 trim，避免依赖引擎的 strutil 细节
	size_t b = 0;
	size_t e = s.size();
	while(b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n'))
		++b;
	while(e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n'))
		--e;

	if(b >= e)
		return false;

	std::string t = s.substr(b, e - b);

	size_t colon = t.find(':');
	if(colon == std::string::npos)
	{
		out.ip = t;
		out.port = defaultPort;
	}
	else
	{
		out.ip = t.substr(0, colon);

		std::string ps = t.substr(colon + 1);
		if(ps.empty())
			return false;

		int p = ::atoi(ps.c_str());
		if(p <= 0 || p > 65535)
			return false;

		out.port = (uint16)p;
	}

	return !out.ip.empty();
}

//-------------------------------------------------------------------------------------
bool RouterClient::start(const std::vector< std::string >& addrs, uint16 defaultPort)
{
	if(running_.load())
	{
		WARNING_MSG(fmt::format("RouterClient::start(): already running, ignore.\n"));
		return false;
	}

	endpoints_.clear();

	for(size_t i = 0; i < addrs.size(); ++i)
	{
		RouterAddr a;
		if(parseRouterAddr(addrs[i], defaultPort, a))
			endpoints_.push_back(a);
		else
			WARNING_MSG(fmt::format("RouterClient::start(): skip bad router address '{}'.\n", addrs[i]));
	}

	if(endpoints_.empty())
	{
		RouterAddr a;
		a.ip = "127.0.0.1";
		a.port = defaultPort;
		endpoints_.push_back(a);
	}

	{
		std::lock_guard<std::mutex> lock(queueMutex_);
		queue_.clear();
	}

	pendingFindService_.clear();

	running_ = true;
	thread_ = new std::thread(&RouterClient::netThreadMain, this);

	INFO_MSG(fmt::format("RouterClient::start(): component={}({}), router addresses={}, defaultPort={}.\n",
		COMPONENT_NAME_EX(componentType_), (uint64)componentId_, (int)endpoints_.size(), (int)defaultPort));

	return true;
}

//-------------------------------------------------------------------------------------
void RouterClient::stop()
{
	if(!running_.exchange(false))
		return;

	// 关闭底层 socket，使网络线程中阻塞的 recvAll 立即返回
	{
		std::lock_guard<std::mutex> lock(sendMutex_);
		alive_ = false;
		if(ep_ != NULL)
		{
			ep_->close();
			// 注意：ep_ 的清零与回收由网络线程负责，此处只负责唤醒它
		}
	}

	if(thread_ != NULL)
	{
		if(thread_->joinable())
			thread_->join();

		delete thread_;
		thread_ = NULL;
	}

	ready_ = false;

	INFO_MSG(fmt::format("RouterClient::stop(): component={}({}) disconnected.\n",
		COMPONENT_NAME_EX(componentType_), (uint64)componentId_));
}

//-------------------------------------------------------------------------------------
uint64_t RouterClient::connId() const
{
	// connId_ 由网络线程在 sendMutex_ 内写入；此处仅为诊断用途，读取一致即可
	const_cast<RouterClient*>(this)->sendMutex_.lock();
	uint64_t id = connId_;
	const_cast<RouterClient*>(this)->sendMutex_.unlock();
	return id;
}

//-------------------------------------------------------------------------------------
// 网络线程
//-------------------------------------------------------------------------------------

void RouterClient::netThreadMain()
{
	while(running_.load())
	{
		// ---- 1) 依次尝试所有 router 地址 ----
		Network::EndPoint* ep = NULL;
		for(size_t i = 0; i < endpoints_.size() && running_.load(); ++i)
		{
			ep = doConnect(endpoints_[i]);
			if(ep != NULL)
				break;

			sleepMS(CONNECT_RETRY_DELAY_MS);
		}

		if(ep == NULL)
		{
			sleepMS(RECONNECT_DELAY_MS);
			continue;
		}

		// ---- 2) HELLO 握手 ----
		if(!doHello(ep))
		{
			ep->close();
			Network::EndPoint::reclaimPoolObject(ep);
			sleepMS(RECONNECT_DELAY_MS);
			continue;
		}

		// ---- 3) 就绪：把 ep 交给主线程发送 ----
		{
			std::lock_guard<std::mutex> lock(sendMutex_);
			ep_ = ep;
			alive_ = true;
		}

		ready_ = true;

		{
			RouterEvent ev;
			ev.type = RouterEvent::EV_READY;
			pushEvent(ev);
		}

		// ---- 4) 收帧循环 ----
		std::string payload;
		while(running_.load())
		{
			if(!recvFrame(ep, payload))
				break;

			handleInboundFrame(payload);
		}

		// ---- 5) 断开清理 ----
		ready_ = false;

		{
			std::lock_guard<std::mutex> lock(sendMutex_);
			alive_ = false;
			if(ep_ == ep)
				ep_ = NULL;
			connId_ = 0;
		}

		ep->close();
		Network::EndPoint::reclaimPoolObject(ep);

		{
			RouterEvent ev;
			ev.type = RouterEvent::EV_DISCONNECTED;
			pushEvent(ev);
		}

		if(running_.load())
			sleepMS(RECONNECT_DELAY_MS);
	}
}

//-------------------------------------------------------------------------------------
Network::EndPoint* RouterClient::doConnect(const RouterAddr& addr)
{
	uint32 ip = ::inet_addr(addr.ip.c_str());
	if(ip == (uint32)INADDR_NONE)
	{
		WARNING_MSG(fmt::format("RouterClient::doConnect(): bad ip '{}'.\n", addr.ip));
		return NULL;
	}

	Network::EndPoint* ep = Network::EndPoint::createPoolObject(OBJECTPOOL_POINT);
	ep->socket(SOCK_STREAM);

	if(!ep->good())
	{
		Network::EndPoint::reclaimPoolObject(ep);
		return NULL;
	}

	// 端口需网络字节序；autosetflags=false 以保持阻塞语义(recvAll 需要阻塞)
	// connect 返回 -1 表示失败，与 cluster_core.cpp 的约定一致
	if(ep->connect(htons(addr.port), ip, false) == -1)
	{
		ep->close();
		Network::EndPoint::reclaimPoolObject(ep);
		return NULL;
	}

	return ep;
}

//-------------------------------------------------------------------------------------
bool RouterClient::doHello(Network::EndPoint* ep)
{
	std::string payload;
	Wire::putU8(payload, (uint8)PEER_COMPONENT);
	Wire::putU32(payload, (uint32)componentId_);		// uid
	Wire::putU32(payload, (uint32)componentType_);
	Wire::putU64(payload, (uint64)componentId_);
	Wire::putU16(payload, (uint16)ROUTER_PROTOCOL_VERSION);

	if(!sendFrameRaw(ep, encodeMessage(MSG_HELLO, payload)))
		return false;

	std::string resp;
	if(!recvFrame(ep, resp))
		return false;

	if(resp.empty() || (uint8)resp[0] != MSG_RESP_HELLO_OK)
	{
		WARNING_MSG(fmt::format("RouterClient::doHello(): rejected by router (resp type={}).\n",
			resp.empty() ? -1 : (int)(uint8)resp[0]));
		return false;
	}

	size_t off = 1;
	size_t n = resp.size();
	uint64 cid = 0;

	if(!Wire::getU64(resp.data(), n, off, cid))
		return false;

	{
		std::lock_guard<std::mutex> lock(sendMutex_);
		connId_ = cid;
	}

	return true;
}

//-------------------------------------------------------------------------------------
void RouterClient::pushEvent(const RouterEvent& ev)
{
	std::lock_guard<std::mutex> lock(queueMutex_);
	queue_.push_back(ev);
}

//-------------------------------------------------------------------------------------
void RouterClient::handleInboundFrame(const std::string& payload)
{
	if(payload.empty())
		return;

	uint8 type = (uint8)payload[0];
	std::string body = payload.substr(1);

	const char* d = body.data();
	size_t n = body.size();
	size_t off = 0;

	switch(type)
	{
	case MSG_SEND:
		{
			RouterEvent ev;
			ev.type = RouterEvent::EV_MAIL;

			if(!getMailbox(d, n, off, ev.dst) ||
				!getMailbox(d, n, off, ev.src) ||
				!Wire::getU64(d, n, off, ev.msgId))
			{
				ev.type = RouterEvent::EV_ERROR;
				ev.text = "bad MSG_SEND";
				pushEvent(ev);
				return;
			}

			ev.body.assign(d + off, n - off);

			pushEvent(ev);
		}
		break;

	case MSG_DELIVERY_FAILED:
		{
			RouterEvent ev;
			ev.type = RouterEvent::EV_DELIVERY_FAILED;

			if(!getMailbox(d, n, off, ev.src) ||
				!getMailbox(d, n, off, ev.dst) ||
				!Wire::getU64(d, n, off, ev.msgId) ||
				!Wire::getU32(d, n, off, ev.code))
			{
				ev.type = RouterEvent::EV_ERROR;
				ev.text = "bad MSG_DELIVERY_FAILED";
			}

			pushEvent(ev);
		}
		break;

	case MSG_RESP_SERVICE:
		{
			RouterEvent ev;
			ev.type = RouterEvent::EV_SERVICE_FOUND;

			uint16 count = 0;
			if(!Wire::getU16(d, n, off, count))
			{
				ev.type = RouterEvent::EV_ERROR;
				ev.text = "bad MSG_RESP_SERVICE";
				pushEvent(ev);
				return;
			}

			for(uint16 i = 0; i < count; ++i)
			{
				MailboxAddress a;
				if(!getMailbox(d, n, off, a))
				{
					ev.type = RouterEvent::EV_ERROR;
					ev.text = "bad MSG_RESP_SERVICE mailbox";
					break;
				}

				ev.addrs.push_back(a);
			}

			pushEvent(ev);
		}
		break;

	case MSG_RESP_NOT_FOUND:
		{
			RouterEvent ev;
			ev.type = RouterEvent::EV_SERVICE_NOT_FOUND;
			pushEvent(ev);
		}
		break;

	case MSG_RESP_ERROR:
		{
			RouterEvent ev;
			ev.type = RouterEvent::EV_ERROR;

			std::string msg;
			if(Wire::getString(d, n, off, msg))
				ev.text = msg;
			else
				ev.text = "router error";

			pushEvent(ev);
		}
		break;

	case MSG_RESP_OK:
	case MSG_RESP_PONG:
	case MSG_RESP_HELLO_OK:
		// 无需处理(HELLO_OK 已在握手中消费；OK/PONG 用于 keepalive 观测)
		break;

	default:
		{
			RouterEvent ev;
			ev.type = RouterEvent::EV_ERROR;
			ev.text = fmt::format("unknown router message type {}", (int)type);
			pushEvent(ev);
		}
		break;
	}
}

//-------------------------------------------------------------------------------------
// 主线程
//-------------------------------------------------------------------------------------

void RouterClient::tick()
{
	drainEvents();
}

//-------------------------------------------------------------------------------------
void RouterClient::drainEvents()
{
	std::deque< RouterEvent > local;
	{
		std::lock_guard<std::mutex> lock(queueMutex_);
		local.swap(queue_);
	}

	while(!local.empty())
	{
		RouterEvent ev = local.front();
		local.pop_front();

		switch(ev.type)
		{
		case RouterEvent::EV_READY:
			INFO_MSG(fmt::format("RouterClient: connected to router, connId={}, component={}({}).\n",
				(uint64)connId(), COMPONENT_NAME_EX(componentType_), (uint64)componentId_));

			if(onReadyChanged)
				onReadyChanged(true);
			break;

		case RouterEvent::EV_DISCONNECTED:
			WARNING_MSG(fmt::format("RouterClient: disconnected from router, component={}({}).\n",
				COMPONENT_NAME_EX(componentType_), (uint64)componentId_));

			if(onReadyChanged)
				onReadyChanged(false);
			break;

		case RouterEvent::EV_MAIL:
			if(onMail)
			{
				Mail m;
				m.src = ev.src;
				m.dst = ev.dst;
				m.msgId = ev.msgId;
				m.body.swap(ev.body);
				onMail(m);
			}
			break;

		case RouterEvent::EV_DELIVERY_FAILED:
			if(onDeliveryFailed)
			{
				DeliveryFailure f;
				f.src = ev.src;
				f.dst = ev.dst;
				f.msgId = ev.msgId;
				f.code = ev.code;
				onDeliveryFailed(f);
			}
			break;

		case RouterEvent::EV_SERVICE_FOUND:
			{
				uint64_t reqId = 0;
				if(!pendingFindService_.empty())
				{
					reqId = pendingFindService_.front();
					pendingFindService_.pop_front();
				}

				if(onServiceFound)
					onServiceFound(reqId, ev.addrs);
			}
			break;

		case RouterEvent::EV_SERVICE_NOT_FOUND:
			{
				uint64_t reqId = 0;
				if(!pendingFindService_.empty())
				{
					reqId = pendingFindService_.front();
					pendingFindService_.pop_front();
				}

				if(onServiceNotFound)
					onServiceNotFound(reqId);
			}
			break;

		case RouterEvent::EV_ERROR:
			WARNING_MSG(fmt::format("RouterClient: router error: {}.\n", ev.text));

			if(onError)
				onError(ev.text);
			break;

		default:
			break;
		}
	}
}

//-------------------------------------------------------------------------------------
bool RouterClient::sendFrame(const std::string& payload)
{
	std::lock_guard<std::mutex> lock(sendMutex_);

	if(!alive_ || ep_ == NULL)
		return false;

	if(!sendFrameRaw(ep_, payload))
	{
		// 让后续发送快速失败，真正的断开由网络线程感知并触发重连
		alive_ = false;
		return false;
	}

	return true;
}

//-------------------------------------------------------------------------------------
// 发送接口
//-------------------------------------------------------------------------------------

bool RouterClient::sendMail(const RouterInterface::MailboxAddress& dst,
	const RouterInterface::MailboxAddress& src,
	uint64_t msgId,
	const std::string& body)
{
	std::string payload;
	putMailbox(payload, dst);		// router 只解析这一段
	putMailbox(payload, src);
	Wire::putU64(payload, msgId);
	payload.append(body);

	return sendFrame(encodeMessage(MSG_SEND, payload));
}

//-------------------------------------------------------------------------------------
bool RouterClient::registerActor(const RouterInterface::MailboxAddress& addr, uint64_t targetConnId)
{
	std::string payload;
	putMailbox(payload, addr);
	Wire::putU64(payload, targetConnId);

	return sendFrame(encodeMessage(MSG_REGISTER_ACTOR, payload));
}

//-------------------------------------------------------------------------------------
bool RouterClient::unregisterActor(const RouterInterface::MailboxAddress& addr)
{
	std::string payload;
	putMailbox(payload, addr);

	return sendFrame(encodeMessage(MSG_UNREGISTER_ACTOR, payload));
}

//-------------------------------------------------------------------------------------
bool RouterClient::suspendActor(const RouterInterface::MailboxAddress& addr)
{
	std::string payload;
	putMailbox(payload, addr);

	return sendFrame(encodeMessage(MSG_SUSPEND_ACTOR, payload));
}

//-------------------------------------------------------------------------------------
bool RouterClient::resumeActor(const RouterInterface::MailboxAddress& addr)
{
	std::string payload;
	putMailbox(payload, addr);

	return sendFrame(encodeMessage(MSG_RESUME_ACTOR, payload));
}

//-------------------------------------------------------------------------------------
bool RouterClient::registerService(const std::string& name, const RouterInterface::MailboxAddress& addr)
{
	// 先让该地址可被 MSG_SEND 路由(router 的 actors_ 表)，
	// 再登记服务名(router 的 services_ 表，供 findService 使用)。
	bool ok = registerActor(addr);

	std::string payload;
	Wire::putString(payload, name);
	putMailbox(payload, addr);

	if(!sendFrame(encodeMessage(MSG_REGISTER_SERVICE, payload)))
		return false;

	return ok;
}

//-------------------------------------------------------------------------------------
bool RouterClient::findService(const std::string& name, uint64_t requestId)
{
	std::string payload;
	Wire::putString(payload, name);

	if(!sendFrame(encodeMessage(MSG_FIND_SERVICE, payload)))
		return false;

	pendingFindService_.push_back(requestId);
	return true;
}

//-------------------------------------------------------------------------------------
}
