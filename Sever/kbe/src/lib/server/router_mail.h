// Copyright 2008-2018 Yolo Technologies, Inc. All Rights Reserved. https://www.comblockengine.com

/*
	router_mail.h —— Router 模式下"发信侧"与"派发侧"共用的唯一契约。

	位置说明：放在 lib/server 下，与 router_interface.h / router_client.h 同级。
	它同时被 lib/entitydef(发信: EntityCallAbstract) 与 各 App(派发: ServerApp::onRouterMail)
	依赖，因此这里只允许依赖 common/common.h 与 router_interface.h，不得引入任何
	组件级头文件。

	设计要点(详见 docs/kbe_router_design.md)：

	1. body 首字节是"自描述标签"(BodyTag)，用来顶替老路径的 newMessage(digest)。
	   老路径用 bundle.newMessage(BaseappInterface::onEntityCall) 写一个消息 digest，
	   接收端靠 digest 选 handler；而 router 只看 dst(前 13 字节) 之后原样透传 body，
	   不认识 digest，所以需要这 1 个字节让接收端知道该走哪个 handler。

	2. 标签之后的载荷与老路径**逐字节相同**。接收端拿到之后，只需 switch(tag)，
	   再把 body[1:] 包成 MemoryStream 交给原有的 handler 即可，业务解析代码零改动，
	   这是本次迁移风险最低的关键。

	3. 实体 Actor 地址只由 ENTITY_ID 决定，与宿主进程无关：
	   MailboxAddress{kind=MAILBOX_ENTITY, componentType=宿主类型提示, actorId=id_}。
	   ENTITYCALL_TYPE 只影响接收端的派发分支，不影响寻址。
	   这正是 MailBox 相对 EntityCall 的核心收益——实体迁移后地址不变。
*/

#ifndef KBE_ROUTER_MAIL_H
#define KBE_ROUTER_MAIL_H

#include "common/common.h"
#include "router_interface.h"

#include <atomic>
#include <functional>
#include <string>

namespace KBEngine{

namespace Network
{
class Bundle;
class MessageHandlers;
}

namespace RouterMail{

/** body 首字节：顶替老的 newMessage(digest)，决定接收端走哪个原有 handler */
enum BodyTag : uint8_t
{
	BODY_UNKNOWN					= 0x00,

	// 原 BaseappInterface::onEntityCall / CellappInterface::onEntityCall
	// 载荷：entityID(ENTITY_ID) + [type(ENTITYCALL_TYPE)] + 方法调用载荷
	BODY_ENTITY_CALL				= 0x01,

	// 原 BaseappInterface::onRemoteMethodCall (客户端 -> base)
	// 载荷：entityID(ENTITY_ID) + 方法调用载荷
	BODY_REMOTE_METHOD_CALL			= 0x02,

	// 原 CellappInterface::onRemoteCallCellMethodFromClient (客户端 -> cell)
	// 载荷：entityID(ENTITY_ID) + 方法调用载荷
	BODY_REMOTE_CELL_FROM_CLIENT	= 0x03,

	// 取代 Baseapp::forwardMessageToClientFromCellapp (cellapp -> baseapp -> client)
	// 载荷：entityID(ENTITY_ID) + 原封不动的客户端消息载荷
	BODY_FORWARD_TO_CLIENT			= 0x04,

	// 取代 Baseapp::forwardMessageToCellappFromCellapp (cellapp -> baseapp -> cellapp)
	// 载荷：entityID(ENTITY_ID) + 原封不动的 cellapp 消息载荷
	BODY_FORWARD_TO_CELLAPP			= 0x05,

	// 组件间裸消息(已剥离 digest 的 interface 消息)，供 Components 直连退化后使用
	// 载荷：msgId(uint32) + 原 interface 消息载荷
	BODY_COMPONENT_MESSAGE			= 0x06,

