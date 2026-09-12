// Copyright 2008-2018 Yolo Technologies, Inc. All Rights Reserved. https://www.comblockengine.com

/*
	router_mail.cpp —— router_mail.h 中需要"与 Bundle 打交道"的部分实现。

	之所以单独一个编译单元：router_mail.h 被 entitydef 与各 App 共同包含，
	不希望把 network/bundle.h 这种重头文件拖进所有包含方。
*/

#include "server/router_mail.h"
#include "network/bundle.h"
#include "network/packet.h"
#include "network/message_handler.h"
#include "common/memorystream.h"

#include <atomic>

namespace KBEngine{
namespace RouterMail{

//-------------------------------------------------------------------------------------
static std::atomic<bool>& enabledFlag()
{
	static std::atomic<bool> s_enabled(false);
	return s_enabled;
}

//-------------------------------------------------------------------------------------
bool isEnabled()
{
	return enabledFlag().load();
}

//-------------------------------------------------------------------------------------
void setEnabled(bool v)
{
	enabledFlag().store(v);
}

//-------------------------------------------------------------------------------------
SendMailHook& sendMailHook()
{
	static SendMailHook s_hook;
	return s_hook;
}

//-------------------------------------------------------------------------------------
std::string bundleToBody(Network::Bundle& bundle)
{
	std::string body;

	Network::Bundle::Packets& packets = bundle.packets();
	Network::Bundle::Packets::iterator iter = packets.begin();

	for(; iter != packets.end(); ++iter)
	{
		Network::Packet* pPacket = (*iter);

		if(pPacket == NULL)
			continue;

		int32 len = (int32)pPacket->length();

		if(len > 0)
			body.append((const char*)pPacket->data() + pPacket->rpos(), (size_t)len);
	}

	// 未写满的当前包不计入 packets_，必须单独取
	// (与 Bundle::append(Bundle& bundle) 完全同一约定)
	Network::Packet* pCurrPacket = bundle.pCurrPacket();

	if(pCurrPacket != NULL)
	{
		int32 len = (int32)pCurrPacket->length();

		if(len > 0)
			body.append((const char*)pCurrPacket->data() + pCurrPacket->rpos(), (size_t)len);
	}

	return body;
}

//-------------------------------------------------------------------------------------
bool dispatchComponentMessage(Network::MessageHandlers& handlers,
	const char* payload, size_t size, const char* logTag)
{
	const char* tag = (logTag != NULL) ? logTag : "RouterMail::dispatchComponentMessage";

	if(payload == NULL || size < sizeof(Network::MessageID))
	{
		WARNING_MSG(fmt::format("{}: component message payload too short, size={}.\n",
			tag, (int)size));

		return false;
	}

	MemoryStream* s = MemoryStream::createPoolObject(OBJECTPOOL_POINT);
	s->append(payload, (int)size);

	Network::MessageID msgId = 0;
	(*s) >> msgId;

	Network::MessageHandler* pMsgHandler = handlers.find(msgId);
	if(pMsgHandler == NULL)
	{
		WARNING_MSG(fmt::format("{}: not found handler for msgID={}.\n", tag, msgId));

		MemoryStream::reclaimPoolObject(s);
		return false;
	}

	Network::MessageLength msgLen;

	if(pMsgHandler->msgLen == NETWORK_VARIABLE_MESSAGE)
		(*s) >> msgLen;
	else
		msgLen = pMsgHandler->msgLen;

	if(s->length() < msgLen || msgLen > NETWORK_MESSAGE_MAX_SIZE / 2)
	{
		WARNING_MSG(fmt::format("{}: msgID={} invalid msglen={}, payload size={}.\n",
			tag, msgId, msgLen, (int)size));

		MemoryStream::reclaimPoolObject(s);
		return false;
	}

	// 临时设置有效读取位，防止 handler 内部越界读取(与老路径手工派发同一约定)
	size_t wpos = s->wpos();
	size_t frpos = s->rpos() + msgLen;
	s->wpos((int)frpos);

	bool ok = true;

	try
	{
		// pChannel=NULL：Router 模式下没有 Channel 的概念，等价于老路径的"内部来源"。
		pMsgHandler->handle(NULL, *s);
	}
	catch(MemoryStreamException&)
	{
		ERROR_MSG(fmt::format("{}: message({}) error.\n", tag, pMsgHandler->name));

		ok = false;
	}

	// 防止 handler 中没有把数据读完导致后续解析错位
	if(ok && msgLen > 0 && frpos != s->rpos())
	{
		CRITICAL_MSG(fmt::format("{}: rpos({}) invalid, expect={}. msgID={}, msglen={}.\n",
			tag, s->rpos(), frpos, msgId, msgLen));

		s->rpos((int)frpos);
	}

	s->wpos((int)wpos);
	MemoryStream::reclaimPoolObject(s);

	return ok;
}

} // RouterMail
} // KBEngine
