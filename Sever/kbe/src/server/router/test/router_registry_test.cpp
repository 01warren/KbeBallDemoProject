// Copyright 2008-2018 Yolo Technologies, Inc. All Rights Reserved. https://www.comblockengine.com

/*
	router_registry_test.cpp —— ActorRegistry 单元测试(零 KBE 依赖)。

	被测对象：kbe/src/server/router/actor_registry.{h,cpp}
	覆盖能力(设计文档 §6.1 / §6.2 / §6.3 / §6.4 / §14.4)：
	  - Actor 路由表的绑定/查找/注销，以及 ActorKey 必须带 componentType
	    (同一 ENTITY_ID 的 base 部分与 cell 部分不能互相覆盖)
	  - 迁移挂起 SUSPEND / RESUME 的缓冲、按序冲刷、溢出丢弃最旧、超时回滚
	  - **目标尚未注册**的待定缓冲 PENDING：入队 / 注册即冲刷 / 溢出 / TTL 超时逐条回报
	  - 连接断开的兜底清理语义

	构建(Windows, VS 开发者命令行)：
		cl /nologo /EHsc /W3 /I"<repo>\kbe\src" router_registry_test.cpp ^
		   ..\actor_registry.cpp /Fe:router_registry_test.exe
	构建(Linux)：
		g++ -std=c++11 -O2 -Wall -I<repo>/kbe/src router_registry_test.cpp \
			../actor_registry.cpp -o router_registry_test
	退出码：0 = 全部通过；1 = 有用例失败。
*/

#include "server/router/actor_registry.h"
#include "test_common.h"

#include <string>
#include <vector>

TestRunner g_runner;

using namespace KBEngine;

// 与线上取值保持一致：见 lib/common/common.h 的 COMPONENT_TYPE
static const uint32_t CT_BASEAPP = 2;
static const uint32_t CT_CELLAPP = 3;
static const uint32_t CT_CLIENT	 = 6;

static MailboxAddress ent(uint64_t id, uint32_t ct)
{
	return RouterInterface::mailboxEntity(id, ct);
}

static MailboxAddress comp(uint64_t cid, uint32_t ct)
{
	return RouterInterface::mailboxComponent(cid, ct);
}

//-------------------------------------------------------------------------------------
// R01 基本绑定 / 查找 / 注销
//-------------------------------------------------------------------------------------
static void case_R01_bind_find_unbind()
{
	ActorRegistry reg;

	MailboxAddress a = ent(1001, CT_CELLAPP);

	CHECK(reg.findActor(a) == NULL);
	CHECK(reg.actorCount() == 0);

	reg.bindActor(a, 77, CT_CELLAPP, 1000);

	const ActorEntry* e = reg.findActor(a);
	CHECK(e != NULL);
	CHECK_EQ_INT(e->connId, 77);
	CHECK_EQ_INT(reg.actorCount(), 1);

	// 换宿主：同一 Actor 重新绑定后指向新连接
	reg.bindActor(a, 88, CT_CELLAPP, 2000);
	CHECK_EQ_INT(reg.findActor(a)->connId, 88);

	CHECK(reg.unbindActor(a));
	CHECK(reg.findActor(a) == NULL);
	CHECK_EQ_INT(reg.actorCount(), 0);

	// 重复注销返回 false
	CHECK(!reg.unbindActor(a));
}

//-------------------------------------------------------------------------------------
// R02 ActorKey 必须带 componentType：base 部分与 cell 部分互不覆盖
//-------------------------------------------------------------------------------------
static void case_R02_actorkey_componenttype_isolation()
{
	ActorRegistry reg;

	const uint64_t eid = 2002;
	MailboxAddress basePart = ent(eid, CT_BASEAPP);
	MailboxAddress cellPart = ent(eid, CT_CELLAPP);

	reg.bindActor(basePart, 11, CT_BASEAPP, 1000);
	reg.bindActor(cellPart, 22, CT_CELLAPP, 1000);

	CHECK_EQ_INT(reg.actorCount(), 2);
	CHECK_EQ_INT(reg.findActor(basePart)->connId, 11);
	CHECK_EQ_INT(reg.findActor(cellPart)->connId, 22);

	// 只注销 cell 部分，base 部分不受影响
	reg.unbindActor(cellPart);
	CHECK(reg.findActor(cellPart) == NULL);
	CHECK_EQ_INT(reg.findActor(basePart)->connId, 11);
	CHECK_EQ_INT(reg.actorCount(), 1);
}