	// baseapp -> client 的 legacy interface 消息(ClientInterface::onXXX)。
	// 载荷：拍平的 Bundle 字节序列(即 [msgId][payload] 的重复出现)。
	// 客户端把 body 包成 MemoryStream 后按原有 MessageHandlers 逐条解析即可，
	// 因此客户端消息层的解析逻辑零改动(与 BODY_COMPONENT_MESSAGE 同一约定)。
	BODY_CLIENT_MESSAGE				= 0x07
};

/** 标签名称，仅用于日志/观测 */
inline const char* bodyTagName(BodyTag tag)
{
	switch(tag)
	{
	case BODY_ENTITY_CALL:				return "ENTITY_CALL";
	case BODY_REMOTE_METHOD_CALL:		return "REMOTE_METHOD_CALL";
	case BODY_REMOTE_CELL_FROM_CLIENT:	return "REMOTE_CELL_FROM_CLIENT";
	case BODY_FORWARD_TO_CLIENT:		return "FORWARD_TO_CLIENT";
	case BODY_FORWARD_TO_CELLAPP:		return "FORWARD_TO_CELLAPP";
	case BODY_COMPONENT_MESSAGE:		return "COMPONENT_MESSAGE";
	case BODY_CLIENT_MESSAGE:			return "CLIENT_MESSAGE";
	default:							return "UNKNOWN";
	}
}

/** Mailbox 类别名称，仅用于日志/观测 */
inline const char* mailboxKindName(uint8_t kind)
{
	switch(kind)
	{
	case RouterInterface::MAILBOX_ENTITY:		return "entity";
	case RouterInterface::MAILBOX_COMPONENT:	return "component";
	case RouterInterface::MAILBOX_SERVICE:		return "service";
	default:									return "unknown";
	}
}

//======================================================================
// 地址推导
//======================================================================

/** entityCall 目标实体的宿主组件类型提示(仅用于观测，不参与路由) */
inline uint32 entityHostComponentType(ENTITYCALL_TYPE type)
{
	return (uint32)entityCallType2ComponentType(type);
}

/** 实体 Actor 地址(由 ENTITYCALL_TYPE 推导宿主类型提示) */
inline RouterInterface::MailboxAddress entityMailbox(ENTITY_ID eid, ENTITYCALL_TYPE type)
{
	return RouterInterface::mailboxEntity((uint64_t)eid, entityHostComponentType(type));
}

/** 实体 Actor 地址(显式给出宿主类型提示) */
inline RouterInterface::MailboxAddress entityMailbox(ENTITY_ID eid, uint32 hostComponentTypeHint)
{
	return RouterInterface::mailboxEntity((uint64_t)eid, hostComponentTypeHint);
}

/** 本进程的组件地址(用作 Mail 的 src) */
inline RouterInterface::MailboxAddress localComponentMailbox()
{
	return RouterInterface::mailboxComponent((uint64_t)g_componentID, (uint32)g_componentType);
}

/** 本进程内的实体地址(本进程既持有实体又是该实体的宿主) */
inline RouterInterface::MailboxAddress localEntityMailbox(ENTITY_ID eid)
{
	return RouterInterface::mailboxEntity((uint64_t)eid, (uint32)g_componentType);
}

/** 某个组件的地址 */
inline RouterInterface::MailboxAddress componentMailbox(COMPONENT_ID cid, COMPONENT_TYPE ctype)
{
	return RouterInterface::mailboxComponent((uint64_t)cid, (uint32)ctype);
}

/** 业务服务地址(按服务名，注册方与调用方得到相同地址) */
inline RouterInterface::MailboxAddress serviceMailbox(const std::string& name)
{
	return RouterInterface::mailboxService(name);
}

/** 地址的可读形式，仅用于日志 */
inline std::string mailboxToString(const RouterInterface::MailboxAddress& a)
{
	return std::string(mailboxKindName(a.kind)) + ":"
		+ std::to_string((unsigned long long)a.actorId);
}

//======================================================================
// body 组装 / 解析
//======================================================================

/** 组装 body：[tag][原接口载荷...] */
inline std::string buildBody(BodyTag tag, const char* payload, size_t size)
{
	std::string body;
	body.reserve(size + 1);
	RouterInterface::Wire::putU8(body, (uint8_t)tag);

	if(payload != NULL && size > 0)
		body.append(payload, size);

	return body;
}

/** 组装 body：[tag][原接口载荷...] */
inline std::string buildBody(BodyTag tag, const std::string& legacyPayload)
{
	return buildBody(tag, legacyPayload.data(), legacyPayload.size());
}

/** 解析 body：拆出 tag 与载荷视图(不拷贝)。tag 未知或 body 为空时返回 false。 */
inline bool parseBody(const std::string& body, BodyTag& tag, const char*& payload, size_t& size)
{
	tag = BODY_UNKNOWN;
	payload = NULL;
	size = 0;

	if(body.empty())
		return false;

	tag = (BodyTag)(uint8_t)body[0];

	if(tag == BODY_UNKNOWN)
		return false;

	payload = body.data() + 1;
	size = body.size() - 1;
	return true;
}

/**
	取出 body 载荷(跳过 tag) 的 std::string 副本。
	派发侧常需要把载荷交给以 MemoryStream 为入口的原有 handler。
*/
inline bool parseBodyPayload(const std::string& body, BodyTag& tag, std::string& outPayload)
{
	const char* payload = NULL;
	size_t size = 0;

	if(!parseBody(body, tag, payload, size))
		return false;

	outPayload.assign(payload, size);
	return true;
}

//======================================================================
// cellapp -> client 的"中继外壳"剥离
//======================================================================

/**
	legacy 通路里 cellapp 并不直连客户端：所有给客户端的消息都被包进
	BaseappInterface::forwardMessageToClientFromCellapp 经 baseapp 中继，
	bundle 形如 [msgID][msgLen][entityID][客户端协议消息序列...]。

	Router 模式下客户端由 baseapp 注册为 CLIENT_TYPE Actor(见
	Baseapp::bindClientProxyActor)，cellapp 可以直接寻址它，因此投递前必须
	剥掉这层中继外壳，只把"客户端协议消息序列"作为 BODY_CLIENT_MESSAGE 的载荷
	(与 Baseapp::sendMailToClientActor / Proxy::sendToClient 同一约定)。

	下面三个尺寸对应 network/common.h 的 sizeof(MessageID) /
	sizeof(MessageLength) / sizeof(MessageLength1)。此处不引入 network 头，
	以保持 router_mail.h "只依赖 common.h + router_interface.h" 的定位；
	一致性由 cellapp.cpp 中的 static_assert 保证(漂移会编译失败)。
*/
static const size_t LEGACY_MSG_ID_SIZE		= 2;
static const size_t LEGACY_MSG_LEN_SIZE		= 2;
static const size_t LEGACY_MSG_LEN1_SIZE	= 4;

/** 变长消息改用扩展长度字段的阈值，与 NETWORK_MESSAGE_MAX_SIZE 一致 */
static const uint32_t LEGACY_MSG_LEN_MAX	= 65535;

/**
	剥掉 cellapp -> client 的中继外壳。

	@param body       拍平的 Bundle 字节序列(第一条消息必须是中继消息)
	@param clientBody 输出：客户端协议消息序列(可直接交给客户端 MessageHandlers 解析)
	@param outMsgID   可选输出：中继消息的 msgID(便于日志/断言)
	@param outEID     可选输出：中继包里携带的 entityID(便于与目标 eid 做一致性校验)
	@return 是否为合法的中继包并成功剥离(载荷为空视为失败)
*/
inline bool stripClientForwardEnvelope(const std::string& body,
	std::string& clientBody, uint16_t* outMsgID = NULL, ENTITY_ID* outEID = NULL)
{
	clientBody.clear();

	const char* d = body.data();
	const size_t n = body.size();
	size_t off = 0;

	uint16_t msgID = 0;
	uint16_t msgLen = 0;

	if(!RouterInterface::Wire::getU16(d, n, off, msgID))
		return false;

	if(!RouterInterface::Wire::getU16(d, n, off, msgLen))
		return false;

	// 与 PacketReader 一致：长度字段达到上限时，真实长度在随后的扩展字段中。
	if(msgLen >= LEGACY_MSG_LEN_MAX)
	{
		uint32_t exMsgLen = 0;
		if(!RouterInterface::Wire::getU32(d, n, off, exMsgLen))
			return false;
	}

	uint32_t forwardedEID = 0;
	if(!RouterInterface::Wire::getU32(d, n, off, forwardedEID))
		return false;

	if(outMsgID != NULL)
		*outMsgID = msgID;

	if(outEID != NULL)
		*outEID = (ENTITY_ID)forwardedEID;

	if(off < n)
		clientBody.assign(d + off, n - off);

	return !clientBody.empty();
}

//======================================================================
// msgId
//======================================================================

/**
	生成一个进程内单调递增的 msgId。
	用途：(1) 日志关联；(2) MSG_DELIVERY_FAILED 回报时把失败关联回具体的发送点。
	注意：router 不做重传/去重，msgId 不是"可靠传输序列号"，业务侧若要幂等
	请自行使用业务幂等键。
*/
inline uint64_t nextMsgId()
{
	static std::atomic<uint64_t> s_msgId(0);
	return ++s_msgId;
}

//======================================================================
// 运行模式开关
//======================================================================

/**
	Router 模式开关。

	默认**关闭**：关闭时全引擎行为与改造前完全一致(仍走 Components 直连 Channel)，
	这是迁移期最重要的安全阀——任何一个组件没准备好，都可以让它单独退回旧通路。

	由 ServerApp 在成功创建 RouterClient 后置位(见 serverapp.cpp)，
	entitycallabstract / 各 App 通过它决定"走 Router 还是走 Channel"。
	注意：这是一个**进程级稳定标志**，不要在单条消息的收发过程中改变它，
	否则同一对收发方会一边写 tag 格式、一边按 digest 解析。
*/
bool isEnabled();
void setEnabled(bool v);

//======================================================================
// Bundle -> body
//======================================================================

/**
	把一个"待发送的 Bundle"的内容按写入顺序拍平成一段连续字节，作为 Mail 的 body。

	背景：Network::Bundle 不是 MemoryStream，而是一个 Packet 列表(见 bundle.h)，
	未写满的当前包保存在 pCurrPacket_ 中、不计入 packets_，
	因此必须"packets_ + pCurrPacket_"两段都取(与 Bundle::append(Bundle&) 同一约定)。
*/
std::string bundleToBody(Network::Bundle& bundle);

//======================================================================
// body -> 原有 handler 派发
//======================================================================

/**
	派发一条"组件级裸消息"(BODY_COMPONENT_MESSAGE)。

	载荷约定与老路径**逐字节相同**：msgId(uint32) + 原 interface 消息载荷，
	也就是"拍平的 Bundle 字节序列"的前缀。因此这里可以直接用 MessageHandlers::find()
	选中原来的 handler 并以 pChannel=NULL 调用，业务解析代码零改动
	(与 cellapp 里 forwardEntityMessageToCellappFromClient 的手工派发完全同一约定)。

	注意：接收侧 handler 必须对 pChannel==NULL 安全。老路径的 handler 首行常写
	`if(pChannel->isExternal()) return;`，这类 handler 在 Router 模式下必须改为
	`if(pChannel != NULL && pChannel->isExternal()) return;`。

	@param handlers 接收方的 MessageHandlers 表(如 BaseappInterface::messageHandlers)
	@param payload   tag 之后的载荷指针(不拷贝)
	@param size      tag 之后的载荷长度
	@param logTag    日志前缀(如 "Baseapp::onRouterMail")
	@return 成功派发返回 true；载荷不完整 / msgId 未注册 / handler 抛异常时告警并返回 false
*/
bool dispatchComponentMessage(Network::MessageHandlers& handlers,
	const char* payload, size_t size, const char* logTag);

//======================================================================
// 发信钩子
//======================================================================

/**
	Router 模式下的实际投递实现，由 ServerApp 在创建 RouterClient 之后注入。
	返回 false 表示本次投递未成功(router 未就绪 / 目标不存在 / 连接断开)，
	具体原因由 RouterClient::onDeliveryFailed 暴露给业务侧，这里不重试。

	为什么钩子存储放在 RouterMail(属于 server.lib)、而不是做成
	EntityCallAbstract 的静态成员：
	后者会让 lib/entitydef 直接引用 lib/server 的符号，从而迫使**每一个**链接
	server.lib 的可执行文件都必须再链接 entitydef.lib(而它们原本都不链)。
	放在这里，依赖方向就与既有的 entitydef -> server(如 Components::getSingleton)
	完全一致，不需要改动任何 App 工程的链接行。
*/
typedef std::tr1::function<bool (const RouterInterface::MailboxAddress& dst,
	uint64_t msgId, const std::string& body)> SendMailHook;

/** 发信钩子的存储引用(定义在 router_mail.cpp) */
SendMailHook& sendMailHook();

} // RouterMail
} // KBEngine

#endif // KBE_ROUTER_MAIL_H
