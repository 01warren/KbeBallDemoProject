// Copyright 2008-2018 Yolo Technologies, Inc. All Rights Reserved. https://www.comblockengine.com

#include "actor_registry.h"

#include <algorithm>

namespace KBEngine {

ActorRegistry::ActorRegistry()
{
}

ActorRegistry::~ActorRegistry()
{
}

void ActorRegistry::clear()
{
	actors_.clear();
	services_.clear();
	suspended_.clear();
	pending_.clear();
}

//-------------------------------------------------------------------------------------
// Actor 路由
//-------------------------------------------------------------------------------------
bool ActorRegistry::bindActor(const MailboxAddress& addr, uint64_t connId,
	uint32_t componentType, uint64_t nowMS)
{
	ActorKey key = makeActorKey(addr);

	// 换宿主绑定等价于"重新注册"：清掉可能残留的挂起/待定状态。
	// 注意：调用方若希望保留缓冲消息，应先 isSuspended()/hasPending() 判断并走
	// resumeActor()/takePending() 冲刷，再调用本函数。
	suspended_.erase(key);
	pending_.erase(key);

	ActorEntry& e = actors_[key];
	e.connId = connId;
	e.componentType = componentType;
	e.lastActiveMS = nowMS;
	return true;
}

bool ActorRegistry::unbindActor(const MailboxAddress& addr)
{
	ActorKey key = makeActorKey(addr);

	suspended_.erase(key);
	pending_.erase(key);

	std::map<ActorKey, ActorEntry>::iterator it = actors_.find(key);
	if(it == actors_.end())
		return false;

	actors_.erase(it);
	return true;
}

const ActorEntry* ActorRegistry::findActor(const MailboxAddress& addr) const
{
	std::map<ActorKey, ActorEntry>::const_iterator it = actors_.find(makeActorKey(addr));
	if(it == actors_.end())
		return NULL;

	return &it->second;
}

void ActorRegistry::removeConn(uint64_t connId,
	std::vector<ActorKey>& removedActors,
	std::vector<std::string>& removedServices)
{
	removedActors.clear();
	removedServices.clear();

	// 1) 删除该连接名下所有 Actor
	{
		std::map<ActorKey, ActorEntry>::iterator it = actors_.begin();
		while(it != actors_.end())
		{
			if(it->second.connId == connId)
			{
				removedActors.push_back(it->first);
				actors_.erase(it++);
			}
			else
			{
				++it;
			}
		}
	}

	// 2) 删除该连接名下所有 Service(按 MailboxAddress 归属判断)
	{
		std::map<std::string, std::vector<MailboxAddress> >::iterator it = services_.begin();
		while(it != services_.end())
		{
			std::vector<MailboxAddress>& list = it->second;

			for(size_t i = list.size(); i > 0; --i)
			{
				const ActorEntry* e = findActor(list[i - 1]);
				if(e == NULL || e->connId == connId)
				{
					removedServices.push_back(it->first);
					list.erase(list.begin() + (i - 1));
				}
			}

			if(list.empty())
				services_.erase(it++);
			else
				++it;
		}
	}

	// 3) 挂起缓冲刻意保留：实体正在迁移到新宿主，RESUME 到达后仍需冲刷；
	//    若宿主的迁移永远不完成，则由 takeExpiredSuspends() 超时清理(设计文档 §6.3)。
	//
	//    待定缓冲同理保留：其键是**目标**地址，与断开连接的归属无关
	//    (目标本来就没注册，因此无从判断它是否属于该连接)，
	//    统一交由 takeExpiredPendings() 超时清理，避免误伤"目标正在启动"的缓冲。
}

//-------------------------------------------------------------------------------------
// Service 名字表
//-------------------------------------------------------------------------------------
bool ActorRegistry::bindService(const std::string& name, const MailboxAddress& addr)
{
	if(name.empty())
		return false;

	std::vector<MailboxAddress>& list = services_[name];

	for(size_t i = 0; i < list.size(); ++i)
	{
		if(list[i] == addr)
			return false;	// 已注册
	}

	list.push_back(addr);
	return true;
}

bool ActorRegistry::unbindService(const std::string& name, const MailboxAddress& addr)
{
	std::map<std::string, std::vector<MailboxAddress> >::iterator it = services_.find(name);
	if(it == services_.end())
		return false;

	std::vector<MailboxAddress>& list = it->second;
	for(size_t i = 0; i < list.size(); ++i)
	{
		if(list[i] == addr)
		{
			list.erase(list.begin() + i);

			if(list.empty())
				services_.erase(it);

			return true;
		}
	}

	return false;
}

std::vector<MailboxAddress> ActorRegistry::findService(const std::string& name) const
{
	std::map<std::string, std::vector<MailboxAddress> >::const_iterator it = services_.find(name);
	if(it == services_.end())
		return std::vector<MailboxAddress>();

	return it->second;
}

//-------------------------------------------------------------------------------------
// 迁移挂起
//-------------------------------------------------------------------------------------
bool ActorRegistry::suspendActor(const MailboxAddress& addr, uint64_t nowMS, uint64_t timeoutMS)
{
	ActorKey key = makeActorKey(addr);

	std::map<ActorKey, ActorEntry>::const_iterator ae = actors_.find(key);
	if(ae == actors_.end())
		return false;	// 未注册的 Actor 无法挂起

	SuspendedActor& sa = suspended_[key];

	if(sa.oldConnId == 0)
	{
		sa.oldConnId = ae->second.connId;
		sa.deadlineMS = nowMS + timeoutMS;
	}
	else
	{
		// 重复 SUSPEND：刷新超时即可，不丢缓冲
		sa.deadlineMS = nowMS + timeoutMS;
	}

	return true;
}

bool ActorRegistry::isSuspended(const MailboxAddress& addr) const
{
	return suspended_.find(makeActorKey(addr)) != suspended_.end();
}

bool ActorRegistry::bufferMail(const MailboxAddress& addr, const std::string& payload,
	size_t maxBuffered, bool& dropped)
{
	dropped = false;

	ActorKey key = makeActorKey(addr);
	std::map<ActorKey, SuspendedActor>::iterator it = suspended_.find(key);
	if(it == suspended_.end())
		return false;

	std::deque<std::string>& q = it->second.buffered;

	// maxBuffered == 0 视为不限制
	if(maxBuffered > 0 && q.size() >= maxBuffered)
	{
		q.pop_front();		// 丢弃最旧的一条(设计文档 §6.3)
		dropped = true;
	}

	q.push_back(payload);
	return true;
}

bool ActorRegistry::resumeActor(const MailboxAddress& addr, uint64_t newConnId,
	uint64_t nowMS, FlushResult& out)
{
	ActorKey key = makeActorKey(addr);

	out.addr = addr;
	out.connId = newConnId;
	out.mails.clear();

	// 路由改指新宿主
	ActorEntry& e = actors_[key];
	e.connId = newConnId;
	e.componentType = addr.componentType;
	e.lastActiveMS = nowMS;

	std::map<ActorKey, SuspendedActor>::iterator it = suspended_.find(key);
	if(it != suspended_.end())
	{
		out.mails.assign(it->second.buffered.begin(), it->second.buffered.end());
		suspended_.erase(it);
	}

	return true;
}

void ActorRegistry::takeExpiredSuspends(uint64_t nowMS, std::vector<FlushResult>& out)
{
	std::map<ActorKey, SuspendedActor>::iterator it = suspended_.begin();
	while(it != suspended_.end())
	{
		if(it->second.deadlineMS > nowMS)
		{
			++it;
			continue;
		}

		std::map<ActorKey, ActorEntry>::const_iterator ae = actors_.find(it->first);
		if(ae != actors_.end())
		{
			// 回滚：冲刷回当前登记(即原宿主)的连接
			FlushResult fr;
			fr.addr = MailboxAddress((uint8_t)it->first.kind, it->first.componentType, it->first.actorId);
			fr.connId = ae->second.connId;
			fr.mails.assign(it->second.buffered.begin(), it->second.buffered.end());
			out.push_back(fr);
		}
		// else：宿主已随连接消失，缓冲只能丢弃(符合设计文档 §8 的取舍)

		suspended_.erase(it++);
	}
}

void ActorRegistry::dropSuspended(const MailboxAddress& addr)
{
	suspended_.erase(makeActorKey(addr));
}

size_t ActorRegistry::suspendedMailCount() const
{
	size_t n = 0;

	std::map<ActorKey, SuspendedActor>::const_iterator it = suspended_.begin();
	for(; it != suspended_.end(); ++it)
		n += it->second.buffered.size();

	return n;
}

//-------------------------------------------------------------------------------------
// 待定(目标尚未注册)缓冲
//-------------------------------------------------------------------------------------
bool ActorRegistry::bufferPendingMail(const MailboxAddress& addr, const std::string& payload,
	const MailboxAddress& src, uint64_t msgId, uint64_t srcConnId,
	uint64_t nowMS, uint64_t timeoutMS, size_t maxBuffered,
	PendingMail& droppedMail, bool& dropped)
{
	dropped = false;

	ActorKey key = makeActorKey(addr);
	PendingActor& pa = pending_[key];

	// 首次入队时定下超时点；后续入队沿用(保证"从目标首次不可达算起"的 TTL 语义)
	if(pa.deadlineMS == 0)
		pa.deadlineMS = nowMS + timeoutMS;

	// maxBuffered == 0 视为不限制
	if(maxBuffered > 0 && pa.buffered.size() >= maxBuffered)
	{
		droppedMail = pa.buffered.front();
		pa.buffered.pop_front();
		dropped = true;
	}

	PendingMail pm;
	pm.payload = payload;
	pm.src = src;
	pm.msgId = msgId;
	pm.srcConnId = srcConnId;
	pa.buffered.push_back(pm);

	return true;
}

bool ActorRegistry::hasPending(const MailboxAddress& addr) const
{
	return pending_.find(makeActorKey(addr)) != pending_.end();
}

void ActorRegistry::takePending(const MailboxAddress& addr, uint64_t newConnId, FlushResult& out)
{
	out.addr = addr;
	out.connId = newConnId;
	out.mails.clear();

	std::map<ActorKey, PendingActor>::iterator it = pending_.find(makeActorKey(addr));
	if(it == pending_.end())
		return;

	// 按到达顺序冲刷(单连接 TCP + 单线程 router 保证 per-(src,dst) FIFO)
	for(size_t i = 0; i < it->second.buffered.size(); ++i)
		out.mails.push_back(it->second.buffered[i].payload);

	pending_.erase(it);
}

void ActorRegistry::takeExpiredPendings(uint64_t nowMS, std::vector<ExpiredPendingMail>& out)
{
	std::map<ActorKey, PendingActor>::iterator it = pending_.begin();
	while(it != pending_.end())
	{
		if(it->second.deadlineMS > nowMS)
		{
			++it;
			continue;
		}

		MailboxAddress dst((uint8_t)it->first.kind, it->first.componentType, it->first.actorId);

		for(size_t i = 0; i < it->second.buffered.size(); ++i)
		{
			ExpiredPendingMail em;
			em.src = it->second.buffered[i].src;
			em.dst = dst;
			em.msgId = it->second.buffered[i].msgId;
			em.srcConnId = it->second.buffered[i].srcConnId;
			out.push_back(em);
		}

		pending_.erase(it++);
	}
}

void ActorRegistry::dropPending(const MailboxAddress& addr)
{
	pending_.erase(makeActorKey(addr));
}

size_t ActorRegistry::pendingMailCount() const
{
	size_t n = 0;

	std::map<ActorKey, PendingActor>::const_iterator it = pending_.begin();
	for(; it != pending_.end(); ++it)
		n += it->second.buffered.size();

	return n;
}

}