//-------------------------------------------------------------------------------------
// R03 迁移挂起：缓冲 -> RESUME 按到达顺序冲刷到新宿主
//-------------------------------------------------------------------------------------
static void case_R03_suspend_resume_flush_order()
{
	ActorRegistry reg;

	MailboxAddress a = ent(3003, CT_BASEAPP);
	reg.bindActor(a, 10, CT_BASEAPP, 1000);

	CHECK(reg.suspendActor(a, 1000, 30000));
	CHECK(reg.isSuspended(a));

	bool dropped = false;
	CHECK(reg.bufferMail(a, "mail-1", 512, dropped));
	CHECK(reg.bufferMail(a, "mail-2", 512, dropped));
	CHECK(reg.bufferMail(a, "mail-3", 512, dropped));
	CHECK(!dropped);
	CHECK_EQ_INT(reg.suspendedMailCount(), 3);

	// 挂起期间 Actor 仍在路由表上(只是消息进缓冲)
	CHECK(reg.findActor(a) != NULL);

	FlushResult fr;
	CHECK(reg.resumeActor(a, 99, 2000, fr));

	CHECK_EQ_INT(fr.connId, 99);
	CHECK_EQ_INT(fr.mails.size(), 3);
	CHECK_EQ_STR(fr.mails[0], "mail-1");
	CHECK_EQ_STR(fr.mails[1], "mail-2");
	CHECK_EQ_STR(fr.mails[2], "mail-3");

	// 冲刷后：路由改指新宿主、挂起状态清除
	CHECK(!reg.isSuspended(a));
	CHECK_EQ_INT(reg.findActor(a)->connId, 99);
	CHECK_EQ_INT(reg.suspendedMailCount(), 0);
}

//-------------------------------------------------------------------------------------
// R04 迁移挂起：缓冲溢出丢弃最旧的一条
//-------------------------------------------------------------------------------------
static void case_R04_suspend_overflow_drops_oldest()
{
	ActorRegistry reg;

	MailboxAddress a = ent(4004, CT_BASEAPP);
	reg.bindActor(a, 10, CT_BASEAPP, 0);
	reg.suspendActor(a, 1000, 30000);

	bool dropped = false;
	CHECK(reg.bufferMail(a, "old-1", 2, dropped));
	CHECK(!dropped);
	CHECK(reg.bufferMail(a, "old-2", 2, dropped));
	CHECK(!dropped);

	// 第三条触发溢出：丢弃最旧的 old-1
	CHECK(reg.bufferMail(a, "new-3", 2, dropped));
	CHECK(dropped);

	CHECK_EQ_INT(reg.suspendedMailCount(), 2);

	FlushResult fr;
	reg.resumeActor(a, 10, 2000, fr);
	CHECK_EQ_INT(fr.mails.size(), 2);
	CHECK_EQ_STR(fr.mails[0], "old-2");
	CHECK_EQ_STR(fr.mails[1], "new-3");
}

//-------------------------------------------------------------------------------------
// R05 迁移挂起：RESUME 超时未到达 -> 回滚冲刷到原宿主
//-------------------------------------------------------------------------------------
static void case_R05_suspend_timeout_rollback()
{
	ActorRegistry reg;

	MailboxAddress a = ent(5005, CT_BASEAPP);
	reg.bindActor(a, 10, CT_BASEAPP, 0);
	reg.suspendActor(a, 1000, 30000);

	bool dropped = false;
	reg.bufferMail(a, "pending-move-1", 512, dropped);
	reg.bufferMail(a, "pending-move-2", 512, dropped);

	// 未到期：不产出
	std::vector<FlushResult> expired;
	reg.takeExpiredSuspends(1000 + 29999, expired);
	CHECK_EQ_INT(expired.size(), 0);
	CHECK(reg.isSuspended(a));

	// 到期：冲刷回原宿主连接(10)
	reg.takeExpiredSuspends(1000 + 30000, expired);
	CHECK_EQ_INT(expired.size(), 1);
	CHECK_EQ_INT(expired[0].connId, 10);
	CHECK_EQ_INT(expired[0].mails.size(), 2);
	CHECK_EQ_STR(expired[0].mails[0], "pending-move-1");
	CHECK_EQ_STR(expired[0].mails[1], "pending-move-2");

	// 回滚后挂起状态清除、路由仍指向原宿主
	CHECK(!reg.isSuspended(a));
	CHECK_EQ_INT(reg.findActor(a)->connId, 10);
}

