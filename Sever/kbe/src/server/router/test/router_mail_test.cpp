// Copyright 2008-2018 Yolo Technologies, Inc. All Rights Reserved. https://www.comblockengine.com

/*
	router_mail_test.cpp —— RouterMail 契约与"中继外壳剥离"单元测试。

	被测对象：kbe/src/lib/server/router_mail.h
	覆盖能力(设计文档 §10 / §14.2.1)：
	  - BodyTag 契约：buildBody / parseBody / parseBodyPayload 的往返与非法输入
	  - 地址推导：entityMailbox / componentMailbox / serviceMailbox
	  - **cellapp -> client 的中继外壳剥离**：legacy 通路把客户端消息包在
	    BaseappInterface::forwardMessageToClientFromCellapp 里经 baseapp 中继，
	    Router 模式下必须剥掉 [msgID][msgLen][entityID] 这层外壳，只把
	    "客户端协议消息序列"作为 BODY_CLIENT_MESSAGE 的载荷投递。
	    这里是本次收敛最容易出错的地方(偏移/扩展长度/越界)，因此逐字节验证。

	构建(Windows, VS 开发者命令行)：
		cl /nologo /EHsc /W3 /I"<repo>\kbe\src" router_mail_test.cpp /Fe:router_mail_test.exe
	构建(Linux)：
		g++ -std=c++11 -O2 -Wall -I<repo>/kbe/src router_mail_test.cpp -o router_mail_test
	退出码：0 = 全部通过；1 = 有用例失败。
*/

#include "server/router_mail.h"
#include "test_common.h"

#include <string>

TestRunner g_runner;

using namespace KBEngine;
using namespace KBEngine::RouterMail;

static const uint32_t CT_BASEAPP = 2;
static const uint32_t CT_CELLAPP = 3;
static const uint32_t CT_CLIENT	 = 6;

// 中继消息的 msgID(这里用任意值，测试只关心剥离规则本身)
static const uint16_t RELAY_MSG_ID = 0x0B01;

/**
	构造一段"拍平的 legacy 中继 bundle"：
		[msgID(2)][msgLen(2)][entityID(4)][客户端协议消息序列...]
	长度达到上限时追加扩展长度字段 [msgLen(2)==65535][extLen(4)]。
*/
static std::string makeRelayBundle(ENTITY_ID eid, const std::string& clientMsgs,
	bool useExtLen = false)
{
	std::string b;
	RouterInterface::Wire::putU16(b, RELAY_MSG_ID);

	const uint32_t payloadLen = (uint32_t)(4 + clientMsgs.size());

	if(useExtLen)
	{
		RouterInterface::Wire::putU16(b, (uint16_t)LEGACY_MSG_LEN_MAX);
		RouterInterface::Wire::putU32(b, payloadLen);
	}
	else
	{
		RouterInterface::Wire::putU16(b, (uint16_t)payloadLen);
	}

	RouterInterface::Wire::putU32(b, (uint32_t)eid);
	b.append(clientMsgs);
	return b;
}

//-------------------------------------------------------------------------------------
// M01 BodyTag 契约：buildBody / parseBody 往返
//-------------------------------------------------------------------------------------
static void case_M01_body_roundtrip()
{
	const BodyTag tags[] = {
		BODY_ENTITY_CALL, BODY_REMOTE_METHOD_CALL, BODY_REMOTE_CELL_FROM_CLIENT,
		BODY_FORWARD_TO_CLIENT, BODY_FORWARD_TO_CELLAPP,
		BODY_COMPONENT_MESSAGE, BODY_CLIENT_MESSAGE
	};

	const std::string payload = "\x01\x02\x03payload-bytes";

	for(size_t i = 0; i < sizeof(tags) / sizeof(tags[0]); ++i)
	{
		const std::string body = buildBody(tags[i], payload);

		CHECK_EQ_INT(body.size(), payload.size() + 1);
		CHECK_EQ_INT((int)(uint8_t)body[0], (int)tags[i]);

		BodyTag outTag = BODY_UNKNOWN;
		const char* outPayload = NULL;
		size_t outSize = 0;

		CHECK(parseBody(body, outTag, outPayload, outSize));
		CHECK_EQ_INT((int)outTag, (int)tags[i]);
		CHECK_EQ_INT(outSize, payload.size());
		CHECK(std::string(outPayload, outSize) == payload);
	}
}

