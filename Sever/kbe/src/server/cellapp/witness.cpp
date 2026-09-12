// Copyright 2008-2018 Yolo Technologies, Inc. All Rights Reserved. https://www.comblockengine.com

#include "witness.h"
#include "entity.h"	
#include "profile.h"
#include "cellapp.h"
#include "view_trigger.h"
#include "network/channel.h"	
#include "network/bundle.h"
#include "network/network_stats.h"
#include "math/math.h"

#include "server/router_mail.h"
#include "client_lib/client_interface.h"

#include "../../server/baseapp/baseapp_interface.h"

#ifndef CODE_INLINE
#include "witness.inl"
#endif

#define UPDATE_FLAG_NULL				0x00000000
#define UPDATE_FLAG_XZ					0x00000001
#define UPDATE_FLAG_XYZ					0x00000002
#define UPDATE_FLAG_YAW					0x00000004
#define UPDATE_FLAG_ROLL				0x00000008
#define UPDATE_FLAG_PITCH				0x00000010
#define UPDATE_FLAG_YAW_PITCH_ROLL		0x00000020
#define UPDATE_FLAG_YAW_PITCH			0x00000040
#define UPDATE_FLAG_YAW_ROLL			0x00000080
#define UPDATE_FLAG_PITCH_ROLL			0x00000100
#define UPDATE_FLAG_ONGOUND				0x00000200