//-------------------------------------------------------------------------------------
// R06 待定缓冲：目标未注册时入队(顶替旧 ForwardComponent_MessageBuffer)
//-------------------------------------------------------------------------------------
static void case_R06_pending_enqueue()
{
	ActorRegistry reg;

	MailboxAddress dst = ent(6006, CT_CELLAPP);	// 目标 cell 部分还没建好
	MailboxAddress src = comp(7, CT_BASEAPP);

	CHECK(!reg.hasPending(dst));

	PendingMail droppedMail;
	bool dropped = false;

	CHECK(reg.bufferPendingMail(dst, "cell-not-ready-1", src, 101, 5,
		1000, 30000, 512, droppedMail, dropped));
	CHECK(!dropped);

	CHECK(reg.bufferPendingMail(dst, "cell-not-ready-2", src, 102, 5,
		1500, 30000, 512, droppedMail, dropped));
	CHECK(!dropped);

	CHECK(reg.hasPending(dst));
	CHECK_EQ_INT(reg.pendingCount(), 1);
	CHECK_EQ_INT(reg.pendingMailCount(), 2);

	// 目标未注册，路由表里查不到
	CHECK(reg.findActor(dst) == NULL);

	// TTL 从"首次不可达"算起，后续入队不刷新
	std::vector<ExpiredPendingMail> expired;
	reg.takeExpiredPendings(1000 + 30000, expired);
	CHECK_EQ_INT(expired.size(), 2);
}

//-------------------------------------------------------------------------------------
// R07 待定缓冲：目标注册即冲刷，且顺序与到达顺序一致
//-------------------------------------------------------------------------------------
static void case_R07_pending_flush_on_register()
{
	ActorRegistry reg;

	MailboxAddress dst = ent(7007, CT_CELLAPP);
	MailboxAddress src1 = comp(7, CT_BASEAPP);
	MailboxAddress src2 = comp(8, CT_BASEAPP);

	PendingMail droppedMail;
	bool dropped = false;

	reg.bufferPendingMail(dst, "p1", src1, 201, 5, 1000, 30000, 512, droppedMail, dropped);
	reg.bufferPendingMail(dst, "p2", src2, 202, 6, 1000, 30000, 512, droppedMail, dropped);
	reg.bufferPendingMail(dst, "p3", src1, 203, 5, 1000, 30000, 512, droppedMail, dropped);

	// ---- 注册时的正确顺序：先 takePending 再 bindActor(router_core.cpp 同款) ----
	FlushResult fr;
	reg.takePending(dst, 42, fr);
	reg.bindActor(dst, 42, CT_CELLAPP, 2000);

	CHECK_EQ_INT(fr.connId, 42);
	CHECK_EQ_INT(fr.mails.size(), 3);
	CHECK_EQ_STR(fr.mails[0], "p1");
	CHECK_EQ_STR(fr.mails[1], "p2");
	CHECK_EQ_STR(fr.mails[2], "p3");

	CHECK(!reg.hasPending(dst));
	CHECK_EQ_INT(reg.pendingCount(), 0);
	CHECK_EQ_INT(reg.pendingMailCount(), 0);
	CHECK_EQ_INT(reg.findActor(dst)->connId, 42);
}

