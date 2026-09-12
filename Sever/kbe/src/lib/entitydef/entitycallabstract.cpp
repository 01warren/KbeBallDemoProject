// Copyright 2008-2018 Yolo Technologies, Inc. All Rights Reserved. https://www.comblockengine.com


#include "entitycallabstract.h"
#include "pyscript/pickler.h"
#include "helper/debug_helper.h"
#include "network/packet.h"
#include "network/bundle.h"
#include "network/network_interface.h"
#include "server/components.h"
#include "entitydef/entitydef.h"
#include "client_lib/client_interface.h"

#include "../../server/baseapp/baseapp_interface.h"
#include "../../server/cellapp/cellapp_interface.h"

#ifndef CODE_INLINE
#include "entitycallabstract.inl"
#endif

namespace KBEngine{
EntityCallAbstract::EntityCallCallHookFunc*	EntityCallAbstract::__hookCallFuncPtr = NULL;
EntityCallAbstract::FindChannelFunc EntityCallAbstract::__findChannelFunc;

SCRIPT_METHOD_DECLARE_BEGIN(EntityCallAbstract)
SCRIPT_METHOD_DECLARE("__reduce_ex__",				reduce_ex__,			METH_VARARGS,		0)
SCRIPT_METHOD_DECLARE_END()

SCRIPT_MEMBER_DECLARE_BEGIN(EntityCallAbstract)
SCRIPT_MEMBER_DECLARE_END()

SCRIPT_GETSET_DECLARE_BEGIN(EntityCallAbstract)
SCRIPT_GET_DECLARE("id",							pyGetID,				0,					0)	
SCRIPT_GETSET_DECLARE_END()
SCRIPT_INIT(EntityCallAbstract, 0, 0, 0, 0, 0)		

//-------------------------------------------------------------------------------------
EntityCallAbstract::EntityCallAbstract(PyTypeObject* scriptType, 
											const Network::Address* pAddr, 
											COMPONENT_ID componentID, 
											ENTITY_ID eid, 
											uint16 utype, 
											ENTITYCALL_TYPE type):
ScriptObject(scriptType, false),
componentID_(componentID),
addr_((pAddr == NULL) ? Network::Address::NONE : *pAddr),
type_(type),
id_(eid),
utype_(utype)
{
}

//-------------------------------------------------------------------------------------
EntityCallAbstract::~EntityCallAbstract()
{
}

//-------------------------------------------------------------------------------------
void EntityCallAbstract::newCall(Network::Bundle& bundle)
{
	newCall_(bundle);
}

//-------------------------------------------------------------------------------------
void EntityCallAbstract::newCall_(Network::Bundle& bundle)
{
	// 如果是server端的entityCall
	const bool isServerSide = (g_componentType != CLIENT_TYPE && g_componentType != BOTS_TYPE);

	// ---- Router 模式 ----
	// body = [tag][id_][type_][方法载荷]，除首字节外与老路径逐字节一致：
	// 老路径用 bundle.newMessage(digest) 写消息头、接收端靠 digest 选 handler；
	// 而 router 只看 dst、原样透传 body，所以改用 1 字节 tag 自描述
	// (见 server/router_mail.h 的 BodyTag 说明)。
	if(RouterMail::isEnabled())
	{
		if(isServerSide)
		{
			// componentID_ == 0 表示目标是一个客户端，否则是服务端实体
			// (具体是 baseapp 还是 cellapp 由接收端按 type_ 决定)
			bundle << (uint8)((componentID_ == 0)
				? RouterMail::BODY_REMOTE_METHOD_CALL
				: RouterMail::BODY_ENTITY_CALL);
		}
		else
		{
			// 如果是客户端上的entityCall调用服务端方法只存在调用cell或者base
			switch(type_)
			{
			case ENTITYCALL_TYPE_BASE:
				bundle << (uint8)RouterMail::BODY_REMOTE_METHOD_CALL;
				break;
			case ENTITYCALL_TYPE_CELL:
				bundle << (uint8)RouterMail::BODY_REMOTE_CELL_FROM_CLIENT;
				break;
			default:
				KBE_ASSERT(false && "no support!\n");
				bundle << (uint8)RouterMail::BODY_UNKNOWN;
				break;
			};
		}

		bundle << id_;

		// 如果是发往客户端的包则无需附加这样一个类型
		if(isServerSide && componentID_ > 0)
			bundle << type_;

		return;
	}

	// ---- 原有路径(Components 直连 Channel) ----
	if(isServerSide)
	{
		// 如果ID为0，则这是一个客户端组件，否则为服务端。
		if(componentID_ == 0)
		{
			bundle.newMessage(ClientInterface::onRemoteMethodCall);
		}
		else
		{
			Components::ComponentInfos* cinfos = Components::getSingleton().findComponent(componentID_);

			if(cinfos != NULL)
			{
				// 找到对应的组件投递过去， 如果这个entityCall还需要中转比如 e.base.cell ， 则由baseapp转往cellapp
				if(cinfos->componentType == BASEAPP_TYPE)
				{
					bundle.newMessage(BaseappInterface::onEntityCall);
				}
				else
				{
					bundle.newMessage(CellappInterface::onEntityCall);
				}
			}
			else
			{
				ERROR_MSG(fmt::format("EntityCallAbstract::newCall_: not found component({}), entityID({})!\n",
					componentID_, id_));
			}
		}

		bundle << id_;
		
		// 如果是发往客户端的包则无需附加这样一个类型
		if(componentID_ > 0)
			bundle << type_;
	}
	else
	{
		// 如果是客户端上的entityCall调用服务端方法只存在调用cell或者base
		switch(type_)
		{
		case ENTITYCALL_TYPE_BASE:
			bundle.newMessage(BaseappInterface::onRemoteMethodCall);
			break;
		case ENTITYCALL_TYPE_CELL:
			bundle.newMessage(BaseappInterface::onRemoteCallCellMethodFromClient);
			break;
		default:
			KBE_ASSERT(false && "no support!\n");
			break;
		};

		bundle << id_;
	}
}

//-------------------------------------------------------------------------------------
bool EntityCallAbstract::sendCall(Network::Bundle* pBundle)
{
	// ---- Router 模式 ----
	// 整个 bundle 就是 body(首字节是 newCall_ 写入的 BodyTag)，拍平后交给 router 投递。
	// 注意：这里不能走 getChannel()，否则会把 tag 格式的 body 当成 digest 格式
	// 直接从旧 channel 发出去，接收端会解析错乱。
	if(RouterMail::isEnabled())
	{
		std::string body = RouterMail::bundleToBody(*pBundle);

		// body 已拷出，bundle 可以立刻归还对象池
		Network::Bundle::reclaimPoolObject(pBundle);

		// 钩子的存储放在 RouterMail(server.lib)里而不是本类的静态成员：
		// 否则 lib/entitydef 会引用 lib/server 的符号，迫使所有链接 server.lib
		// 的可执行文件都必须再链接 entitydef.lib。见 router_mail.h 说明。
		RouterMail::SendMailHook& hook = RouterMail::sendMailHook();

		if(hook == NULL)
		{
			ERROR_MSG(fmt::format("EntityCallAbstract::sendCall: router mode enabled but sendMail hook "
				"is not installed, entityID({})!\n", id_));

			return false;
		}

		// RouterClient::sendMail 是同步入队，不重试；
		// 失败原因由 RouterClient::onDeliveryFailed 回调暴露给业务侧。
		if(hook(mailboxAddress(), RouterMail::nextMsgId(), body))
			return true;

		ERROR_MSG(fmt::format("EntityCallAbstract::sendCall: router deliver failed, entityID({}), bodySize({})!\n",
			id_, body.size()));

		return false;
	}

	KBE_ASSERT(Components::getSingleton().pNetworkInterface() != NULL);
	Network::Channel* pChannel = getChannel();

	if(pChannel && !pChannel->isDestroyed())
	{
		pChannel->send(pBundle);
		return true;
	}
	else
	{
		ERROR_MSG(fmt::format("EntityCallAbstract::sendCall: invalid channel({}), entityID({})!\n",
			addr_.c_str(), id_));
	}

	Network::Bundle::reclaimPoolObject(pBundle);
	return false;
}

//-------------------------------------------------------------------------------------
PyObject* EntityCallAbstract::__py_reduce_ex__(PyObject* self, PyObject* protocol)
{
	EntityCallAbstract* eentitycall = static_cast<EntityCallAbstract*>(self);
	
	PyObject* args = PyTuple_New(2);
	PyObject* unpickleMethod = script::Pickler::getUnpickleFunc("EntityCall");
	PyTuple_SET_ITEM(args, 0, unpickleMethod);
	
	PyObject* args1 = PyTuple_New(4);
	PyTuple_SET_ITEM(args1, 0, PyLong_FromLong(eentitycall->id()));
	PyTuple_SET_ITEM(args1, 1, PyLong_FromUnsignedLongLong(eentitycall->componentID()));
	PyTuple_SET_ITEM(args1, 2, PyLong_FromUnsignedLong(eentitycall->utype()));

	int16 mbType = static_cast<int16>(eentitycall->type());
	
	PyTuple_SET_ITEM(args1, 3, PyLong_FromLong(mbType));
	PyTuple_SET_ITEM(args, 1, args1);

	if(unpickleMethod == NULL){
		Py_DECREF(args);
		return NULL;
	}
	return args;
}

//-------------------------------------------------------------------------------------
PyObject* EntityCallAbstract::pyGetID()
{ 
	return PyLong_FromLong(id()); 
}

//-------------------------------------------------------------------------------------
RouterInterface::MailboxAddress EntityCallAbstract::mailboxAddress() const
{
	// 只由 id_ 决定寻址；type_ 仅作为 componentType 观测提示(见头文件说明)
	return RouterMail::entityMailbox(id_, type_);
}

//-------------------------------------------------------------------------------------
Network::Channel* EntityCallAbstract::getChannel(void)
{
	// Router 模式下不存在"实体到实体的直连频道"，所有投递都经 router。
	// 返回 NULL 有两个作用：
	//   1. 调用方退化到 sendCall 兜底分支(RouterClient::sendMail)；
	//   2. 阻止 RemoteEntityMethod / Cellapp::onEntityCall 等调用点
	//      绕过 sendCall 直接往旧 channel 发包。
	// 依赖 getChannel() 做 RTT / lastReceivedTime 观测的调用方会退化为"不可用"，
	// 这是 router 集中转发后的正常语义(连接对象已不存在于实体维度)。
	if(RouterMail::isEnabled())
		return NULL;

	if (__findChannelFunc == NULL)
		return NULL;

	return __findChannelFunc(*this);
}

//-------------------------------------------------------------------------------------
ScriptDefModule* EntityCallAbstract::pScriptDefModule()
{
	return EntityDef::findScriptModule(utype_);
}

//-------------------------------------------------------------------------------------

}
