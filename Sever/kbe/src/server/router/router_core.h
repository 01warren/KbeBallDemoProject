// Copyright 2008-2018 Yolo Technologies, Inc. All Rights Reserved. https://www.comblockengine.com

/*
	RouterCore：接入 + 路由中心。

	职责(详见 docs/kbe_router_design.md)：
	  - 监听 clientPort(客户端) 与 serverPort(服务器组件) 两个 TCP 端口；
	  - 每连接一个读线程：阻塞收帧 -> 投递事件到主线程队列；
	  - 主线程 tick()：处理事件 -> 解析 -> 查路由表 -> 原样转发 MSG_SEND；
	  - 仅在无法投递时回 MSG_DELIVERY_FAILED(路由无状态，不做可靠性)。

	线程模型(与 cluster 一致)：
	  网络线程只收/发，路由表与 Service 表只被主线程访问；
	  Connection 的 ep/alive 由自身 sendMutex 保护(读线程退出时在锁内置 alive=false 并关闭)。
*/

#ifndef KBE_ROUTER_CORE_H
#define KBE_ROUTER_CORE_H

#include "actor_registry.h"
#include "router_interface.h"
#include "network/endpoint.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace KBEngine {

class RouterCore
{
public:
	RouterCore(const std::string& selfIP, uint16 clientPort, uint16 serverPort,
		uint32 suspendTimeoutMS, uint32 actorBufferMax,
		uint32 pendingTimeoutMS, uint32 pendingBufferMax);

	~RouterCore();

	/** 绑定两个监听端口并启动网络线程 */
	bool start();

	void stop();

	/** 主循环(ServerApp 帧)推进：处理事件队列 + 挂起超时回滚 + 待定超时清理 */
	void tick();

private:
	// 一条 peer 连接(客户端或服务器组件)
	struct Connection
	{
		uint64_t			connId;
		uint8_t				peerKind;		// RouterInterface::PeerKind
		int32_t				uid;
		int32_t				componentType;
		uint64_t			componentId;

		Network::EndPoint*	ep;				// 由 sendMutex 保护
		bool				alive;			// 由 sendMutex 保护
		std::mutex			sendMutex;

		std::string			peerName;		// 观测用

		Connection()
			: connId(0), peerKind(RouterInterface::PEER_CLIENT), uid(0),
			  componentType(0), componentId(0), ep(NULL), alive(true)
		{
		}
	};

	typedef std::shared_ptr<Connection> ConnectionPtr;

	// 网络线程投递到主线程的事件
	enum EventType : uint8_t
	{
		EVENT_CONN_OPENED	= 1,	// data: 空
		EVENT_INBOUND		= 2,	// data: [u8 消息类型][载荷]
		EVENT_CONN_CLOSED	= 3		// data: 空
	};

	struct Event
	{
		uint8_t			type;
		uint64_t		connId;
		ConnectionPtr	conn;		// 仅 EVENT_CONN_OPENED 使用
		std::string		data;
	};

	// ---- 网络线程 ----
	bool spawn(const std::function<void()>& fn);
	void acceptorLoop(Network::EndPoint* listenEP, const char* tag, uint8_t defaultPeerKind);
	void connThread(ConnectionPtr conn);

	// ---- 主循环 ----
	void drainEvents();
	void handleConnOpened(const ConnectionPtr& conn);
	void handleConnClosed(uint64_t connId);
	void handleInbound(ConnectionPtr conn, const std::string& data);
	void checkSuspendTimeouts();
	void checkPendingTimeouts();

	// ---- 消息处理(主线程) ----
	void onHello(ConnectionPtr conn, const std::string& payload);
	void onRegisterActor(ConnectionPtr conn, const std::string& payload);
	void onUnregisterActor(ConnectionPtr conn, const std::string& payload);
	void onSuspendActor(ConnectionPtr conn, const std::string& payload);
	void onResumeActor(ConnectionPtr conn, const std::string& payload);
	void onRegisterService(ConnectionPtr conn, const std::string& payload);
	void onFindService(ConnectionPtr conn, const std::string& payload);

	/**
		MSG_SEND：只解析 dst，然后把整个载荷原样转发给目标连接。
		失败时回 MSG_DELIVERY_FAILED(见设计文档 §8.3)。
	*/
	void onSend(ConnectionPtr conn, const std::string& payload);

	// ---- 收发 ----
	ConnectionPtr findConn(uint64_t connId);
	bool sendFrame(const ConnectionPtr& conn, const std::string& payload);
	bool deliverTo(uint64_t connId, const std::string& payload);

	void replyOk(const ConnectionPtr& conn);
	void replyError(const ConnectionPtr& conn, const std::string& msg);
	void replyPong(const ConnectionPtr& conn);
	void replyNotFound(const ConnectionPtr& conn);

	void reportDeliveryFailed(const ConnectionPtr& conn,
		const MailboxAddress& src, const MailboxAddress& dst,
		uint64_t msgId, uint32_t code);

	/**
		把投递失败回报给**指定连接**。
		用于延迟失败场景(待定缓冲溢出/超时)：那时当前处理的连接与原始发送方不是同一个，
		原始发送方连接可能已断开，此时静默丢弃(可靠性由业务侧超时兜底，见设计文档 §8)。
	*/
	void reportDeliveryFailedTo(uint64_t connId,
		const MailboxAddress& src, const MailboxAddress& dst,
		uint64_t msgId, uint32_t code);

	/** 实际编码并发送 MSG_DELIVERY_FAILED(上面两个入口的公共实现) */
	void sendDeliveryFailed(const ConnectionPtr& conn,
		const MailboxAddress& src, const MailboxAddress& dst,
		uint64_t msgId, uint32_t code);

	// 冲刷挂起缓冲(投递到指定连接)
	void flushMails(const FlushResult& fr);

private:
	// ---- 配置 ----
	std::string		selfIP_;
	uint16			clientPort_;
	uint16			serverPort_;
	uint32			suspendTimeoutMS_;
	uint32			actorBufferMax_;
	// ---- 待定(目标尚未注册)缓冲配置 ----
	// pendingBufferMax_ == 0 表示关闭该能力：未注册目标直接回 TARGET_NOT_FOUND(改造前行为)
	uint32			pendingTimeoutMS_;
	uint32			pendingBufferMax_;

	// ---- 生命周期 ----
	std::vector<std::thread>	threads_;
	std::mutex					threadsMutex_;
	std::atomic<bool>			running_;

	Network::EndPoint*			clientListenEP_;
	Network::EndPoint*			serverListenEP_;

	// ---- 连接表(仅主线程读写) ----
	std::map<uint64_t, ConnectionPtr>	conns_;
	std::atomic<uint64_t>				nextConnId_;

	// ---- 事件队列(网络线程写，主线程读) ----
	std::mutex			queueMutex_;
	std::vector<Event>	queue_;

	// ---- 路由表(仅主线程访问) ----
	ActorRegistry		registry_;

	// ---- 统计 ----
	uint64_t	counterSent_;
	uint64_t	counterForwarded_;
	uint64_t	counterFailed_;
	uint64_t	counterPending_;		// 进入待定缓冲的 Mail 数
	uint64_t	counterFlushed_;		// 待定缓冲冲刷出去的 Mail 数
};

}

#endif // KBE_ROUTER_CORE_H