//-------------------------------------------------------------------------------------
// M02 非法 body：空 / tag 为 0
//-------------------------------------------------------------------------------------
static void case_M02_invalid_body()
{
	BodyTag tag = BODY_ENTITY_CALL;
	const char* payload = NULL;
	size_t size = 0;

	CHECK(!parseBody(std::string(), tag, payload, size));
	CHECK_EQ_INT((int)tag, (int)BODY_UNKNOWN);

	std::string zeroTag;
	zeroTag.append(1, (char)0x00);
	zeroTag.append("payload");
	CHECK(!parseBody(zeroTag, tag, payload, size));

	// 只有 tag、没有载荷是合法的(载荷长度为 0)
	std::string onlyTag;
	onlyTag.append(1, (char)BODY_ENTITY_CALL);
	CHECK(parseBody(onlyTag, tag, payload, size));
	CHECK_EQ_INT((int)tag, (int)BODY_ENTITY_CALL);
	CHECK_EQ_INT(size, 0);
}

//-------------------------------------------------------------------------------------
// M03 parseBodyPayload 取出跳过 tag 的载荷副本
//-------------------------------------------------------------------------------------
static void case_M03_parse_body_payload()
{
	const std::string legacy = "entityID+methodcall";

	BodyTag tag = BODY_UNKNOWN;
	std::string out;

	CHECK(parseBodyPayload(buildBody(BODY_FORWARD_TO_CLIENT, legacy), tag, out));
	CHECK_EQ_INT((int)tag, (int)BODY_FORWARD_TO_CLIENT);
	CHECK(out == legacy);

	CHECK(!parseBodyPayload(std::string(), tag, out));
}

//-------------------------------------------------------------------------------------
// M04 地址推导：实体地址只由 ENTITY_ID 决定；组件地址带 componentID
//-------------------------------------------------------------------------------------
static void case_M04_mailbox_address()
{
	typedef RouterInterface::MailboxAddress Addr;

	Addr e1 = entityMailbox((ENTITY_ID)4242, CT_CELLAPP);
	CHECK_EQ_INT((int)e1.kind, (int)RouterInterface::MAILBOX_ENTITY);
	CHECK_EQ_INT(e1.componentType, CT_CELLAPP);
	CHECK_EQ_INT(e1.actorId, 4242);

	// 同一实体在 base 与 cell 上只有 componentType 不同。
	//
	// 注意：MailboxAddress::operator== 只比较 kind + actorId —— componentType 在
	// 协议注释里被定义为"元数据：便于观测"，因此这两个地址在 operator== 下**相等**。
	// 真正的 base/cell 隔离发生在路由表键 ActorKey 上(含 componentType，
	// 见 router_registry_test R02)，所以投递不会串。
	//
	// 这里固化当前语义：若将来把 componentType 纳入 operator==，本断言会失败，
	// 提醒评估 bindService/unbindService/findService 等按地址比较的调用点。
	Addr e1base = entityMailbox((ENTITY_ID)4242, CT_BASEAPP);
	CHECK(e1base == e1);
	CHECK_EQ_INT(e1base.componentType, CT_BASEAPP);
	CHECK_EQ_INT(e1base.actorId, e1.actorId);

	Addr c = componentMailbox((COMPONENT_ID)9, (COMPONENT_TYPE)CT_CELLAPP);
	CHECK_EQ_INT((int)c.kind, (int)RouterInterface::MAILBOX_COMPONENT);
	CHECK_EQ_INT(c.componentType, CT_CELLAPP);
	CHECK_EQ_INT(c.actorId, 9);

	Addr s = serviceMailbox("Login");
	CHECK_EQ_INT((int)s.kind, (int)RouterInterface::MAILBOX_SERVICE);

	CHECK(mailboxToString(e1) == "entity:4242");
	CHECK(bodyTagName(BODY_CLIENT_MESSAGE) == std::string("CLIENT_MESSAGE"));
}

