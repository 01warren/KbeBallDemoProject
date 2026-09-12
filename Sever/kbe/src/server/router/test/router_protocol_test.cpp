// Copyright 2008-2018 Yolo Technologies, Inc. All Rights Reserved. https://www.comblockengine.com

/*
	router_protocol_test.cpp —— 协议级序列测试（零 KBE 依赖，进程内）。

	为什么不是"起真实 router 进程"：
		真实端到端需要构建 router.exe。当前环境只有 VS2022(v143) 工具集，
		而 kbe/src/libs 下的预编译库是用 v142 + /GL 构建的，用 v143 链接必然
		C1905（LTCG 前后端版本不兼容），python37 又无法在本机重建。
		因此这里退一步：**用真实的 ActorRegistry + 真实的 RouterInterface 线格式**，
		把 RouterCore 的消息分派顺序复刻成 driver，验证"协议 + 路由表"的组合语义。
		Makefile/脚本里保留了 `--with-router` 的真实进程模式（环境具备时可跑）。

	被测对象：
		- kbe/src/lib/server/router_interface.h 的 Wire 编解码 / Mailbox 线格式
		- kbe/src/server/router/actor_registry.{h,cpp} 的 PENDING / SUSPEND 语义
		- RouterCore 的分派顺序（由 driver 复刻；顺序本身由 R10 用例防回退）

	driver 复刻的分派顺序（与 router_core.cpp 一一对应）：
		MSG_REGISTER_ACTOR : isSuspended -> resumeActor
		                     hasPending   -> takePending + bindActor
		                     else         -> bindActor
		MSG_SEND           : 已注册且未挂起 -> 立即投递
		                     已挂起         -> bufferMail
		                     未注册         -> bufferPendingMail
		MSG_SUSPEND_ACTOR  : suspendActor
		MSG_RESUME_ACTOR   : resumeActor

	构建(Windows)：cl /nologo /EHsc /W3 /I"<repo>\kbe\src" /I"<repo>\kbe\src\lib" ^
	                   router_protocol_test.cpp ..\actor_registry.cpp /Fe:router_protocol_test.exe
	构建(Linux)  ：g++ -std=c++11 -O2 -Wall -I<repo>/kbe/src -I<repo>/kbe/src/lib ^
	                   router_protocol_test.cpp ../actor_registry.cpp -o router_protocol_test
	退出码：0 = 全部通过；1 = 有用例失败。
*/

#include "server/router/actor_registry.h"
#include "test_common.h"

#include <string>
#include <vector>

TestRunner g_runner;

using namespace KBEngine;
using namespace KBEngine::RouterInterface;

typedef MailboxAddress Addr;

static const uint32_t CT_BASEAPP = 2;
static const uint32_t CT_CELLAPP = 3;

//=====================================================================
// RouterCore 分派顺序的复刻（driver）
//=====================================================================
struct Delivery
{
	Addr	dst;
	uint64_t connId;
	std::string body;
	uint64_t msgId;
};

class RouterCoreDriver
{
public:
	RouterCoreDriver() : now_(1000), nextMsgId_(1) {}

	// MSG_REGISTER_ACTOR
	void onRegister(const Addr& addr, uint64_t connId)
	{
		if(reg_.isSuspended(addr))
		{
			FlushResult fr;
			reg_.resumeActor(addr, connId, now_, fr);
			collect(fr);
			return;
		}

		if(reg_.hasPending(addr))
		{
			FlushResult fr;
			reg_.takePending(addr, connId, fr);	// 必须先 take 再 bind
			reg_.bindActor(addr, connId, addr.componentType, now_);
			collect(fr);
			return;
		}

		reg_.bindActor(addr, connId, addr.componentType, now_);
	}

	// MSG_SEND：返回 true 表示"立即投递成功"
	bool onSend(const Addr& dst, const Addr& src, const std::string& body, uint64_t srcConnId)
	{
		const uint64_t msgId = nextMsgId_++;

		const ActorEntry* e = reg_.findActor(dst);

		if(e != NULL && !reg_.isSuspended(dst))
		{
			Delivery d;
			d.dst = dst;
			d.connId = e->connId;
			d.body = body;
			d.msgId = msgId;
			delivered_.push_back(d);
			return true;
		}

		if(reg_.isSuspended(dst))
		{
			bool dropped = false;
			reg_.bufferMail(dst, body, maxBuffered_, dropped);
			return false;	// 未立即投递：进入迁移挂起缓冲
		}

		// 目标未注册 -> PENDING
		PendingMail droppedMail;
		bool dropped = false;

		reg_.bufferPendingMail(dst, body, src, msgId, srcConnId,
			now_, timeoutMS_, maxBuffered_, droppedMail, dropped);

		return false;		// 未立即投递：进入待定缓冲
	}

