// Copyright 2008-2018 Yolo Technologies, Inc. All Rights Reserved. https://www.comblockengine.com

/*
	ActorRegistry：router 的路由表 + 服务名字表 + 迁移挂起缓冲。

	全部为**纯内存软状态**，router 重启后由 peer 重连重新注册填充(见设计文档 §6 / §8)。
	本类只被 router 主线程访问，因此内部不加锁。

	| actors_    | ActorKey{kind, componentType, actorId} -> ActorEntry | Actor 在哪条连接上 |
	| services_  | serviceName -> vector<MailboxAddress>             | 服务名 -> 地址列表 |
	| suspended_ | ActorKey -> SuspendedActor{oldConnId, buffered}   | 迁移期缓冲        |
	| pending_   | ActorKey -> PendingActor{deadlineMS, buffered}    | 目标未注册期缓冲  |

suspended_ 与 pending_ 的区别(设计文档 §6.3 / §14.4)：
  - suspended_：Actor **已注册**、只是正在迁移，缓冲后由 RESUME 冲刷回新宿主，超时回滚到原宿主；
  - pending_  ：Actor **尚未注册**(组件未连接 / 实体 cell 部分还没创建)，
                缓冲后由"首次注册"冲刷；超时或溢出则回失败(不会永远占着内存)。
*/

#ifndef KBE_ROUTER_ACTOR_REGISTRY_H
#define KBE_ROUTER_ACTOR_REGISTRY_H

#include "router_interface.h"

#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <vector>

namespace KBEngine {

typedef RouterInterface::MailboxAddress MailboxAddress;

/*
	Actor 的唯一键：kind + componentType + actorId。

	为什么必须带上 componentType：
	同一个 ENTITY_ID 在引擎里会同时存在"base 部分"(宿主 baseapp)与"cell 部分"(宿主 cellapp)，
	二者是两个独立进程上、各自可被寻址的 Actor。若只用 {kind, actorId} 做键，
	后注册的一方会覆盖先注册的一方，导致 entity.base / entity.cell 中必有一个路由错误。

	由于 base/cell 的 ENTITYCALL_TYPE 会映射到不同的 COMPONENT_TYPE
	(entityCallType2ComponentType: BASE->BASEAPP_TYPE / CELL->CELLAPP_TYPE)，
	componentType 天然就是二者的判别位。它在线格式里本就随 mailbox 一起传输
	(见 MAILBOX_WIRE_SIZE)，因此这里是**纯 router 内部改动，协议保持兼容**。
*/
struct ActorKey
{
	uint8_t  kind;
	uint32_t componentType;
	uint64_t actorId;

	ActorKey() : kind(0), componentType(0), actorId(0) {}
	ActorKey(uint8_t k, uint32_t ct, uint64_t id) : kind(k), componentType(ct), actorId(id) {}

	bool operator<(const ActorKey& o) const
	{
		if(kind != o.kind) return kind < o.kind;
		if(componentType != o.componentType) return componentType < o.componentType;
		return actorId < o.actorId;
	}
};

inline ActorKey makeActorKey(const MailboxAddress& a)
{
	return ActorKey(a.kind, a.componentType, a.actorId);
}

// 路由表条目
struct ActorEntry
{
	uint64_t connId;			// 当前宿主的连接
	uint32_t componentType;		// 宿主组件类型(观测用)
	uint64_t lastActiveMS;		// 最近一次注册/投递时间(观测用)

	ActorEntry() : connId(0), componentType(0), lastActiveMS(0) {}
};

// 迁移挂起条目
struct SuspendedActor
{
	uint64_t oldConnId;					// 挂起时的宿主连接(超时回滚到此连接)
	uint64_t deadlineMS;				// 挂起超时时间点
	std::deque<std::string> buffered;	// 等待投递的 MSG_SEND 载荷(保持到达顺序)
};

// 冲刷结果：把 mails 按顺序投递到 connId
struct FlushResult
{
	MailboxAddress addr;
	uint64_t connId;
	std::vector<std::string> mails;
};

// 待定缓冲中的一条 Mail
struct PendingMail
{
	std::string		payload;		// 完整 MSG_SEND 载荷(dst + src + msgId + body)
	MailboxAddress	src;			// 原始发送方(回报失败时用)
	uint64_t		msgId;			// 原始 msgId(回报失败时用)
	uint64_t		srcConnId;		// 原始发送方的连接(回报失败时用；发送方已断开则送不到)

	PendingMail() : msgId(0), srcConnId(0) {}
};

// 目标尚未注册期间的缓冲条目
struct PendingActor
{
	uint64_t				deadlineMS;		// 待定超时时间点(到期未注册则丢弃并回失败)
	std::deque<PendingMail>	buffered;		// 等待投递的 Mail(保持到达顺序)

	PendingActor() : deadlineMS(0) {}
};

// 待定超时需要回报的失败项(逐条回报，因为每条 Mail 的发送方可能不同)
struct ExpiredPendingMail
{
	MailboxAddress	src;
	MailboxAddress	dst;
	uint64_t		msgId;
	uint64_t		srcConnId;