//-------------------------------------------------------------------------------------
// R08 待定缓冲：溢出丢弃最旧的一条，并把被丢弃的那条交给调用方回报失败
//-------------------------------------------------------------------------------------
static void case_R08_pending_overflow_drops_oldest()
{
	ActorRegistry reg;

	MailboxAddress dst = ent(8008, CT_CELLAPP);
	MailboxAddress src = comp(7, CT_BASEAPP);

	PendingMail droppedMail;
	bool dropped = false;

	reg.bufferPendingMail(dst, "oldest", src, 301, 5, 1000, 30000, 2, droppedMail, dropped);
	CHECK(!dropped);

	reg.bufferPendingMail(dst, "middle", src, 302, 5, 1000, 30000, 2, droppedMail, dropped);
	CHECK(!dropped);

	reg.bufferPendingMail(dst, "newest", src, 303, 5, 1000, 30000, 2, droppedMail, dropped);
	CHECK(dropped);
	CHECK_EQ_STR(droppedMail.payload, "oldest");
	CHECK_EQ_INT(droppedMail.msgId, 301);
	CHECK_EQ_INT(droppedMail.srcConnId, 5);

	CHECK_EQ_INT(reg.pendingMailCount(), 2);

	FlushResult fr;
	reg.takePending(dst, 42, fr);
	CHECK_EQ_STR(fr.mails[0], "middle");
	CHECK_EQ_STR(fr.mails[1], "newest");
}

//-------------------------------------------------------------------------------------
// R09 待定缓冲：TTL 到期逐条回报(每条 Mail 的发送方可能不同)
//-------------------------------------------------------------------------------------
static void case_R09_pending_ttl_expire_per_mail()
{
	ActorRegistry reg;

	MailboxAddress dst = ent(9009, CT_CELLAPP);
	MailboxAddress src1 = comp(7, CT_BASEAPP);
	MailboxAddress src2 = comp(8, CT_BASEAPP);

	PendingMail droppedMail;
	bool dropped = false;

	reg.bufferPendingMail(dst, "m1", src1, 401, 5, 1000, 30000, 512, droppedMail, dropped);
	reg.bufferPendingMail(dst, "m2", src2, 402, 6, 1000, 30000, 512, droppedMail, dropped);

	// 未到期
	std::vector<ExpiredPendingMail> expired;
	reg.takeExpiredPendings(1000 + 29999, expired);
	CHECK_EQ_INT(expired.size(), 0);

	// 到期：逐条回报，dst/src/msgId/srcConnId 都可还原
	reg.takeExpiredPendings(1000 + 30000, expired);
	CHECK_EQ_INT(expired.size(), 2);

	CHECK_EQ_INT(expired[0].msgId, 401);
	CHECK_EQ_INT(expired[0].srcConnId, 5);
	CHECK(expired[0].src == src1);
	CHECK(expired[0].dst == dst);

	CHECK_EQ_INT(expired[1].msgId, 402);
	CHECK_EQ_INT(expired[1].srcConnId, 6);
	CHECK(expired[1].src == src2);

	// 到期后缓冲被清理
	CHECK(!reg.hasPending(dst));
	CHECK_EQ_INT(reg.pendingCount(), 0);
}

//-------------------------------------------------------------------------------------
// R10 顺序约束(回归防护)：bindActor() 会清掉待定缓冲，
//     因此"注册即冲刷"必须先 takePending() 再 bindActor()。
//     这条用例把错误顺序固化下来，防止有人调换调用次序导致静默丢消息。
//-------------------------------------------------------------------------------------
static void case_R10_bind_before_takepending_drops_mail()
{
	ActorRegistry reg;

	MailboxAddress dst = ent(1010, CT_CELLAPP);
	MailboxAddress src = comp(7, CT_BASEAPP);

	PendingMail droppedMail;
	bool dropped = false;
	reg.bufferPendingMail(dst, "will-be-dropped", src, 501, 5, 1000, 30000, 512, droppedMail, dropped);
	CHECK(reg.hasPending(dst));

	// 错误顺序：直接 bindActor
	reg.bindActor(dst, 42, CT_CELLAPP, 2000);

	CHECK(!reg.hasPending(dst));
	CHECK_EQ_INT(reg.pendingMailCount(), 0);

	FlushResult fr;
	reg.takePending(dst, 42, fr);
	CHECK_EQ_INT(fr.mails.size(), 0);
}