	void onSuspend(const Addr& addr) { reg_.suspendActor(addr, now_, timeoutMS_); }

	void onResume(const Addr& addr, uint64_t connId)
	{
		FlushResult fr;
		reg_.resumeActor(addr, connId, now_, fr);
		collect(fr);
	}

	// 时钟推进 + 到期清理（takeExpiredPendings / takeExpiredSuspends）
	void advance(uint64_t ms)
	{
		now_ += ms;

		std::vector<ExpiredPendingMail> expired;
		reg_.takeExpiredPendings(now_, expired);
		expired_.insert(expired_.end(), expired.begin(), expired.end());
	}

	// ---- 观测 ----
	const std::vector<Delivery>& delivered() const { return delivered_; }
	const std::vector<ExpiredPendingMail>& expired() const { return expired_; }
	ActorRegistry& registry() { return reg_; }

	void setMaxBuffered(size_t n) { maxBuffered_ = n; }
	void setTimeout(uint64_t ms) { timeoutMS_ = ms; }

private:
	void collect(const FlushResult& fr)
	{
		for(size_t i = 0; i < fr.mails.size(); ++i)
		{
			Delivery d;
			d.dst = fr.addr;
			d.connId = fr.connId;
			d.body = fr.mails[i];
			d.msgId = 0;	// 冲刷时沿用原始 msgId，这里不参与断言
			delivered_.push_back(d);
		}
	}

	ActorRegistry					reg_;
	uint64_t						now_;
	uint64_t						nextMsgId_;
	size_t							maxBuffered_ = 512;
	uint64_t						timeoutMS_ = 30000;

	std::vector<Delivery>			delivered_;
	std::vector<ExpiredPendingMail>	expired_;
};

//=====================================================================
// 线格式编解码助手（使用真实的 RouterInterface::Wire）
//=====================================================================
static std::string encodeMailbox(const Addr& a)
{
	std::string b;
	putMailbox(b, a);
	return b;
}

static bool decodeMailbox(const std::string& b, Addr& a)
{
	size_t off = 0;
	return getMailbox(b.data(), b.size(), off, a);
}

static std::string encodeSend(const Addr& dst, const Addr& src, uint64_t msgId,
	const std::string& body)
{
	std::string payload;
	putMailbox(payload, dst);
	putMailbox(payload, src);
	Wire::putU64(payload, msgId);
	payload.append(body);
	return payload;
}

//-------------------------------------------------------------------------------------
// P01 Mailbox 线格式：13 字节，编解码往返
//-------------------------------------------------------------------------------------
static void case_P01_mailbox_wire()
{
	Addr a = mailboxEntity(4242, CT_CELLAPP);

	const std::string bytes = encodeMailbox(a);
	CHECK_EQ_INT(bytes.size(), MAILBOX_WIRE_SIZE);

	Addr out;
	CHECK(decodeMailbox(bytes, out));
	CHECK_EQ_INT((int)out.kind, (int)a.kind);
	CHECK_EQ_INT(out.componentType, a.componentType);
	CHECK_EQ_INT(out.actorId, a.actorId);

	// 截断输入必须失败（不信任外部字节）
	std::string truncated = bytes.substr(0, MAILBOX_WIRE_SIZE - 1);
	CHECK(!decodeMailbox(truncated, out));
}

//-------------------------------------------------------------------------------------
// P02 MSG_SEND 载荷：dst(13) + src(13) + msgId(8) + body
//-------------------------------------------------------------------------------------
static void case_P02_send_payload()
{
	Addr dst = mailboxEntity(1001, CT_CELLAPP);
	Addr src = mailboxComponent(7, CT_BASEAPP);
	const std::string body("hello-body");

	const std::string payload = encodeSend(dst, src, 987654321, body);
	CHECK_EQ_INT(payload.size(), SEND_HEADER_WIRE_SIZE + body.size());

	size_t off = 0;
	Addr outDst, outSrc;
	uint64_t msgId = 0;

	CHECK(getMailbox(payload.data(), payload.size(), off, outDst));
	CHECK(getMailbox(payload.data(), payload.size(), off, outSrc));
	CHECK(Wire::getU64(payload.data(), payload.size(), off, msgId));

	CHECK_EQ_INT(outDst.actorId, dst.actorId);
	CHECK_EQ_INT(outSrc.actorId, src.actorId);
	CHECK_EQ_INT(msgId, 987654321);
	CHECK_EQ_STR(payload.substr(off), body);
}