//-------------------------------------------------------------------------------------
// M05 剥中继外壳：正常长度
//-------------------------------------------------------------------------------------
static void case_M05_strip_envelope_normal()
{
	// 客户端协议消息序列：[clientMsgID(2)][len(2)][body...]
	std::string clientMsgs;
	RouterInterface::Wire::putU16(clientMsgs, 0x0102);
	RouterInterface::Wire::putU16(clientMsgs, 3);
	clientMsgs.append("abc");

	const std::string bundle = makeRelayBundle(777, clientMsgs);

	std::string stripped;
	uint16_t outMsgID = 0;
	ENTITY_ID outEID = 0;

	CHECK(stripClientForwardEnvelope(bundle, stripped, &outMsgID, &outEID));
	CHECK_EQ_INT((int)outMsgID, (int)RELAY_MSG_ID);
	CHECK_EQ_INT(outEID, 777);
	CHECK(stripped == clientMsgs);
	CHECK_EQ_INT(stripped.size(), clientMsgs.size());

	// 剥离结果再包成 BODY_CLIENT_MESSAGE 后能被对端原样还原
	const std::string body = buildBody(BODY_CLIENT_MESSAGE, stripped);

	BodyTag tag = BODY_UNKNOWN;
	std::string payloadOut;
	CHECK(parseBodyPayload(body, tag, payloadOut));
	CHECK_EQ_INT((int)tag, (int)BODY_CLIENT_MESSAGE);
	CHECK(payloadOut == clientMsgs);
}

//-------------------------------------------------------------------------------------
// M06 剥中继外壳：使用扩展长度字段(超长同步包)
//-------------------------------------------------------------------------------------
static void case_M06_strip_envelope_extlen()
{
	std::string clientMsgs;
	RouterInterface::Wire::putU16(clientMsgs, 0x0304);
	RouterInterface::Wire::putU16(clientMsgs, 2);
	clientMsgs.append("hi");

	// 与 M05 同一份载荷，但强制走扩展长度分支
	const std::string bundle = makeRelayBundle(888, clientMsgs, true);

	std::string stripped;
	uint16_t outMsgID = 0;
	ENTITY_ID outEID = 0;

	CHECK(stripClientForwardEnvelope(bundle, stripped, &outMsgID, &outEID));
	CHECK_EQ_INT(outEID, 888);
	CHECK(stripped == clientMsgs);

	// 扩展长度分支必须多跳过 4 个字节，否则这里不会相等
	CHECK_EQ_INT(stripped.size(), clientMsgs.size());
}

//-------------------------------------------------------------------------------------
// M07 剥中继外壳：越界/截断输入一律返回 false(不信任外部字节)
//-------------------------------------------------------------------------------------
static void case_M07_strip_envelope_truncated()
{
	std::string stripped;

	CHECK(!stripClientForwardEnvelope(std::string(), stripped));
	CHECK(!stripClientForwardEnvelope(std::string("\x01", 1), stripped));

	// 只有 msgID，缺长度字段
	std::string onlyID;
	RouterInterface::Wire::putU16(onlyID, RELAY_MSG_ID);
	CHECK(!stripClientForwardEnvelope(onlyID, stripped));

	// 有长度字段，缺 entityID
	std::string noEID;
	RouterInterface::Wire::putU16(noEID, RELAY_MSG_ID);
	RouterInterface::Wire::putU16(noEID, 4);
	CHECK(!stripClientForwardEnvelope(noEID, stripped));

	// 声明走扩展长度但缺扩展字段
	std::string noExt;
	RouterInterface::Wire::putU16(noExt, RELAY_MSG_ID);
	RouterInterface::Wire::putU16(noExt, (uint16_t)LEGACY_MSG_LEN_MAX);
	CHECK(!stripClientForwardEnvelope(noExt, stripped));

	CHECK(stripped.empty());
}