	ExpiredPendingMail() : msgId(0), srcConnId(0) {}
};

class ActorRegistry
{
public:
	ActorRegistry();
	~ActorRegistry();

	void clear();

	// ---- Actor 路由 ----
	/** 绑定/更新 Actor 的宿主连接 */
	bool bindActor(const MailboxAddress& addr, uint64_t connId,
		uint32_t componentType, uint64_t nowMS);

	/** 注销 Actor */
	bool unbindActor(const MailboxAddress& addr);

	/** 查找 Actor，未注册返回 NULL */
	const ActorEntry* findActor(const MailboxAddress& addr) const;

	/** 连接断开时的兜底清理：删除该连接名下所有 Actor 与 Service 注册 */
	void removeConn(uint64_t connId,
		std::vector<ActorKey>& removedActors,
		std::vector<std::string>& removedServices);

	// ---- Service 名字表 ----
	bool bindService(const std::string& name, const MailboxAddress& addr);
	bool unbindService(const std::string& name, const MailboxAddress& addr);
	std::vector<MailboxAddress> findService(const std::string& name) const;

	// ---- 迁移挂起 ----
	/** 标记 Actor 进入迁移挂起(此后投往它的 Mail 进入缓冲) */
	bool suspendActor(const MailboxAddress& addr, uint64_t nowMS, uint64_t timeoutMS);

	bool isSuspended(const MailboxAddress& addr) const;

	/**
		把一条 Mail 放入挂起缓冲。
		@param dropped 缓冲溢出时置 true，并丢弃最旧的一条(调用方据此回报 DELIVERY_DROPPED_SUSPENDED)
		@return 是否成功入队
	*/
	bool bufferMail(const MailboxAddress& addr, const std::string& payload,
		size_t maxBuffered, bool& dropped);

	/** 迁移完成：路由改指 newConnId，并取出缓冲的 Mail(按到达顺序) */
	bool resumeActor(const MailboxAddress& addr, uint64_t newConnId,
		uint64_t nowMS, FlushResult& out);

	/**
		挂起超时回滚：把已超时的挂起条目取出，冲刷回其**原宿主连接**，避免永久挂起。
		(设计文档 §6.3：RESUME 未在超时内到达 -> 自动回滚)
	*/
	void takeExpiredSuspends(uint64_t nowMS, std::vector<FlushResult>& out);

	/** 丢弃某个 Actor 的挂起缓冲(例如其宿主已彻底消失) */
	void dropSuspended(const MailboxAddress& addr);

	// ---- 待定(目标尚未注册)缓冲 ----
	/**
		目标 Actor 尚未注册时把一条 Mail 放入待定缓冲(设计文档 §6.2 / §14.4)。

		语义：这是"目标尚未可达 -> 缓冲 -> 就绪后重投"能力的落点，用来顶替
		旧通路 ForwardComponent_MessageBuffer / entity_messages_forward_handler 的
		"组件未连接 / 实体 cell 部分尚未创建" 时的入队等待。

		@param droppedMail 溢出时带出被丢弃的**最旧**一条(调用方据此向它的发送方回报失败)
		@param dropped     是否发生了溢出丢弃
		@return 是否成功入队(新的一条总是会入队)
	*/
	bool bufferPendingMail(const MailboxAddress& addr, const std::string& payload,
		const MailboxAddress& src, uint64_t msgId, uint64_t srcConnId,
		uint64_t nowMS, uint64_t timeoutMS, size_t maxBuffered,
		PendingMail& droppedMail, bool& dropped);

	/** 该 Actor 是否有待定缓冲(注册前调用，命中则走 takePending 冲刷) */
	bool hasPending(const MailboxAddress& addr) const;

	/** 目标注册即"就绪"：取出待定缓冲，按到达顺序投递到 newConnId */
	void takePending(const MailboxAddress& addr, uint64_t newConnId, FlushResult& out);

	/** 待定超时：取出所有到期条目(每条的发送方可能不同，故逐条回报失败) */
	void takeExpiredPendings(uint64_t nowMS, std::vector<ExpiredPendingMail>& out);

	/** 丢弃某个 Actor 的待定缓冲(例如其宿主已彻底消失) */
	void dropPending(const MailboxAddress& addr);

	// ---- 统计 ----
	size_t actorCount() const { return actors_.size(); }
	size_t serviceCount() const { return services_.size(); }
	size_t suspendedCount() const { return suspended_.size(); }
	size_t suspendedMailCount() const;
	size_t pendingCount() const { return pending_.size(); }
	size_t pendingMailCount() const;

private:
	std::map<ActorKey, ActorEntry>						actors_;
	std::map<std::string, std::vector<MailboxAddress> >	services_;
	std::map<ActorKey, SuspendedActor>					suspended_;
	std::map<ActorKey, PendingActor>					pending_;
};

}

#endif // KBE_ROUTER_ACTOR_REGISTRY_H