//-------------------------------------------------------------------------------------
// R11 显式丢弃：dropPending / dropSuspended
//-------------------------------------------------------------------------------------
static void case_R11_drop_pending_and_suspended()
{
	ActorRegistry reg;

	MailboxAddress a = ent(1111, CT_CELLAPP);
	reg.bindActor(a, 10, CT_CELLAPP, 0);

	PendingMail droppedMail;
	bool dropped = false;
	reg.bufferPendingMail(a, "p", comp(7, CT_BASEAPP), 601, 5, 0, 30000, 512, droppedMail, dropped);
	CHECK(reg.hasPending(a));

	reg.dropPending(a);
	CHECK(!reg.hasPending(a));
	CHECK_EQ_INT(reg.pendingMailCount(), 0);

	reg.suspendActor(a, 0, 30000);
	CHECK(reg.isSuspended(a));
	reg.dropSuspended(a);
	CHECK(!reg.isSuspended(a));
}

//-------------------------------------------------------------------------------------
// R12 连接断开兜底：清理该连接名下的 Actor / Service，
//     但保留 pending_(其键是"目标"地址，与目标是否属于该连接无关)
//-------------------------------------------------------------------------------------
static void case_R12_remove_conn()
{
	ActorRegistry reg;

	MailboxAddress a1 = ent(1201, CT_CELLAPP);
	MailboxAddress a2 = ent(1202, CT_CELLAPP);

	reg.bindActor(a1, 5, CT_CELLAPP, 0);
	reg.bindActor(a2, 6, CT_CELLAPP, 0);
	reg.bindService("Login", a1);
	reg.bindService("Login", a2);

	// 目标尚未注册的待定缓冲，不应被连接断开误伤
	PendingMail droppedMail;
	bool dropped = false;
	reg.bufferPendingMail(ent(1299, CT_CELLAPP), "target-starting",
		comp(7, CT_BASEAPP), 701, 5, 0, 30000, 512, droppedMail, dropped);

	std::vector<ActorKey> removedActors;
	std::vector<std::string> removedServices;
	reg.removeConn(5, removedActors, removedServices);

	CHECK_EQ_INT(removedActors.size(), 1);
	CHECK_EQ_INT(removedActors[0].actorId, 1201);
	CHECK_EQ_INT(removedServices.size(), 1);
	CHECK_EQ_STR(removedServices[0], "Login");

	CHECK(reg.findActor(a1) == NULL);
	CHECK(reg.findActor(a2) != NULL);
	CHECK_EQ_INT(reg.findService("Login").size(), 1);

	// 待定缓冲仍在，交由 TTL 清理
	CHECK_EQ_INT(reg.pendingMailCount(), 1);
}

//-------------------------------------------------------------------------------------
int main()
{
	BEGIN_CASE("R01", "bind / find / unbind actor");
	case_R01_bind_find_unbind();
	END_CASE();

	BEGIN_CASE("R02", "ActorKey carries componentType (base vs cell isolation)");
	case_R02_actorkey_componenttype_isolation();
	END_CASE();

	BEGIN_CASE("R03", "SUSPEND -> buffer -> RESUME flush in arrival order");
	case_R03_suspend_resume_flush_order();
	END_CASE();

	BEGIN_CASE("R04", "SUSPEND buffer overflow drops the oldest");
	case_R04_suspend_overflow_drops_oldest();
	END_CASE();

	BEGIN_CASE("R05", "SUSPEND timeout rolls back to the original host");
	case_R05_suspend_timeout_rollback();
	END_CASE();

	BEGIN_CASE("R06", "PENDING buffer enqueues while target unregistered");
	case_R06_pending_enqueue();
	END_CASE();

	BEGIN_CASE("R07", "PENDING flushed on register, in arrival order");
	case_R07_pending_flush_on_register();
	END_CASE();

	BEGIN_CASE("R08", "PENDING overflow drops the oldest and reports it");
	case_R08_pending_overflow_drops_oldest();
	END_CASE();

	BEGIN_CASE("R09", "PENDING TTL expiry reports every buffered mail");
	case_R09_pending_ttl_expire_per_mail();
	END_CASE();

	BEGIN_CASE("R10", "regression: bindActor() before takePending() loses mail");
	case_R10_bind_before_takepending_drops_mail();
	END_CASE();

	BEGIN_CASE("R11", "dropPending / dropSuspended release buffers");
	case_R11_drop_pending_and_suspended();
	END_CASE();

	BEGIN_CASE("R12", "removeConn clears actors/services, keeps pending");
	case_R12_remove_conn();
	END_CASE();

	return g_runner.summary("router_registry_test");
}