//-------------------------------------------------------------------------------------
// M08 剥中继外壳：只有外壳没有客户端消息 -> 视为失败(没有可投递的内容)
//-------------------------------------------------------------------------------------
static void case_M08_strip_envelope_empty_payload()
{
	const std::string bundle = makeRelayBundle(999, std::string());

	std::string stripped;
	ENTITY_ID outEID = 0;

	CHECK(!stripClientForwardEnvelope(bundle, stripped, NULL, &outEID));
	CHECK(stripped.empty());
}

//-------------------------------------------------------------------------------------
// M09 端到端组合：Witness::update() 那种"多条客户端消息聚合在一个中继包里"的形态
//-------------------------------------------------------------------------------------
static void case_M09_strip_envelope_multi_messages()
{
	std::string clientMsgs;

	// 第一条客户端消息
	RouterInterface::Wire::putU16(clientMsgs, 0x0011);
	RouterInterface::Wire::putU16(clientMsgs, 4);
	clientMsgs.append("aaaa");

	// 第二条客户端消息
	RouterInterface::Wire::putU16(clientMsgs, 0x0012);
	RouterInterface::Wire::putU16(clientMsgs, 2);
	clientMsgs.append("bb");

	const std::string bundle = makeRelayBundle(1234, clientMsgs);

	std::string stripped;
	CHECK(stripClientForwardEnvelope(bundle, stripped));

	// 客户端按原有 MessageHandlers 逐条解析：这里逐条读出 msgID/len 验证
	size_t off = 0;
	uint16_t id1 = 0, len1 = 0, id2 = 0, len2 = 0;

	CHECK(RouterInterface::Wire::getU16(stripped.data(), stripped.size(), off, id1));
	CHECK(RouterInterface::Wire::getU16(stripped.data(), stripped.size(), off, len1));
	CHECK_EQ_INT((int)id1, 0x0011);
	CHECK_EQ_INT((int)len1, 4);
	CHECK(stripped.substr(off, len1) == "aaaa");
	off += len1;

	CHECK(RouterInterface::Wire::getU16(stripped.data(), stripped.size(), off, id2));
	CHECK(RouterInterface::Wire::getU16(stripped.data(), stripped.size(), off, len2));
	CHECK_EQ_INT((int)id2, 0x0012);
	CHECK_EQ_INT((int)len2, 2);
	CHECK(stripped.substr(off, len2) == "bb");
	off += len2;

	CHECK_EQ_INT(off, stripped.size());
}

//-------------------------------------------------------------------------------------
int main()
{
	BEGIN_CASE("M01", "BodyTag buildBody/parseBody roundtrip");
	case_M01_body_roundtrip();
	END_CASE();

	BEGIN_CASE("M02", "parseBody rejects empty/unknown body");
	case_M02_invalid_body();
	END_CASE();

	BEGIN_CASE("M03", "parseBodyPayload copies payload after tag");
	case_M03_parse_body_payload();
	END_CASE();

	BEGIN_CASE("M04", "mailbox address derivation");
	case_M04_mailbox_address();
	END_CASE();

	BEGIN_CASE("M05", "strip baseapp relay envelope (normal length)");
	case_M05_strip_envelope_normal();
	END_CASE();

	BEGIN_CASE("M06", "strip baseapp relay envelope (extended length)");
	case_M06_strip_envelope_extlen();
	END_CASE();

	BEGIN_CASE("M07", "strip envelope rejects truncated input");
	case_M07_strip_envelope_truncated();
	END_CASE();

	BEGIN_CASE("M08", "strip envelope with empty client payload fails");
	case_M08_strip_envelope_empty_payload();
	END_CASE();

	BEGIN_CASE("M09", "strip envelope keeps aggregated client messages");
	case_M09_strip_envelope_multi_messages();
	END_CASE();

	return g_runner.summary("router_mail_test");
}