//-------------------------------------------------------------------------------------
// I01(进程内) 目标未注册 -> PENDING 缓冲 -> 注册后按序冲刷
//-------------------------------------------------------------------------------------
static void case_I01_pending_then_register()
{
	RouterCoreDriver d;

	Addr dst = mailboxEntity(2001, CT_CELLAPP);
	Addr src = mailboxComponent(7, CT_BASEAPP);

	// 目标不存在：不应"立即投递"，也不应被判为失败丢弃
	CHECK(!d.onSend(dst, src, "cell-not-ready-1", 5));
	CHECK(!d.onSend(dst, src, "cell-not-ready-2", 5));
	CHECK(!d.onSend(dst, src, "cell-not-ready-3", 5));
	CHECK_EQ_INT(d.delivered().size(), 0);
	CHECK(d.registry().hasPending(dst));

	// 目标注册即冲刷：顺序与到达顺序一致
	d.onRegister(dst, 42);

	CHECK_EQ_INT(d.delivered().size(), 3);
	CHECK_EQ_INT(d.delivered()[0].connId, 42);
	CHECK_EQ_STR(d.delivered()[0].body, "cell-not-ready-1");
	CHECK_EQ_STR(d.delivered()[1].body, "cell-not-ready-2");
	CHECK_EQ_STR(d.delivered()[2].body, "cell-not-ready-3");

	// 注册之后的新消息直接投递
	CHECK(d.onSend(dst, src, "after-register", 5));
	CHECK_EQ_INT(d.delivered().size(), 4);
	CHECK_EQ_STR(d.delivered()[3].body, "after-register");
}

//-------------------------------------------------------------------------------------
// I02(进程内) 待定缓冲溢出：超出 pendingBufferMax 后最旧的被丢弃并回报
//-------------------------------------------------------------------------------------
static void case_I02_pending_overflow()
{
	RouterCoreDriver d;
	d.setMaxBuffered(2);

	Addr dst = mailboxEntity(2002, CT_CELLAPP);
	Addr src = mailboxComponent(7, CT_BASEAPP);

	d.onSend(dst, src, "m-1", 5);
	d.onSend(dst, src, "m-2", 5);
	d.onSend(dst, src, "m-3", 5);	// 触发溢出，丢弃 m-1

	CHECK_EQ_INT(d.registry().pendingMailCount(), 2);

	d.onRegister(dst, 42);
	CHECK_EQ_INT(d.delivered().size(), 2);
	CHECK_EQ_STR(d.delivered()[0].body, "m-2");
	CHECK_EQ_STR(d.delivered()[1].body, "m-3");
}

//-------------------------------------------------------------------------------------
// I03(进程内) 待定缓冲 TTL 到期：逐条回 DELIVERY_FAILED(DROPPED_PENDING)
//-------------------------------------------------------------------------------------
static void case_I03_pending_ttl()
{
	RouterCoreDriver d;
	d.setTimeout(30000);

	Addr dst = mailboxEntity(2003, CT_CELLAPP);
	Addr src = mailboxComponent(7, CT_BASEAPP);

	d.onSend(dst, src, "will-expire-1", 5);
	d.onSend(dst, src, "will-expire-2", 6);

	// 未到期
	d.advance(29999);
	CHECK_EQ_INT(d.expired().size(), 0);

	// 到期：每条缓冲的 Mail 都要能拿到回报所需的全部信息
	d.advance(2);
	CHECK_EQ_INT(d.expired().size(), 2);
	CHECK_EQ_INT(d.expired()[0].srcConnId, 5);
	CHECK_EQ_INT(d.expired()[1].srcConnId, 6);
	CHECK(d.expired()[0].dst == dst);
	CHECK(d.expired()[0].src == src);

	// 到期后缓冲清空，再注册不会重复投递
	d.onRegister(dst, 42);
	CHECK_EQ_INT(d.delivered().size(), 0);
}