namespace KBEngine{	


//-------------------------------------------------------------------------------------
Witness::Witness():
pEntity_(NULL),
viewRadius_(0.0f),
viewHysteresisArea_(5.0f),
pViewTrigger_(NULL),
pViewHysteresisAreaTrigger_(NULL),
viewEntities_(),
viewEntities_map_(),
clientViewSize_(0)
{
	updatableName = "Witness";
}

//-------------------------------------------------------------------------------------
Witness::~Witness()
{
	pEntity_ = NULL;
	SAFE_RELEASE(pViewTrigger_);
	SAFE_RELEASE(pViewHysteresisAreaTrigger_);
}

//-------------------------------------------------------------------------------------
void Witness::addToStream(KBEngine::MemoryStream& s)
{
	/**
	 * @TODO(phw): ???????????????????????¶Ã?????
	 * ??????°Í?A??B??C????????????????????????????viewEntities_??????????????????entityID??
	 * ??????????????????????????????cellapp?????????????
	 * ????????????????????????????????????flags_ == ENTITYREF_FLAG_UNKONWN??EntityRef?????
	 * ???????????????viewEntities_??
	 * ?????Witness::update()????????flags_ == ENTITYREF_FLAG_UNKONWN?????????????????????entity??????????????
	 * ?????????????updateVolatileData()?????
	 * ???????????ß“?????entity?????????????????????????¶Ã?????????????????????
	
	s << viewRadius_ << viewHysteresisArea_ << clientViewSize_;	
	
	uint32 size = viewEntitiesmap_.size();
	s << size;

	EntityRef::VIEW_ENTITIES::iterator iter = viewEntities_.begin();
	for(; iter != viewEntities_.end(); ++iter)
	{
		(*iter)->addToStream(s);
	}
	*/

	// ??????????????????????space??cell???????????????????
	s << viewRadius_ << viewHysteresisArea_ << (uint16)0;	
	s << (uint32)0; // viewEntities_map_.size();
}

//-------------------------------------------------------------------------------------
void Witness::createFromStream(KBEngine::MemoryStream& s)
{
	s >> viewRadius_ >> viewHysteresisArea_ >> clientViewSize_;

	uint32 size;
	s >> size;
	
	for(uint32 i=0; i<size; ++i)
	{
		EntityRef* pEntityRef = EntityRef::createPoolObject(OBJECTPOOL_POINT);
		pEntityRef->createFromStream(s);
		viewEntities_.push_back(pEntityRef);
		viewEntities_map_[pEntityRef->id()] = pEntityRef;
		pEntityRef->aliasID(i);
	}

	setViewRadius(viewRadius_, viewHysteresisArea_);

	lastBasePos_.z = -FLT_MAX;
	lastBaseDir_.yaw(-FLT_MAX);
	Cellapp::getSingleton().addUpdatable(this);
}

//-------------------------------------------------------------------------------------
void Witness::attach(Entity* pEntity)
{
	//DEBUG_MSG(fmt::format("Witness::attach: {}({}).\n", 
	//	pEntity->scriptName(), pEntity->id()));

	pEntity_ = pEntity;

	lastBasePos_.z = -FLT_MAX;
	lastBaseDir_.yaw(-FLT_MAX);

	if(g_kbeSrvConfig.getCellApp().use_coordinate_system)
	{
		// ????????View??¶∂
		ENGINE_COMPONENT_INFO& ecinfo = ServerConfig::getSingleton().getCellApp();
		setViewRadius(ecinfo.defaultViewRadius, ecinfo.defaultViewHysteresisArea);
	}

	Cellapp::getSingleton().addUpdatable(this);

	onAttach(pEntity);
}

//-------------------------------------------------------------------------------------
void Witness::onAttach(Entity* pEntity)
{
	lastBasePos_.z = -FLT_MAX;
	lastBaseDir_.yaw(-FLT_MAX);

	// ???????enterworld
	Network::Bundle* pSendBundle = Network::Bundle::createPoolObject(OBJECTPOOL_POINT);
	NETWORK_ENTITY_MESSAGE_FORWARD_CLIENT_BEGIN(pEntity_->id(), (*pSendBundle));
	
	ENTITY_MESSAGE_FORWARD_CLIENT_BEGIN(pSendBundle, ClientInterface::onUpdatePropertys, updatePropertys);
	MemoryStream* s1 = MemoryStream::createPoolObject(OBJECTPOOL_POINT);
	(*pSendBundle) << pEntity_->id();
	pEntity_->addPositionAndDirectionToStream(*s1, true);
	(*pSendBundle).append(*s1);
	MemoryStream::reclaimPoolObject(s1);
	ENTITY_MESSAGE_FORWARD_CLIENT_END(pSendBundle, ClientInterface::onUpdatePropertys, updatePropertys);
	
	ENTITY_MESSAGE_FORWARD_CLIENT_BEGIN(pSendBundle, ClientInterface::onEntityEnterWorld, entityEnterWorld);

	(*pSendBundle) << pEntity_->id();
	pEntity_->pScriptModule()->addSmartUTypeToBundle(pSendBundle);
	if(!pEntity_->isOnGround())
		(*pSendBundle) << pEntity_->isOnGround();

	ENTITY_MESSAGE_FORWARD_CLIENT_END(pSendBundle, ClientInterface::onEntityEnterWorld, entityEnterWorld);

	// Router mode: deliver to the client Actor directly, otherwise fall back
	// to the legacy sendCall (Router disabled).
	if(!Cellapp::getSingleton().sendBundleToClientActor(pEntity_->id(), pSendBundle))
		pEntity_->clientEntityCall()->sendCall(pSendBundle);
}

//-------------------------------------------------------------------------------------
void Witness::detach(Entity* pEntity)
{
	//DEBUG_MSG(fmt::format("Witness::detach: {}({}).\n", 
	//	pEntity->scriptName(), pEntity->id()));

	EntityCall* pClientMB = pEntity_->clientEntityCall();
	if(pClientMB)
	{
		Network::Channel* pChannel = pClientMB->getChannel();

		// Router mode: pChannel is always NULL here, but the leaveworld
		// notification still has to be delivered through the router.
		if(RouterMail::isEnabled() || pChannel)
		{
			if(pChannel)
				pChannel->send();

			// ???????leaveworld
			Network::Bundle* pSendBundle = Network::Bundle::createPoolObject(OBJECTPOOL_POINT);
			NETWORK_ENTITY_MESSAGE_FORWARD_CLIENT_BEGIN(pEntity_->id(), (*pSendBundle));

			ENTITY_MESSAGE_FORWARD_CLIENT_BEGIN(pSendBundle, ClientInterface::onEntityLeaveWorld, entityLeaveWorld);
			(*pSendBundle) << pEntity->id();
			ENTITY_MESSAGE_FORWARD_CLIENT_END(pSendBundle, ClientInterface::onEntityLeaveWorld, entityLeaveWorld);

			if(!Cellapp::getSingleton().sendBundleToClientActor(pEntity_->id(), pSendBundle))
				pClientMB->sendCall(pSendBundle);
		}
	}

	clear(pEntity);
}

//-------------------------------------------------------------------------------------
void Witness::clear(Entity* pEntity)
{
	KBE_ASSERT(pEntity == pEntity_);
	uninstallViewTrigger();

	VIEW_ENTITIES::iterator iter = viewEntities_.begin();
	for(; iter != viewEntities_.end(); ++iter)
	{
		if((*iter)->pEntity())
		{
			(*iter)->pEntity()->delWitnessed(pEntity_);
		}
		
		EntityRef::reclaimPoolObject((*iter));
	}
	
	pEntity_ = NULL;
	viewRadius_ = 0.0f;
	viewHysteresisArea_ = 5.0f;
	clientViewSize_ = 0;

	// ?????????????‰Ô????????
	// ?????????????????????enterview?????ß·?????????????
	// ??pViewTrigger_?????????????????????pViewTrigger_??crash
	//SAFE_RELEASE(pViewTrigger_);
	//SAFE_RELEASE(pViewHysteresisAreaTrigger_);

	viewEntities_.clear();
	viewEntities_map_.clear();

	Cellapp::getSingleton().removeUpdatable(this);
}

//-------------------------------------------------------------------------------------
static ObjectPool<Witness> _g_objPool("Witness");
ObjectPool<Witness>& Witness::ObjPool()
{
	return _g_objPool;
}

//-------------------------------------------------------------------------------------
Witness* Witness::createPoolObject(const std::string& logPoint)
{
	return _g_objPool.createObject(logPoint);
}

//-------------------------------------------------------------------------------------
void Witness::reclaimPoolObject(Witness* obj)
{
	_g_objPool.reclaimObject(obj);
}

//-------------------------------------------------------------------------------------
void Witness::destroyObjPool()
{
	DEBUG_MSG(fmt::format("Witness::destroyObjPool(): size {}.\n",
		_g_objPool.size()));

	_g_objPool.destroy();
}

//-------------------------------------------------------------------------------------
Witness::SmartPoolObjectPtr Witness::createSmartPoolObj(const std::string& logPoint)
{
	return SmartPoolObjectPtr(new SmartPoolObject<Witness>(ObjPool().createObject(logPoint), _g_objPool));
}

//-------------------------------------------------------------------------------------
void Witness::onReclaimObject()
{
}

//-------------------------------------------------------------------------------------
const Position3D& Witness::basePos()
{
	return pEntity()->position();
}

//-------------------------------------------------------------------------------------
const Direction3D& Witness::baseDir()
{
	return pEntity()->direction();
}

//-------------------------------------------------------------------------------------
void Witness::setViewRadius(float radius, float hyst)
{
	if(!g_kbeSrvConfig.getCellApp().use_coordinate_system)
		return;

	viewRadius_ = radius;
	viewHysteresisArea_ = hyst;

	// ????¶À?????????????¶À????????????°¬?¶∂?-512~512???????????¶∂?????????????
	// ???????????????????????????????????????????¶À??????????????®¢?
	// ?¶œ?: MemoryStream::appendPackXZ
	if(viewRadius_ + viewHysteresisArea_ > 512)
	{
		if (g_kbeSrvConfig.getCellApp().entity_posdir_updates_type > 0)
		{
			viewRadius_ = 512 - 5.0f;
			viewHysteresisArea_ = 5.0f;

			ERROR_MSG(fmt::format("Witness::setViewRadius({}): viewRadius({}) cannot be greater than 512! Beyond 512, please set kbengine[_defaults].xml->entity_posdir_updates->type to 0.\n",
				(pEntity_ ? pEntity_->id() : 0), (viewRadius_ + viewHysteresisArea_)));

			// ?????????????ßπ
			// return;
		}
	}

	if (viewRadius_ > 0.f && pEntity_)
	{
		if (pViewTrigger_ == NULL)
		{
			pViewTrigger_ = new ViewTrigger((CoordinateNode*)pEntity_->pEntityCoordinateNode(), viewRadius_, viewRadius_);

			// ???????????????ßµ??????????
			if (((CoordinateNode*)pEntity_->pEntityCoordinateNode())->pCoordinateSystem())
				pViewTrigger_->install();
		}
		else
		{
			pViewTrigger_->update(viewRadius_, viewRadius_);

			// ???????????????ßµ??????????
			if (!pViewTrigger_->isInstalled() && ((CoordinateNode*)pEntity_->pEntityCoordinateNode())->pCoordinateSystem())
				pViewTrigger_->reinstall((CoordinateNode*)pEntity_->pEntityCoordinateNode());
		}

		if (viewHysteresisArea_ > 0.01f && pEntity_/*????update?????????????? */)
		{
			if (pViewHysteresisAreaTrigger_ == NULL)
			{
				pViewHysteresisAreaTrigger_ = new ViewTrigger((CoordinateNode*)pEntity_->pEntityCoordinateNode(),
					viewHysteresisArea_ + viewRadius_, viewHysteresisArea_ + viewRadius_);

				if (((CoordinateNode*)pEntity_->pEntityCoordinateNode())->pCoordinateSystem())
					pViewHysteresisAreaTrigger_->install();
			}
			else
			{
				pViewHysteresisAreaTrigger_->update(viewHysteresisArea_ + viewRadius_, viewHysteresisArea_ + viewRadius_);

				// ???????????????ßµ??????????
				if (!pViewHysteresisAreaTrigger_->isInstalled() && ((CoordinateNode*)pEntity_->pEntityCoordinateNode())->pCoordinateSystem())
					pViewHysteresisAreaTrigger_->reinstall((CoordinateNode*)pEntity_->pEntityCoordinateNode());
			}
		}
		else
		{
			// ???????????????pViewHysteresisAreaTrigger_???????update
			// ?????View???ßÿ????pViewHysteresisAreaTrigger_???????????????pViewHysteresisAreaTrigger_?????View
			if (pViewHysteresisAreaTrigger_)
				pViewHysteresisAreaTrigger_->update(viewHysteresisArea_ + viewRadius_, viewHysteresisArea_ + viewRadius_);
		}
	}
	else
	{
		uninstallViewTrigger();
	}
}

//-------------------------------------------------------------------------------------
void Witness::onEnterView(ViewTrigger* pViewTrigger, Entity* pEntity)
{
	// ??????????Hysteresis?????????????????
	 if (pViewHysteresisAreaTrigger_ == pViewTrigger)
		return;

	// ??????????????????????????ß“????????????ßÿ????
	Py_INCREF(pEntity);

	// ??onEnteredview??addWitnessed?????????????????
	// pEntity_?????????NULL????????ß›???DECREF
	Entity* pSelfEntity = pEntity_;
	Py_INCREF(pSelfEntity);

	VIEW_ENTITIES_MAP::iterator iter = viewEntities_map_.find(pEntity->id());
	if (iter != viewEntities_map_.end())
	{
		EntityRef* pEntityRef = iter->second;
		if ((pEntityRef->flags() & ENTITYREF_FLAG_LEAVE_CLIENT_PENDING) > 0)
		{
			//DEBUG_MSG(fmt::format("Witness::onEnterView: {} entity={}\n", 
			//	pEntity_->id(), pEntity->id()));

			// ???flags??ENTITYREF_FLAG_LEAVE_CLIENT_PENDING | ENTITYREF_FLAG_NORMAL?????????
			// ?????????????????Åß???ENTITYREF_FLAG_NORMAL????
			// ?????ENTITYREF_FLAG_LEAVE_CLIENT_PENDING??????????y?????????????? ENTITYREF_FLAG_ENTER_CLIENT_PENDING
			if ((pEntityRef->flags() & ENTITYREF_FLAG_NORMAL) > 0)
			{
				EntityCall* pClientMB = pEntity_->clientEntityCall();
				if (pClientMB)
				{
					Network::Bundle* pSendBundle = Network::Bundle::createPoolObject(OBJECTPOOL_POINT);
					NETWORK_ENTITY_MESSAGE_FORWARD_CLIENT_BEGIN(pEntity_->id(), (*pSendBundle));
					ENTITY_MESSAGE_FORWARD_CLIENT_BEGIN(pSendBundle, ClientInterface::onEntityLeaveWorldOptimized, leaveWorld);
					_addViewEntityIDToBundle(pSendBundle, pEntityRef);
					ENTITY_MESSAGE_FORWARD_CLIENT_END(pSendBundle, ClientInterface::onEntityLeaveWorldOptimized, leaveWorld);

					if(!Cellapp::getSingleton().sendBundleToClientActor(pEntity_->id(), pSendBundle))
						pClientMB->sendCall(pSendBundle);

					KBE_ASSERT(clientViewSize_ > 0);
					--clientViewSize_;

					VIEW_ENTITIES::iterator iter1 = viewEntities_.begin();
					for (; iter1 != viewEntities_.end(); iter1++)
					{
						if ((*iter1)->id() == pEntityRef->id())
						{
							viewEntities_.erase(iter1);
							break;
						}
					}

					viewEntities_.push_back(pEntityRef);
					updateEntitiesAliasID();
				}
			}

			pEntityRef->flags(ENTITYREF_FLAG_ENTER_CLIENT_PENDING);
			pEntityRef->pEntity(pEntity);
			pEntity->addWitnessed(pEntity_);
			pSelfEntity->onEnteredView(pEntity);
		}

		Py_DECREF(pEntity);
		Py_DECREF(pSelfEntity);
		return;
	}

	//DEBUG_MSG(fmt::format("Witness::onEnterView: {} entity={}\n", 
	//	pEntity_->id(), pEntity->id()));
	
	EntityRef* pEntityRef = EntityRef::createPoolObject(OBJECTPOOL_POINT);
	pEntityRef->pEntity(pEntity);
	pEntityRef->flags(pEntityRef->flags() | ENTITYREF_FLAG_ENTER_CLIENT_PENDING);
	viewEntities_.push_back(pEntityRef);
	viewEntities_map_[pEntityRef->id()] = pEntityRef;
	pEntityRef->aliasID(viewEntities_map_.size() - 1);
	
	pEntity->addWitnessed(pEntity_);
	pSelfEntity->onEnteredView(pEntity);

	Py_DECREF(pEntity);
	Py_DECREF(pSelfEntity);
}

//-------------------------------------------------------------------------------------
void Witness::onLeaveView(ViewTrigger* pViewTrigger, Entity* pEntity)
{
	// ??????®¥?Hysteresis?????????Hysteresis?????????View
	if (pViewHysteresisAreaTrigger_ && pViewHysteresisAreaTrigger_ != pViewTrigger)
		return;

	VIEW_ENTITIES_MAP::iterator iter = viewEntities_map_.find(pEntity->id());
	if (iter == viewEntities_map_.end())
		return;

	_onLeaveView(iter->second);
}

//-------------------------------------------------------------------------------------
void Witness::_onLeaveView(EntityRef* pEntityRef)
{
	//DEBUG_MSG(fmt::format("Witness::onLeaveView: {} entity={}\n", 
	//	pEntity_->id(), pEntityRef->id()));

	// ????delete?? ?????????update????????????????????????
	//EntityRef::reclaimPoolObject((*iter));
	//viewEntities_.erase(iter);
	//viewEntities_map_.erase(iter);

	pEntityRef->flags(((pEntityRef->flags() | ENTITYREF_FLAG_LEAVE_CLIENT_PENDING) & ~(ENTITYREF_FLAG_ENTER_CLIENT_PENDING)));

	if(pEntityRef->pEntity())
		pEntityRef->pEntity()->delWitnessed(pEntity_);

	pEntityRef->pEntity(NULL);
}

//-------------------------------------------------------------------------------------
void Witness::resetViewEntities()
{
	clientViewSize_ = 0;
	VIEW_ENTITIES::iterator iter = viewEntities_.begin();
	for(; iter != viewEntities_.end(); )
	{
		if(((*iter)->flags() & ENTITYREF_FLAG_LEAVE_CLIENT_PENDING) > 0)
		{
			viewEntities_map_.erase((*iter)->id());
			EntityRef::reclaimPoolObject((*iter));
			iter = viewEntities_.erase(iter);
			continue;
		}

		(*iter)->flags(ENTITYREF_FLAG_ENTER_CLIENT_PENDING);
		++iter;
	}
	
	updateEntitiesAliasID();
}

//-------------------------------------------------------------------------------------
void Witness::onEnterSpace(SpaceMemory* pSpace)
{
	Network::Bundle* pSendBundle = Network::Bundle::createPoolObject(OBJECTPOOL_POINT);
	NETWORK_ENTITY_MESSAGE_FORWARD_CLIENT_BEGIN(pEntity_->id(), (*pSendBundle));

	// ??¶À???????
	Position3D &pos = pEntity_->position();
	Direction3D &dir = pEntity_->direction();
	ENTITY_MESSAGE_FORWARD_CLIENT_BEGIN(pSendBundle, ClientInterface::onSetEntityPosAndDir, setEntityPosAndDir);
	(*pSendBundle) << pEntity_->id();
	(*pSendBundle) << pos.x << pos.y << pos.z;
	(*pSendBundle) << dir.roll() << dir.pitch() << dir.yaw();
	ENTITY_MESSAGE_FORWARD_CLIENT_END(pSendBundle, ClientInterface::onSetEntityPosAndDir, setEntityPosAndDir);
	
	// ?????????¶Ã??
	ENTITY_MESSAGE_FORWARD_CLIENT_BEGIN(pSendBundle, ClientInterface::onEntityEnterSpace, entityEnterSpace);

	(*pSendBundle) << pEntity_->id();
	(*pSendBundle) << pSpace->id();
	if(!pEntity_->isOnGround())
		(*pSendBundle) << pEntity_->isOnGround();

	ENTITY_MESSAGE_FORWARD_CLIENT_END(pSendBundle, ClientInterface::onEntityEnterSpace, entityEnterSpace);

	// ?????????????
	if(!Cellapp::getSingleton().sendBundleToClientActor(pEntity_->id(), pSendBundle))
		pEntity_->clientEntityCall()->sendCall(pSendBundle);

	installViewTrigger();
}

//-------------------------------------------------------------------------------------
void Witness::onLeaveSpace(SpaceMemory* pSpace)
{
	uninstallViewTrigger();

	Network::Bundle* pSendBundle = Network::Bundle::createPoolObject(OBJECTPOOL_POINT);
	NETWORK_ENTITY_MESSAGE_FORWARD_CLIENT_BEGIN(pEntity_->id(), (*pSendBundle));

	ENTITY_MESSAGE_FORWARD_CLIENT_BEGIN(pSendBundle, ClientInterface::onEntityLeaveSpace, entityLeaveSpace);
	(*pSendBundle) << pEntity_->id();
	ENTITY_MESSAGE_FORWARD_CLIENT_END(pSendBundle, ClientInterface::onEntityLeaveSpace, entityLeaveSpace);

	if(!Cellapp::getSingleton().sendBundleToClientActor(pEntity_->id(), pSendBundle))
		pEntity_->clientEntityCall()->sendCall(pSendBundle);

	lastBasePos_.z = -FLT_MAX;
	lastBaseDir_.yaw(-FLT_MAX);

	VIEW_ENTITIES::iterator iter = viewEntities_.begin();
	for(; iter != viewEntities_.end(); ++iter)
	{
		if((*iter)->pEntity())
		{
			(*iter)->pEntity()->delWitnessed(pEntity_);
		}

		EntityRef::reclaimPoolObject((*iter));
	}

	viewEntities_.clear();
	viewEntities_map_.clear();

	clientViewSize_ = 0;
}

//-------------------------------------------------------------------------------------
void Witness::installViewTrigger()
{
	if (pViewTrigger_)
	{
		// ??????View???0?????????????????????
		if (viewRadius_ <= 0.f)
			return;

		// ????????pViewHysteresisAreaTrigger_???????ßª??????????????????
		// ???ª«???Avatar???y???????????????View?????????????????????????onWitnessed???????????????????
		// ????View????????¶ƒ???????????°‰?????????old_xx????-FLT_MAX????????????????????????Avatar??View???????ßÿ????
		// ???????pViewHysteresisAreaTrigger_????????????View?????????????pViewTrigger_?????????????????????????????????????????
		if (pViewHysteresisAreaTrigger_ && pEntity_/*?????????????????? */)
			pViewHysteresisAreaTrigger_->reinstall((CoordinateNode*)pEntity_->pEntityCoordinateNode());

		if (pEntity_/*?????????????????? */)
			pViewTrigger_->reinstall((CoordinateNode*)pEntity_->pEntityCoordinateNode());
	}
	else
	{
		KBE_ASSERT(pViewHysteresisAreaTrigger_ == NULL);
	}
}

//-------------------------------------------------------------------------------------
void Witness::uninstallViewTrigger()
{
	if (pViewTrigger_)
		pViewTrigger_->uninstall();

	if (pViewHysteresisAreaTrigger_)
		pViewHysteresisAreaTrigger_->uninstall();

	// ???????????View
	VIEW_ENTITIES::iterator iter = viewEntities_.begin();
	for (; iter != viewEntities_.end(); ++iter)
	{
		_onLeaveView((*iter));
	}
}

//-------------------------------------------------------------------------------------
bool Witness::pushBundle(Network::Bundle* pBundle)
{
	// Router mode: cellapp is no longer connected to the client directly
	// (EntityCallAbstract::getChannel() returns NULL). Flatten the legacy
	// client Bundle and deliver it to the client Actor via router.
	if(RouterMail::isEnabled())
	{
		if(pEntity_ == NULL)
		{
			Network::Bundle::reclaimPoolObject(pBundle);
			return false;
		}

		return Cellapp::getSingleton().sendBundleToClientActor(pEntity_->id(), pBundle);
	}

	Network::Channel* pc = pChannel();
	if(!pc)
		return false;

	pc->send(pBundle);
	return true;
}

//-------------------------------------------------------------------------------------
Network::Channel* Witness::pChannel()
{
	if(pEntity_ == NULL)
		return NULL;

	EntityCall* clientMB = pEntity_->clientEntityCall();
	if(!clientMB)
		return NULL;

	Network::Channel* pChannel = clientMB->getChannel();
	if(!pChannel)
		return NULL;
	
	return pChannel;
}

//-------------------------------------------------------------------------------------
void Witness::_addViewEntityIDToBundle(Network::Bundle* pBundle, EntityRef* pEntityRef)
{
	if(!EntityDef::entityAliasID())
	{
		(*pBundle) << pEntityRef->id();
	}
	else
	{
		// ???????????????????????????????????????entityID?????
		// clientViewSize_????????????????????????????
		if(clientViewSize_ > 255)
		{
			(*pBundle) << pEntityRef->id();
		}
		else
		{
			if ((pEntityRef->flags() & (ENTITYREF_FLAG_NORMAL)) > 0)
			{
				KBE_ASSERT(pEntityRef->aliasID() <= 255);
				(*pBundle) << (uint8)pEntityRef->aliasID();
			}
			else
			{
				(*pBundle) << pEntityRef->id();
			}
		}
	}
}

//-------------------------------------------------------------------------------------
const Network::MessageHandler& Witness::getViewEntityMessageHandler(const Network::MessageHandler& normalMsgHandler, 
	const Network::MessageHandler& optimizedMsgHandler, ENTITY_ID entityID, int& ialiasID)
{
	ialiasID = -1;
	if(!EntityDef::entityAliasID())
	{
		return normalMsgHandler;
	}
	else
	{
		if (clientViewSize_ > 255)
		{
			return normalMsgHandler;
		}
		else
		{
			uint8 aliasID = 0;
			if(entityID2AliasID(entityID, aliasID))
			{
				ialiasID = aliasID;
				return optimizedMsgHandler;
			}
			else
			{
				return normalMsgHandler;
			}
		}
	}
	
	return normalMsgHandler;
}

//-------------------------------------------------------------------------------------
bool Witness::entityID2AliasID(ENTITY_ID id, uint8& aliasID)
{
	VIEW_ENTITIES_MAP::iterator iter = viewEntities_map_.find(id);
	if (iter == viewEntities_map_.end())
	{
		aliasID = 0;
		return false;
	}

	EntityRef* pEntityRef = iter->second;
	if ((pEntityRef->flags() & (ENTITYREF_FLAG_NORMAL)) <= 0)
	{
		aliasID = 0;
		return false;
	}

	// ???
	if (pEntityRef->aliasID() > 255)
	{
		aliasID = 0;
		return false;
	}
	
	aliasID = (uint8)pEntityRef->aliasID();
	return true;
}

//-------------------------------------------------------------------------------------
void Witness::updateEntitiesAliasID()
{
	int n = 0;
	VIEW_ENTITIES::iterator iter = viewEntities_.begin();
	for(; iter != viewEntities_.end(); ++iter)
	{
		EntityRef* pEntityRef = (*iter);
		pEntityRef->aliasID(n++);
		
		if(n >= 255)
			break;
	}
}

//-------------------------------------------------------------------------------------
bool Witness::update()
{
	SCOPED_PROFILE(CLIENT_UPDATE_PROFILE);

	if(pEntity_ == NULL || !pEntity_->clientEntityCall())
		return true;

	Network::Channel* pChannel = pEntity_->clientEntityCall()->getChannel();

	// Router mode: the client is no longer connected to cellapp directly
	// (getChannel() is always NULL). Do not skip the update, deliver it to
	// the client Actor through the router instead.
	if(!pChannel && !RouterMail::isEnabled())
		return true;

	Py_INCREF(pEntity_);

	static bool notificationScriptBegin = PyObject_HasAttrString(pEntity_, "onUpdateBegin") > 0;
	if (notificationScriptBegin)
	{
		PyObject* pyResult = PyObject_CallMethod(pEntity_,
			const_cast<char*>("onUpdateBegin"),
			const_cast<char*>(""));

		if (pyResult != NULL)
		{
			Py_DECREF(pyResult);
		}
		else
		{
			SCRIPT_ERROR_CHECK();
		}
	}

	if (viewEntities_map_.size() > 0 || pEntity_->isControlledNotSelfClient())
	{
		// Router mode has no channel-cached bundle, use a plain pool object
		Network::Bundle* pSendBundle = (pChannel != NULL) ? pChannel->createSendBundle()
			: Network::Bundle::createPoolObject(OBJECTPOOL_POINT);
		
		// ??????pSendBundle?????????????????????????bundle????????????????
		bool isBufferedSendBundleMessageLength = pSendBundle->packets().size() > 0 ? true : 
			(pSendBundle->pCurrPacket() && pSendBundle->pCurrPacket()->length() > 0);
		
		NETWORK_ENTITY_MESSAGE_FORWARD_CLIENT_BEGIN(pEntity_->id(), (*pSendBundle));
		addBaseDataToStream(pSendBundle);

		VIEW_ENTITIES::iterator iter = viewEntities_.begin();
		for(; iter != viewEntities_.end(); )
		{
			EntityRef* pEntityRef = (*iter);
			
			if((pEntityRef->flags() & ENTITYREF_FLAG_ENTER_CLIENT_PENDING) > 0)
			{
				// ???????id??????°Í? ????entity?????View???????????????
				Entity* otherEntity = Cellapp::getSingleton().findEntity(pEntityRef->id());
				if(otherEntity == NULL)
				{
					pEntityRef->pEntity(NULL);
					_onLeaveView(pEntityRef);
					viewEntities_map_.erase(pEntityRef->id());
					EntityRef::reclaimPoolObject(pEntityRef);
					iter = viewEntities_.erase(iter);
					updateEntitiesAliasID();
					continue;
				}
				
				pEntityRef->removeflags(ENTITYREF_FLAG_ENTER_CLIENT_PENDING);

				MemoryStream* s1 = MemoryStream::createPoolObject(OBJECTPOOL_POINT);
				otherEntity->addPositionAndDirectionToStream(*s1, true);			
				otherEntity->addClientDataToStream(s1, true);
				
				ENTITY_MESSAGE_FORWARD_CLIENT_BEGIN(pSendBundle, ClientInterface::onUpdatePropertys, updatePropertys);
				(*pSendBundle) << otherEntity->id();
				(*pSendBundle).append(*s1);
				MemoryStream::reclaimPoolObject(s1);
				ENTITY_MESSAGE_FORWARD_CLIENT_END(pSendBundle, ClientInterface::onUpdatePropertys, updatePropertys);
				
				ENTITY_MESSAGE_FORWARD_CLIENT_BEGIN(pSendBundle, ClientInterface::onEntityEnterWorld, entityEnterWorld);
				(*pSendBundle) << otherEntity->id();
				otherEntity->pScriptModule()->addSmartUTypeToBundle(pSendBundle);
				if(!otherEntity->isOnGround())
					(*pSendBundle) << otherEntity->isOnGround();

				ENTITY_MESSAGE_FORWARD_CLIENT_END(pSendBundle, ClientInterface::onEntityEnterWorld, entityEnterWorld);

				pEntityRef->flags(ENTITYREF_FLAG_NORMAL);

				KBE_ASSERT(clientViewSize_ != 65535);

				++clientViewSize_;
			}
			else if((pEntityRef->flags() & ENTITYREF_FLAG_LEAVE_CLIENT_PENDING) > 0)
			{
				pEntityRef->removeflags(ENTITYREF_FLAG_LEAVE_CLIENT_PENDING);

				if((pEntityRef->flags() & ENTITYREF_FLAG_NORMAL) > 0)
				{
					ENTITY_MESSAGE_FORWARD_CLIENT_BEGIN(pSendBundle, ClientInterface::onEntityLeaveWorldOptimized, leaveWorld);
					_addViewEntityIDToBundle(pSendBundle, pEntityRef);
					ENTITY_MESSAGE_FORWARD_CLIENT_END(pSendBundle, ClientInterface::onEntityLeaveWorldOptimized, leaveWorld);
					
					KBE_ASSERT(clientViewSize_ > 0);
					--clientViewSize_;
				}

				viewEntities_map_.erase(pEntityRef->id());
				EntityRef::reclaimPoolObject(pEntityRef);
				iter = viewEntities_.erase(iter);
				updateEntitiesAliasID();
				continue;
			}
			else
			{
				Entity* otherEntity = pEntityRef->pEntity();
				if(otherEntity == NULL)
				{
					viewEntities_map_.erase(pEntityRef->id());
					EntityRef::reclaimPoolObject(pEntityRef);
					iter = viewEntities_.erase(iter);
					KBE_ASSERT(clientViewSize_ > 0);
					--clientViewSize_;
					updateEntitiesAliasID();
					continue;
				}
				
				KBE_ASSERT(pEntityRef->flags() == ENTITYREF_FLAG_NORMAL);
				
				addUpdateToStream(pSendBundle, getEntityVolatileDataUpdateFlags(otherEntity), pEntityRef);
			}

			++iter;
		}

		size_t pSendBundleMessageLength = pSendBundle->currMsgLength();
		if (pSendBundleMessageLength > 8/*NETWORK_ENTITY_MESSAGE_FORWARD_CLIENT_BEGIN?????????????ß≥*/)
		{
			if(pSendBundleMessageLength > PACKET_MAX_SIZE_TCP)
			{
				WARNING_MSG(fmt::format("Witness::update({}): sendToClient {} Bytes.\n", 
					pEntity_->id(), pSendBundleMessageLength));
			}

			AUTO_SCOPED_PROFILE("sendToClient");

			if(RouterMail::isEnabled())
				Cellapp::getSingleton().sendBundleToClientActor(pEntity_->id(), pSendBundle);
			else
				pChannel->send(pSendBundle);
			}
		else
		{
			// ???bundle??channel??????
			// ???????????????????????????????
			// ?????y?NETWORK_ENTITY_MESSAGE_FORWARD_CLIENT_BEGIN???????????
			if(isBufferedSendBundleMessageLength)
			{
				KBE_ASSERT(pSendBundleMessageLength == 8);
				pSendBundle->revokeMessage(8);
				pChannel->pushBundle(pSendBundle);
			}
			else
			{
				Network::Bundle::reclaimPoolObject(pSendBundle);
			}
		}
	}

	static bool notificationScriptEnd = PyObject_HasAttrString(pEntity_, "onUpdateEnd") > 0;
	if (notificationScriptEnd)
	{
		PyObject* pyResult = PyObject_CallMethod(pEntity_,
			const_cast<char*>("onUpdateEnd"),
			const_cast<char*>(""));

		if (pyResult != NULL)
		{
			Py_DECREF(pyResult);
		}
		else
		{
			SCRIPT_ERROR_CHECK();
		}
	}

	Py_DECREF(pEntity_);
	return true;
}

//-------------------------------------------------------------------------------------
void Witness::addBaseDataToStream(Network::Bundle* pSendBundle)
{
	if (pEntity_->isControlledNotSelfClient())
	{
		const Direction3D& bdir = baseDir();
		Vector3 changeDir = bdir.dir - lastBaseDir_.dir;

		if (KBEVec3Length(&changeDir) > 0.0004f)
		{
			ENTITY_MESSAGE_FORWARD_CLIENT_BEGIN(pSendBundle, ClientInterface::onUpdateBaseDir, onUpdateBaseDir);
			(*pSendBundle) << bdir.yaw() << bdir.pitch() << bdir.roll();
			ENTITY_MESSAGE_FORWARD_CLIENT_END(pSendBundle, ClientInterface::onUpdateBaseDir, onUpdateBaseDir);
			lastBaseDir_ = bdir;
		}
	}

	const Position3D& bpos = basePos();
	Vector3 movement = bpos - lastBasePos_;

	if(KBEVec3Length(&movement) < 0.0004f)
		return;

	if (fabs(lastBasePos_.y - bpos.y) > 0.0004f)
	{
		ENTITY_MESSAGE_FORWARD_CLIENT_BEGIN(pSendBundle, ClientInterface::onUpdateBasePos, basePos);
		pSendBundle->appendPackAnyXYZ(bpos.x, bpos.y, bpos.z, 0.f);
		ENTITY_MESSAGE_FORWARD_CLIENT_END(pSendBundle, ClientInterface::onUpdateBasePos, basePos);
	}
	else
	{
		ENTITY_MESSAGE_FORWARD_CLIENT_BEGIN(pSendBundle, ClientInterface::onUpdateBasePosXZ, basePos);
		pSendBundle->appendPackAnyXZ(bpos.x, bpos.z, 0.f);
		ENTITY_MESSAGE_FORWARD_CLIENT_END(pSendBundle, ClientInterface::onUpdateBasePosXZ, basePos);
	}

	lastBasePos_ = bpos;
}

//-------------------------------------------------------------------------------------
void Witness::addUpdateToStream(Network::Bundle* pForwardBundle, uint32 flags, EntityRef* pEntityRef)
{
	Entity* otherEntity = pEntityRef->pEntity();

	static uint8 type = g_kbeSrvConfig.getCellApp().entity_posdir_updates_type;
	static uint16 threshold = g_kbeSrvConfig.getCellApp().entity_posdir_updates_smart_threshold;
	
	bool isOptimized = true;
	if ((type == 2 && clientViewSize_ <= threshold) || type == 0)
	{
		isOptimized = false;
	} 
	
	if (isOptimized)
	{
		switch (flags)
		{
		case UPDATE_FLAG_NULL:
		{
			// (*pForwardBundle).newMessage(ClientInterface::onUpdateData);
		}
		break;
		case UPDATE_FLAG_XZ:
		{
			Position3D relativePos = otherEntity->position() - this->pEntity()->position();

			ENTITY_MESSAGE_FORWARD_CLIENT_BEGIN(pForwardBundle, ClientInterface::onUpdateData_xz_optimized, update);
			_addViewEntityIDToBundle(pForwardBundle, pEntityRef);
			pForwardBundle->appendPackXZ(relativePos.x, relativePos.z);
			ENTITY_MESSAGE_FORWARD_CLIENT_END(pForwardBundle, ClientInterface::onUpdateData_xz_optimized, update);
		}
		break;
		case UPDATE_FLAG_XYZ:
		{
			Position3D relativePos = otherEntity->position() - this->pEntity()->position();

			ENTITY_MESSAGE_FORWARD_CLIENT_BEGIN(pForwardBundle, ClientInterface::onUpdateData_xyz_optimized, update);
			_addViewEntityIDToBundle(pForwardBundle, pEntityRef);
			pForwardBundle->appendPackXZ(relativePos.x, relativePos.z);
			pForwardBundle->appendPackY(relativePos.y);
			ENTITY_MESSAGE_FORWARD_CLIENT_END(pForwardBundle, ClientInterface::onUpdateData_xyz_optimized, update);
		}
		break;
		case UPDATE_FLAG_YAW:
		{
			const Direction3D& dir = otherEntity->direction();

			ENTITY_MESSAGE_FORWARD_CLIENT_BEGIN(pForwardBundle, ClientInterface::onUpdateData_y_optimized, update);
			_addViewEntityIDToBundle(pForwardBundle, pEntityRef);
			(*pForwardBundle) << angle2int8(dir.yaw());
			ENTITY_MESSAGE_FORWARD_CLIENT_END(pForwardBundle, ClientInterface::onUpdateData_y_optimized, update);
		}
		break;
		case UPDATE_FLAG_ROLL:
		{
			const Direction3D& dir = otherEntity->direction();

			ENTITY_MESSAGE_FORWARD_CLIENT_BEGIN(pForwardBundle, ClientInterface::onUpdateData_r_optimized, update);
			_addViewEntityIDToBundle(pForwardBundle, pEntityRef);
			(*pForwardBundle) << angle2int8(dir.roll());
			ENTITY_MESSAGE_FORWARD_CLIENT_END(pForwardBundle, ClientInterface::onUpdateData_r_optimized, update);
		}
		break;
		case UPDATE_FLAG_PITCH:
		{
			const Direction3D& dir = otherEntity->direction();

			ENTITY_MESSAGE_FORWARD_CLIENT_BEGIN(pForwardBundle, ClientInterface::onUpdateData_p_optimized, update);
			_addViewEntityIDToBundle(pForwardBundle, pEntityRef);
			(*pForwardBundle) << angle2int8(dir.pitch());
			ENTITY_MESSAGE_FORWARD_CLIENT_END(pForwardBundle, ClientInterface::onUpdateData_p_optimized, update);
		}
		break;
		case UPDATE_FLAG_YAW_PITCH_ROLL:
		{
			const Direction3D& dir = otherEntity->direction();

			ENTITY_MESSAGE_FORWARD_CLIENT_BEGIN(pForwardBundle, ClientInterface::onUpdateData_ypr_optimized, update);
			_addViewEntityIDToBundle(pForwardBundle, pEntityRef);
			(*pForwardBundle) << angle2int8(dir.yaw());
			(*pForwardBundle) << angle2int8(dir.pitch());
			(*pForwardBundle) << angle2int8(dir.roll());
			ENTITY_MESSAGE_FORWARD_CLIENT_END(pForwardBundle, ClientInterface::onUpdateData_ypr_optimized, update);
		}
		break;
		case UPDATE_FLAG_YAW_PITCH:
		{
			const Direction3D& dir = otherEntity->direction();

			ENTITY_MESSAGE_FORWARD_CLIENT_BEGIN(pForwardBundle, ClientInterface::onUpdateData_yp_optimized, update);
			_addViewEntityIDToBundle(pForwardBundle, pEntityRef);
			(*pForwardBundle) << angle2int8(dir.yaw());
			(*pForwardBundle) << angle2int8(dir.pitch());
			ENTITY_MESSAGE_FORWARD_CLIENT_END(pForwardBundle, ClientInterface::onUpdateData_yp_optimized, update);
		}
		break;
		case UPDATE_FLAG_YAW_ROLL:
		{
			const Direction3D& dir = otherEntity->direction();

			ENTITY_MESSAGE_FORWARD_CLIENT_BEGIN(pForwardBundle, ClientInterface::onUpdateData_yr_optimized, update);
			_addViewEntityIDToBundle(pForwardBundle, pEntityRef);
			(*pForwardBundle) << angle2int8(dir.yaw());
			(*pForwardBundle) << angle2int8(dir.roll());
			ENTITY_MESSAGE_FORWARD_CLIENT_END(pForwardBundle, ClientInterface::onUpdateData_yr_optimized, update);
		}
		break;
		case UPDATE_FLAG_PITCH_ROLL:
		{
			const Direction3D& dir = otherEntity->direction();

			ENTITY_MESSAGE_FORWARD_CLIENT_BEGIN(pForwardBundle, ClientInterface::onUpdateData_pr_optimized, update);
			_addViewEntityIDToBundle(pForwardBundle, pEntityRef);
			(*pForwardBundle) << angle2int8(dir.pitch());
			(*pForwardBundle) << angle2int8(dir.roll());
			ENTITY_MESSAGE_FORWARD_CLIENT_END(pForwardBundle, ClientInterface::onUpdateData_pr_optimized, update);
		}
		break;
		case (UPDATE_FLAG_XZ | UPDATE_FLAG_YAW):
		{
			Position3D relativePos = otherEntity->position() - this->pEntity()->position();
			const Direction3D& dir = otherEntity->direction();

			ENTITY_MESSAGE_FORWARD_CLIENT_BEGIN(pForwardBundle, ClientInterface::onUpdateData_xz_y_optimized, update);
			_addViewEntityIDToBundle(pForwardBundle, pEntityRef);
			pForwardBundle->appendPackXZ(relativePos.x, relativePos.z);
			(*pForwardBundle) << angle2int8(dir.yaw());
			ENTITY_MESSAGE_FORWARD_CLIENT_END(pForwardBundle, ClientInterface::onUpdateData_xz_y_optimized, update);
		}
		break;
		case (UPDATE_FLAG_XZ | UPDATE_FLAG_PITCH):
		{
			Position3D relativePos = otherEntity->position() - this->pEntity()->position();
			const Direction3D& dir = otherEntity->direction();

			ENTITY_MESSAGE_FORWARD_CLIENT_BEGIN(pForwardBundle, ClientInterface::onUpdateData_xz_p_optimized, update);
			_addViewEntityIDToBundle(pForwardBundle, pEntityRef);
			pForwardBundle->appendPackXZ(relativePos.x, relativePos.z);
			(*pForwardBundle) << angle2int8(dir.pitch());
			ENTITY_MESSAGE_FORWARD_CLIENT_END(pForwardBundle, ClientInterface::onUpdateData_xz_p_optimized, update);
		}
		break;
		case (UPDATE_FLAG_XZ | UPDATE_FLAG_ROLL):
		{
			Position3D relativePos = otherEntity->position() - this->pEntity()->position();
			const Direction3D& dir = otherEntity->direction();

			ENTITY_MESSAGE_FORWARD_CLIENT_BEGIN(pForwardBundle, ClientInterface::onUpdateData_xz_r_optimized, update);
			_addViewEntityIDToBundle(pForwardBundle, pEntityRef);
			pForwardBundle->appendPackXZ(relativePos.x, relativePos.z);
			(*pForwardBundle) << angle2int8(dir.roll());
			ENTITY_MESSAGE_FORWARD_CLIENT_END(pForwardBundle, ClientInterface::onUpdateData_xz_r_optimized, update);
		}
		break;
		case (UPDATE_FLAG_XZ | UPDATE_FLAG_YAW_ROLL):
		{
			Position3D relativePos = otherEntity->position() - this->pEntity()->position();
			const Direction3D& dir = otherEntity->direction();

			ENTITY_MESSAGE_FORWARD_CLIENT_BEGIN(pForwardBundle, ClientInterface::onUpdateData_xz_yr_optimized, update);
			_addViewEntityIDToBundle(pForwardBundle, pEntityRef);
			pForwardBundle->appendPackXZ(relativePos.x, relativePos.z);
			(*pForwardBundle) << angle2int8(dir.yaw());
			(*pForwardBundle) << angle2int8(dir.roll());
			ENTITY_MESSAGE_FORWARD_CLIENT_END(pForwardBundle, ClientInterface::onUpdateData_xz_yr_optimized, update);
		}
		break;
		case (UPDATE_FLAG_XZ | UPDATE_FLAG_YAW_PITCH):
		{
			Position3D relativePos = otherEntity->position() - this->pEntity()->position();
			const Direction3D& dir = otherEntity->direction();

			ENTITY_MESSAGE_FORWARD_CLIENT_BEGIN(pForwardBundle, ClientInterface::onUpdateData_xz_yp_optimized, update);
			_addViewEntityIDToBundle(pForwardBundle, pEntityRef);
			pForwardBundle->appendPackXZ(relativePos.x, relativePos.z);
			(*pForwardBundle) << angle2int8(dir.yaw());
			(*pForwardBundle) << angle2int8(dir.pitch());
			ENTITY_MESSAGE_FORWARD_CLIENT_END(pForwardBundle, ClientInterface::onUpdateData_xz_yp_optimized, update);
		}
		break;
		case (UPDATE_FLAG_XZ | UPDATE_FLAG_PITCH_ROLL):
		{
			Position3D relativePos = otherEntity->position() - this->pEntity()->position();
			const Direction3D& dir = otherEntity->direction();

			ENTITY_MESSAGE_FORWARD_CLIENT_BEGIN(pForwardBundle, ClientInterface::onUpdateData_xz_pr_optimized, update);
			_addViewEntityIDToBundle(pForwardBundle, pEntityRef);
			pForwardBundle->appendPackXZ(relativePos.x, relativePos.z);
			(*pForwardBundle) << angle2int8(dir.pitch());
			(*pForwardBundle) << angle2int8(dir.roll());
			ENTITY_MESSAGE_FORWARD_CLIENT_END(pForwardBundle, ClientInterface::onUpdateData_xz_pr_optimized, update);
		}
		break;
		case (UPDATE_FLAG_XZ | UPDATE_FLAG_YAW_PITCH_ROLL):
		{
			Position3D relativePos = otherEntity->position() - this->pEntity()->position();
			const Direction3D& dir = otherEntity->direction();

			ENTITY_MESSAGE_FORWARD_CLIENT_BEGIN(pForwardBundle, ClientInterface::onUpdateData_xz_ypr_optimized, update);
			_addViewEntityIDToBundle(pForwardBundle, pEntityRef);
			pForwardBundle->appendPackXZ(relativePos.x, relativePos.z);
			(*pForwardBundle) << angle2int8(dir.yaw());
			(*pForwardBundle) << angle2int8(dir.pitch());
			(*pForwardBundle) << angle2int8(dir.roll());
			ENTITY_MESSAGE_FORWARD_CLIENT_END(pForwardBundle, ClientInterface::onUpdateData_xz_ypr_optimized, update);
		}
		break;
		case (UPDATE_FLAG_XYZ | UPDATE_FLAG_YAW):
		{
			Position3D relativePos = otherEntity->position() - this->pEntity()->position();
			const Direction3D& dir = otherEntity->direction();

			ENTITY_MESSAGE_FORWARD_CLIENT_BEGIN(pForwardBundle, ClientInterface::onUpdateData_xyz_y_optimized, update);
			_addViewEntityIDToBundle(pForwardBundle, pEntityRef);
			pForwardBundle->appendPackXZ(relativePos.x, relativePos.z);
			pForwardBundle->appendPackY(relativePos.y);
			(*pForwardBundle) << angle2int8(dir.yaw());
			ENTITY_MESSAGE_FORWARD_CLIENT_END(pForwardBundle, ClientInterface::onUpdateData_xyz_y_optimized, update);
		}
		break;
		case (UPDATE_FLAG_XYZ | UPDATE_FLAG_PITCH):
		{
			Position3D relativePos = otherEntity->position() - this->pEntity()->position();
			const Direction3D& dir = otherEntity->direction();

			ENTITY_MESSAGE_FORWARD_CLIENT_BEGIN(pForwardBundle, ClientInterface::onUpdateData_xyz_p_optimized, update);
			_addViewEntityIDToBundle(pForwardBundle, pEntityRef);
			pForwardBundle->appendPackXZ(relativePos.x, relativePos.z);
			pForwardBundle->appendPackY(relativePos.y);
			(*pForwardBundle) << angle2int8(dir.pitch());
			ENTITY_MESSAGE_FORWARD_CLIENT_END(pForwardBundle, ClientInterface::onUpdateData_xyz_p_optimized, update);
		}
		break;
		case (UPDATE_FLAG_XYZ | UPDATE_FLAG_ROLL):
		{
			Position3D relativePos = otherEntity->position() - this->pEntity()->position();
			const Direction3D& dir = otherEntity->direction();

			ENTITY_MESSAGE_FORWARD_CLIENT_BEGIN(pForwardBundle, ClientInterface::onUpdateData_xyz_r_optimized, update);
			_addViewEntityIDToBundle(pForwardBundle, pEntityRef);
			pForwardBundle->appendPackXZ(relativePos.x, relativePos.z);
			pForwardBundle->appendPackY(relativePos.y);
			(*pForwardBundle) << angle2int8(dir.roll());
			ENTITY_MESSAGE_FORWARD_CLIENT_END(pForwardBundle, ClientInterface::onUpdateData_xyz_r_optimized, update);
		}
		break;
		case (UPDATE_FLAG_XYZ | UPDATE_FLAG_YAW_ROLL):
		{
			Position3D relativePos = otherEntity->position() - this->pEntity()->position();
			const Direction3D& dir = otherEntity->direction();

			ENTITY_MESSAGE_FORWARD_CLIENT_BEGIN(pForwardBundle, ClientInterface::onUpdateData_xyz_yr_optimized, update);
			_addViewEntityIDToBundle(pForwardBundle, pEntityRef);
			pForwardBundle->appendPackXZ(relativePos.x, relativePos.z);
			pForwardBundle->appendPackY(relativePos.y);
			(*pForwardBundle) << angle2int8(dir.yaw());
			(*pForwardBundle) << angle2int8(dir.roll());
			ENTITY_MESSAGE_FORWARD_CLIENT_END(pForwardBundle, ClientInterface::onUpdateData_xyz_yr_optimized, update);
		}
		break;
		case (UPDATE_FLAG_XYZ | UPDATE_FLAG_YAW_PITCH):
		{
			Position3D relativePos = otherEntity->position() - this->pEntity()->position();
			const Direction3D& dir = otherEntity->direction();

			ENTITY_MESSAGE_FORWARD_CLIENT_BEGIN(pForwardBundle, ClientInterface::onUpdateData_xyz_yp_optimized, update);
			_addViewEntityIDToBundle(pForwardBundle, pEntityRef);
			pForwardBundle->appendPackXZ(relativePos.x, relativePos.z);
			pForwardBundle->appendPackY(relativePos.y);
			(*pForwardBundle) << angle2int8(dir.yaw());
			(*pForwardBundle) << angle2int8(dir.pitch());
			ENTITY_MESSAGE_FORWARD_CLIENT_END(pForwardBundle, ClientInterface::onUpdateData_xyz_yp_optimized, update);
		}
		break;
		case (UPDATE_FLAG_XYZ | UPDATE_FLAG_PITCH_ROLL):
		{
			Position3D relativePos = otherEntity->position() - this->pEntity()->position();
			const Direction3D& dir = otherEntity->direction();

			ENTITY_MESSAGE_FORWARD_CLIENT_BEGIN(pForwardBundle, ClientInterface::onUpdateData_xyz_pr_optimized, update);
			_addViewEntityIDToBundle(pForwardBundle, pEntityRef);
			pForwardBundle->appendPackXZ(relativePos.x, relativePos.z);
			pForwardBundle->appendPackY(relativePos.y);
			(*pForwardBundle) << angle2int8(dir.pitch());
			(*pForwardBundle) << angle2int8(dir.roll());
			ENTITY_MESSAGE_FORWARD_CLIENT_END(pForwardBundle, ClientInterface::onUpdateData_xyz_pr_optimized, update);
		}
		break;
		case (UPDATE_FLAG_XYZ | UPDATE_FLAG_YAW_PITCH_ROLL):
		{
			Position3D relativePos = otherEntity->position() - this->pEntity()->position();
			const Direction3D& dir = otherEntity->direction();

			ENTITY_MESSAGE_FORWARD_CLIENT_BEGIN(pForwardBundle, ClientInterface::onUpdateData_xyz_ypr_optimized, update);
			_addViewEntityIDToBundle(pForwardBundle, pEntityRef);
			pForwardBundle->appendPackXZ(relativePos.x, relativePos.z);
			pForwardBundle->appendPackY(relativePos.y);
			(*pForwardBundle) << angle2int8(dir.yaw());
			(*pForwardBundle) << angle2int8(dir.pitch());
			(*pForwardBundle) << angle2int8(dir.roll());
			ENTITY_MESSAGE_FORWARD_CLIENT_END(pForwardBundle, ClientInterface::onUpdateData_xyz_ypr_optimized, update);
		}
		break;
		default:
			KBE_ASSERT(false);
			break;
		};
	}
	else
	{
		switch (flags)
		{
		case UPDATE_FLAG_NULL:
		{
			// (*pForwardBundle).newMessage(ClientInterface::onUpdateData);
		}
		break;
		case UPDATE_FLAG_XZ:
		{
			const Position3D& pos = otherEntity->position();

			ENTITY_MESSAGE_FORWARD_CLIENT_BEGIN(pForwardBundle, ClientInterface::onUpdateData_xz, update);
			_addViewEntityIDToBundle(pForwardBundle, pEntityRef);
			(*pForwardBundle) << pos.x;
			(*pForwardBundle) << pos.z;
			ENTITY_MESSAGE_FORWARD_CLIENT_END(pForwardBundle, ClientInterface::onUpdateData_xz, update);
		}
		break;
		case UPDATE_FLAG_XYZ:
		{
			const Position3D& pos = otherEntity->position();

			ENTITY_MESSAGE_FORWARD_CLIENT_BEGIN(pForwardBundle, ClientInterface::onUpdateData_xyz, update);
			_addViewEntityIDToBundle(pForwardBundle, pEntityRef);
			(*pForwardBundle) << pos.x;
			(*pForwardBundle) << pos.y;
			(*pForwardBundle) << pos.z;
			ENTITY_MESSAGE_FORWARD_CLIENT_END(pForwardBundle, ClientInterface::onUpdateData_xyz, update);
		}
		break;
		case UPDATE_FLAG_YAW:
		{
			const Direction3D& dir = otherEntity->direction();

			ENTITY_MESSAGE_FORWARD_CLIENT_BEGIN(pForwardBundle, ClientInterface::onUpdateData_y, update);
			_addViewEntityIDToBundle(pForwardBundle, pEntityRef);
			(*pForwardBundle) << dir.yaw();
			ENTITY_MESSAGE_FORWARD_CLIENT_END(pForwardBundle, ClientInterface::onUpdateData_y, update);
		}
		break;
		case UPDATE_FLAG_ROLL:
		{
			const Direction3D& dir = otherEntity->direction();

			ENTITY_MESSAGE_FORWARD_CLIENT_BEGIN(pForwardBundle, ClientInterface::onUpdateData_r, update);
			_addViewEntityIDToBundle(pForwardBundle, pEntityRef);
			(*pForwardBundle) << dir.roll();
			ENTITY_MESSAGE_FORWARD_CLIENT_END(pForwardBundle, ClientInterface::onUpdateData_r, update);
		}
		break;
		case UPDATE_FLAG_PITCH:
		{
			const Direction3D& dir = otherEntity->direction();

			ENTITY_MESSAGE_FORWARD_CLIENT_BEGIN(pForwardBundle, ClientInterface::onUpdateData_p, update);
			_addViewEntityIDToBundle(pForwardBundle, pEntityRef);
			(*pForwardBundle) << dir.pitch();
			ENTITY_MESSAGE_FORWARD_CLIENT_END(pForwardBundle, ClientInterface::onUpdateData_p, update);
		}
		break;
		case UPDATE_FLAG_YAW_PITCH_ROLL:
		{
			const Direction3D& dir = otherEntity->direction();

			ENTITY_MESSAGE_FORWARD_CLIENT_BEGIN(pForwardBundle, ClientInterface::onUpdateData_ypr, update);
			_addViewEntityIDToBundle(pForwardBundle, pEntityRef);
			(*pForwardBundle) << dir.yaw();
			(*pForwardBundle) << dir.pitch();
			(*pForwardBundle) << dir.roll();
			ENTITY_MESSAGE_FORWARD_CLIENT_END(pForwardBundle, ClientInterface::onUpdateData_ypr, update);
		}
		break;
		case UPDATE_FLAG_YAW_PITCH:
		{
			const Direction3D& dir = otherEntity->direction();

			ENTITY_MESSAGE_FORWARD_CLIENT_BEGIN(pForwardBundle, ClientInterface::onUpdateData_yp, update);
			_addViewEntityIDToBundle(pForwardBundle, pEntityRef);
			(*pForwardBundle) << dir.yaw();
			(*pForwardBundle) << dir.pitch();
			ENTITY_MESSAGE_FORWARD_CLIENT_END(pForwardBundle, ClientInterface::onUpdateData_yp, update);
		}
		break;
		case UPDATE_FLAG_YAW_ROLL:
		{
			const Direction3D& dir = otherEntity->direction();

			ENTITY_MESSAGE_FORWARD_CLIENT_BEGIN(pForwardBundle, ClientInterface::onUpdateData_yr, update);
			_addViewEntityIDToBundle(pForwardBundle, pEntityRef);
			(*pForwardBundle) << dir.yaw();
			(*pForwardBundle) << dir.roll();
			ENTITY_MESSAGE_FORWARD_CLIENT_END(pForwardBundle, ClientInterface::onUpdateData_yr, update);
		}
		break;
		case UPDATE_FLAG_PITCH_ROLL:
		{
			const Direction3D& dir = otherEntity->direction();

			ENTITY_MESSAGE_FORWARD_CLIENT_BEGIN(pForwardBundle, ClientInterface::onUpdateData_pr, update);
			_addViewEntityIDToBundle(pForwardBundle, pEntityRef);
			(*pForwardBundle) << dir.pitch();
			(*pForwardBundle) << dir.roll();
			ENTITY_MESSAGE_FORWARD_CLIENT_END(pForwardBundle, ClientInterface::onUpdateData_pr, update);
		}
		break;
		case (UPDATE_FLAG_XZ | UPDATE_FLAG_YAW):
		{
			const Position3D& pos = otherEntity->position();
			const Direction3D& dir = otherEntity->direction();

			ENTITY_MESSAGE_FORWARD_CLIENT_BEGIN(pForwardBundle, ClientInterface::onUpdateData_xz_y, update);
			_addViewEntityIDToBundle(pForwardBundle, pEntityRef);
			(*pForwardBundle) << pos.x;
			(*pForwardBundle) << pos.z;
			(*pForwardBundle) << dir.yaw();
			ENTITY_MESSAGE_FORWARD_CLIENT_END(pForwardBundle, ClientInterface::onUpdateData_xz_y, update);
		}
		break;
		case (UPDATE_FLAG_XZ | UPDATE_FLAG_PITCH):
		{
			const Position3D& pos = otherEntity->position();
			const Direction3D& dir = otherEntity->direction();

			ENTITY_MESSAGE_FORWARD_CLIENT_BEGIN(pForwardBundle, ClientInterface::onUpdateData_xz_p, update);
			_addViewEntityIDToBundle(pForwardBundle, pEntityRef);
			(*pForwardBundle) << pos.x;
			(*pForwardBundle) << pos.z;
			(*pForwardBundle) << dir.pitch();
			ENTITY_MESSAGE_FORWARD_CLIENT_END(pForwardBundle, ClientInterface::onUpdateData_xz_p, update);
		}
		break;
		case (UPDATE_FLAG_XZ | UPDATE_FLAG_ROLL):
		{
			const Position3D& pos = otherEntity->position();
			const Direction3D& dir = otherEntity->direction();

			ENTITY_MESSAGE_FORWARD_CLIENT_BEGIN(pForwardBundle, ClientInterface::onUpdateData_xz_r, update);
			_addViewEntityIDToBundle(pForwardBundle, pEntityRef);
			(*pForwardBundle) << pos.x;
			(*pForwardBundle) << pos.z;
			(*pForwardBundle) << dir.roll();
			ENTITY_MESSAGE_FORWARD_CLIENT_END(pForwardBundle, ClientInterface::onUpdateData_xz_r, update);
		}
		break;
		case (UPDATE_FLAG_XZ | UPDATE_FLAG_YAW_ROLL):
		{
			const Position3D& pos = otherEntity->position();
			const Direction3D& dir = otherEntity->direction();

			ENTITY_MESSAGE_FORWARD_CLIENT_BEGIN(pForwardBundle, ClientInterface::onUpdateData_xz_yr, update);
			_addViewEntityIDToBundle(pForwardBundle, pEntityRef);
			(*pForwardBundle) << pos.x;
			(*pForwardBundle) << pos.z;
			(*pForwardBundle) << dir.yaw();
			(*pForwardBundle) << dir.roll();
			ENTITY_MESSAGE_FORWARD_CLIENT_END(pForwardBundle, ClientInterface::onUpdateData_xz_yr, update);
		}
		break;
		case (UPDATE_FLAG_XZ | UPDATE_FLAG_YAW_PITCH):
		{
			const Position3D& pos = otherEntity->position();
			const Direction3D& dir = otherEntity->direction();

			ENTITY_MESSAGE_FORWARD_CLIENT_BEGIN(pForwardBundle, ClientInterface::onUpdateData_xz_yp, update);
			_addViewEntityIDToBundle(pForwardBundle, pEntityRef);
			(*pForwardBundle) << pos.x;
			(*pForwardBundle) << pos.z;
			(*pForwardBundle) << dir.yaw();
			(*pForwardBundle) << dir.pitch();
			ENTITY_MESSAGE_FORWARD_CLIENT_END(pForwardBundle, ClientInterface::onUpdateData_xz_yp, update);
		}
		break;
		case (UPDATE_FLAG_XZ | UPDATE_FLAG_PITCH_ROLL):
		{
			const Position3D& pos = otherEntity->position();
			const Direction3D& dir = otherEntity->direction();

			ENTITY_MESSAGE_FORWARD_CLIENT_BEGIN(pForwardBundle, ClientInterface::onUpdateData_xz_pr, update);
			_addViewEntityIDToBundle(pForwardBundle, pEntityRef);
			(*pForwardBundle) << pos.x;
			(*pForwardBundle) << pos.z;
			(*pForwardBundle) << dir.pitch();
			(*pForwardBundle) << dir.roll();
			ENTITY_MESSAGE_FORWARD_CLIENT_END(pForwardBundle, ClientInterface::onUpdateData_xz_pr, update);
		}
		break;
		case (UPDATE_FLAG_XZ | UPDATE_FLAG_YAW_PITCH_ROLL):
		{
			const Position3D& pos = otherEntity->position();
			const Direction3D& dir = otherEntity->direction();

			ENTITY_MESSAGE_FORWARD_CLIENT_BEGIN(pForwardBundle, ClientInterface::onUpdateData_xz_ypr, update);
			_addViewEntityIDToBundle(pForwardBundle, pEntityRef);
			(*pForwardBundle) << pos.x;
			(*pForwardBundle) << pos.z;
			(*pForwardBundle) << dir.yaw();
			(*pForwardBundle) << dir.pitch();
			(*pForwardBundle) << dir.roll();
			ENTITY_MESSAGE_FORWARD_CLIENT_END(pForwardBundle, ClientInterface::onUpdateData_xz_ypr, update);
		}
		break;
		case (UPDATE_FLAG_XYZ | UPDATE_FLAG_YAW):
		{
			const Position3D& pos = otherEntity->position();
			const Direction3D& dir = otherEntity->direction();

			ENTITY_MESSAGE_FORWARD_CLIENT_BEGIN(pForwardBundle, ClientInterface::onUpdateData_xyz_y, update);
			_addViewEntityIDToBundle(pForwardBundle, pEntityRef);
			(*pForwardBundle) << pos.x;
			(*pForwardBundle) << pos.y;
			(*pForwardBundle) << pos.z;
			(*pForwardBundle) << dir.yaw();
			ENTITY_MESSAGE_FORWARD_CLIENT_END(pForwardBundle, ClientInterface::onUpdateData_xyz_y, update);
		}
		break;
		case (UPDATE_FLAG_XYZ | UPDATE_FLAG_PITCH):
		{
			const Position3D& pos = otherEntity->position();
			const Direction3D& dir = otherEntity->direction();

			ENTITY_MESSAGE_FORWARD_CLIENT_BEGIN(pForwardBundle, ClientInterface::onUpdateData_xyz_p, update);
			_addViewEntityIDToBundle(pForwardBundle, pEntityRef);
			(*pForwardBundle) << pos.x;
			(*pForwardBundle) << pos.y;
			(*pForwardBundle) << pos.z;
			(*pForwardBundle) << dir.pitch();
			ENTITY_MESSAGE_FORWARD_CLIENT_END(pForwardBundle, ClientInterface::onUpdateData_xyz, update);
		}
		break;
		case (UPDATE_FLAG_XYZ | UPDATE_FLAG_ROLL):
		{
			const Position3D& pos = otherEntity->position();
			const Direction3D& dir = otherEntity->direction();

			ENTITY_MESSAGE_FORWARD_CLIENT_BEGIN(pForwardBundle, ClientInterface::onUpdateData_xyz_r, update);
			_addViewEntityIDToBundle(pForwardBundle, pEntityRef);
			(*pForwardBundle) << pos.x;
			(*pForwardBundle) << pos.y;
			(*pForwardBundle) << pos.z;
			(*pForwardBundle) << dir.roll();
			ENTITY_MESSAGE_FORWARD_CLIENT_END(pForwardBundle, ClientInterface::onUpdateData_xyz_r, update);
		}
		break;
		case (UPDATE_FLAG_XYZ | UPDATE_FLAG_YAW_ROLL):
		{
			const Position3D& pos = otherEntity->position();
			const Direction3D& dir = otherEntity->direction();

			ENTITY_MESSAGE_FORWARD_CLIENT_BEGIN(pForwardBundle, ClientInterface::onUpdateData_xyz_yr, update);
			_addViewEntityIDToBundle(pForwardBundle, pEntityRef);
			(*pForwardBundle) << pos.x;
			(*pForwardBundle) << pos.y;
			(*pForwardBundle) << pos.z;
			(*pForwardBundle) << dir.yaw();
			(*pForwardBundle) << dir.roll();
			ENTITY_MESSAGE_FORWARD_CLIENT_END(pForwardBundle, ClientInterface::onUpdateData_xyz_yr, update);
		}
		break;
		case (UPDATE_FLAG_XYZ | UPDATE_FLAG_YAW_PITCH):
		{
			const Position3D& pos = otherEntity->position();
			const Direction3D& dir = otherEntity->direction();

			ENTITY_MESSAGE_FORWARD_CLIENT_BEGIN(pForwardBundle, ClientInterface::onUpdateData_xyz_yp, update);
			_addViewEntityIDToBundle(pForwardBundle, pEntityRef);
			(*pForwardBundle) << pos.x;
			(*pForwardBundle) << pos.y;
			(*pForwardBundle) << pos.z;
			(*pForwardBundle) << dir.yaw();
			(*pForwardBundle) << dir.pitch();
			ENTITY_MESSAGE_FORWARD_CLIENT_END(pForwardBundle, ClientInterface::onUpdateData_xyz_yp, update);
		}
		break;
		case (UPDATE_FLAG_XYZ | UPDATE_FLAG_PITCH_ROLL):
		{
			const Position3D& pos = otherEntity->position();
			const Direction3D& dir = otherEntity->direction();

			ENTITY_MESSAGE_FORWARD_CLIENT_BEGIN(pForwardBundle, ClientInterface::onUpdateData_xyz_pr, update);
			_addViewEntityIDToBundle(pForwardBundle, pEntityRef);
			(*pForwardBundle) << pos.x;
			(*pForwardBundle) << pos.y;
			(*pForwardBundle) << pos.z;
			(*pForwardBundle) << dir.pitch();
			(*pForwardBundle) << dir.roll();
			ENTITY_MESSAGE_FORWARD_CLIENT_END(pForwardBundle, ClientInterface::onUpdateData_xyz_pr, update);
		}
		break;
		case (UPDATE_FLAG_XYZ | UPDATE_FLAG_YAW_PITCH_ROLL):
		{
			const Position3D& pos = otherEntity->position();
			const Direction3D& dir = otherEntity->direction();

			ENTITY_MESSAGE_FORWARD_CLIENT_BEGIN(pForwardBundle, ClientInterface::onUpdateData_xyz_ypr, update);
			_addViewEntityIDToBundle(pForwardBundle, pEntityRef);
			(*pForwardBundle) << pos.x;
			(*pForwardBundle) << pos.y;
			(*pForwardBundle) << pos.z;
			(*pForwardBundle) << dir.yaw();
			(*pForwardBundle) << dir.pitch();
			(*pForwardBundle) << dir.roll();
			ENTITY_MESSAGE_FORWARD_CLIENT_END(pForwardBundle, ClientInterface::onUpdateData_xyz_ypr, update);
		}
		break;
		default:
			KBE_ASSERT(false);
			break;
		};
	}
}

//-------------------------------------------------------------------------------------
uint32 Witness::getEntityVolatileDataUpdateFlags(Entity* otherEntity)
{
	uint32 flags = UPDATE_FLAG_NULL;

	/* ????????????????????¶À?®∞???????????
	   ????????????????entity????????????moveToPoint()??????????
	         ???????????ßÿ?????????????????????????????
	*/
	if (otherEntity->controlledBy() && pEntity_->id() == otherEntity->controlledBy()->id())
		return flags;

	const VolatileInfo* pVolatileInfo = otherEntity->pCustomVolatileinfo();
	if (!pVolatileInfo)
		pVolatileInfo = otherEntity->pScriptModule()->getPVolatileInfo();

	static uint16 entity_posdir_additional_updates = g_kbeSrvConfig.getCellApp().entity_posdir_additional_updates;
	
	if ((pVolatileInfo->position() > 0.f) && (entity_posdir_additional_updates == 0 || g_kbetime - otherEntity->posChangedTime() < entity_posdir_additional_updates))
	{
		if (!otherEntity->isOnGround() || !pVolatileInfo->optimized())
		{
			flags |= UPDATE_FLAG_XYZ; 
		}
		else
		{
			flags |= UPDATE_FLAG_XZ; 
		}
	}

	if((entity_posdir_additional_updates == 0) || (g_kbetime - otherEntity->dirChangedTime() < entity_posdir_additional_updates))
	{
		if (pVolatileInfo->yaw() > 0.f)
		{
			if (pVolatileInfo->roll() > 0.f)
			{
				if (pVolatileInfo->pitch() > 0.f)
				{
					flags |= UPDATE_FLAG_YAW_PITCH_ROLL;
				}
				else
				{
					flags |= UPDATE_FLAG_YAW_ROLL;
				}
			}
			else if (pVolatileInfo->pitch() > 0.f)
			{
				flags |= UPDATE_FLAG_YAW_PITCH;
			}
			else
			{
				flags |= UPDATE_FLAG_YAW;
			}
		}
		else if (pVolatileInfo->roll() > 0.f)
		{
			if (pVolatileInfo->pitch() > 0.f)
			{
				flags |= UPDATE_FLAG_PITCH_ROLL;
			}
			else
			{
				flags |= UPDATE_FLAG_ROLL;
			}
		}
		else if (pVolatileInfo->pitch() > 0.f)
		{
			flags |= UPDATE_FLAG_PITCH; 
		}
	}

	return flags;
}

//-------------------------------------------------------------------------------------
bool Witness::sendToClient(const Network::MessageHandler& msgHandler, Network::Bundle* pBundle)
{
	if(pushBundle(pBundle))
		return true;

	ERROR_MSG(fmt::format("Witness::sendToClient: {} pBundles is NULL, not found channel.\n", pEntity_->id()));
	Network::Bundle::reclaimPoolObject(pBundle);
	return false;
}

//-------------------------------------------------------------------------------------
}