//-------------------------------------------------------------------------------------
// I04(进程内) 组件 Actor 寻址：cellapp 组件地址可注册、可被 MSG_SEND 命中
//-------------------------------------------------------------------------------------
static void case_I04_component_actor_addressing()
{
	RouterCoreDriver d;

	Addr cellA = mailboxComponent(11, CT_CELLAPP);
	Addr cellB = mailboxComponent(12, CT_CELLAPP);
	Addr src = mailboxComponent(7, CT_BASEAPP);

	d.onRegister(cellA, 101);
	d.onRegister(cellB, 102);

	CHECK(d.onSend(cellA, src, "to-cellA", 5));
	CHECK(d.onSend(cellB, src, "to-cellB", 5));

	CHECK_EQ_INT(d.delivered().size(), 2);
	CHECK_EQ_INT(d.delivered()[0].connId, 101);
	CHECK_EQ_INT(d.delivered()[1].connId, 102);

	// 组件地址与实体地址即使 actorId 相同也不冲突（kind 不同）
	Addr entity11 = mailboxEntity(11, CT_CELLAPP);
	CHECK(!(entity11 == cellA));
	d.onRegister(entity11, 201);
	CHECK(d.onSend(entity11, src, "to-entity11", 5));
	CHECK_EQ_INT(d.delivered()[2].connId, 201);
}

//-------------------------------------------------------------------------------------
// I05(进程内) 迁移序列：SUSPEND -> 缓冲 -> RESUME -> 按序冲刷到新宿主
//-------------------------------------------------------------------------------------
static void case_I05_migration_suspend_resume()
{
	RouterCoreDriver d;

	Addr entity = mailboxEntity(2005, CT_BASEAPP);
	Addr src = mailboxComponent(7, CT_BASEAPP);

	d.onRegister(entity, 10);
	d.onSuspend(entity);

	CHECK(d.registry().isSuspended(entity));

	d.onSend(entity, src, "during-move-1", 5);
	d.onSend(entity, src, "during-move-2", 5);
	CHECK_EQ_INT(d.delivered().size(), 0);

	// 迁移完成：目标注册（RESUME 语义）
	d.onResume(entity, 99);

	CHECK_EQ_INT(d.delivered().size(), 2);
	CHECK_EQ_INT(d.delivered()[0].connId, 99);
	CHECK_EQ_STR(d.delivered()[0].body, "during-move-1");
	CHECK_EQ_STR(d.delivered()[1].body, "during-move-2");

	// 迁移后的新消息直达新宿主
	d.onSend(entity, src, "after-move", 5);
	CHECK_EQ_INT(d.delivered().size(), 3);
	CHECK_EQ_INT(d.delivered()[2].connId, 99);
}

//-------------------------------------------------------------------------------------
// I06(进程内) 顺序约束：迁移中目标注册(REGISTER) 等价于 RESUME，不能丢缓冲
//-------------------------------------------------------------------------------------
static void case_I06_register_during_migration_is_resume()
{
	RouterCoreDriver d;

	Addr entity = mailboxEntity(2006, CT_BASEAPP);
	Addr src = mailboxComponent(7, CT_BASEAPP);

	d.onRegister(entity, 10);
	d.onSuspend(entity);
	d.onSend(entity, src, "buffered", 5);

	// 迁移期间"目标注册"必须走 RESUME 分支（保留并冲刷），而不是 bindActor 分支
	d.onRegister(entity, 77);

	CHECK_EQ_INT(d.delivered().size(), 1);
	CHECK_EQ_INT(d.delivered()[0].connId, 77);
	CHECK(!d.registry().isSuspended(entity));
	CHECK_EQ_INT(d.registry().findActor(entity)->connId, 77);
}

//-------------------------------------------------------------------------------------
int main()
{
	BEGIN_CASE("P01", "mailbox wire format (13 bytes) roundtrip");
	case_P01_mailbox_wire();
	END_CASE();

	BEGIN_CASE("P02", "MSG_SEND payload encode/decode");
	case_P02_send_payload();
	END_CASE();

	BEGIN_CASE("I01", "send to unregistered -> pending -> flush on register");
	case_I01_pending_then_register();
	END_CASE();

	BEGIN_CASE("I02", "pending overflow drops the oldest");
	case_I02_pending_overflow();
	END_CASE();

	BEGIN_CASE("I03", "pending TTL expiry reports every mail");
	case_I03_pending_ttl();
	END_CASE();

	BEGIN_CASE("I04", "component Actor addressing works");
	case_I04_component_actor_addressing();
	END_CASE();

	BEGIN_CASE("I05", "migration SUSPEND -> buffer -> RESUME flush");
	case_I05_migration_suspend_resume();
	END_CASE();

	BEGIN_CASE("I06", "REGISTER during migration acts as RESUME");
	case_I06_register_during_migration_is_resume();
	END_CASE();

	return g_runner.summary("router_protocol_test");
}
