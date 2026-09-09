// Copyright 2008-2018 Yolo Technologies, Inc. All Rights Reserved. https://www.comblockengine.com


#include "components.h"
#include "helper/debug_helper.h"
#include "helper/sys_info.h"
#include "network/channel.h"	
#include "network/address.h"	
#include "network/bundle.h"	
#include "network/udp_packet.h"
#include "network/tcp_packet.h"
#include "network/network_interface.h"
#include "client_lib/client_interface.h"
#include "server/serverconfig.h"

#include "../../server/baseappmgr/baseappmgr_interface.h"
#include "../../server/cellappmgr/cellappmgr_interface.h"
#include "../../server/baseapp/baseapp_interface.h"
#include "../../server/cellapp/cellapp_interface.h"
#include "../../server/dbmgr/dbmgr_interface.h"
#include "../../server/loginapp/loginapp_interface.h"
#include "../../server/tools/logger/logger_interface.h"
#include "../../server/tools/bots/bots_interface.h"
#include "../../server/tools/interfaces/interfaces_interface.h"

// 注: components 的注册/查询/续租已全部走 cluster，不再引用 machine 消息集。
// (updateComponentInfos 等遗留的 machine 本地探活实现待 machine 进程摘除时一并删除)
#include "../../server/cluster/cluster_interface.h"
#include "network/endpoint.h"

#if KBE_PLATFORM != PLATFORM_WIN32
#include <sys/socket.h>
#include <sys/select.h>
#include <arpa/inet.h>
#include <signal.h>
#include <errno.h>
#endif

namespace KBEngine
{
namespace
{
//-------------------------------------------------------------------------------------
// cluster 服务面 TCP 客户端(短连接请求/应答)。
// 每条消息发送到 cluster 副本的 servicePort，若应答为重定向(NOT_LEADER)
// 则自动跟随到当前 leader，上层只感知"是否取回一帧应答"。
//-------------------------------------------------------------------------------------

bool clusterSelectWait(Network::EndPoint& ep, bool writable, uint32 timeoutMS)
{
	fd_set fds;
	FD_ZERO(&fds);
	FD_SET((int)ep, &fds);
	struct timeval tv;
	tv.tv_sec = (long)(timeoutMS / 1000);
	tv.tv_usec = (long)(timeoutMS % 1000) * 1000;

	if(writable)
		return select((int)ep + 1, NULL, &fds, NULL, &tv) > 0;

	return select((int)ep + 1, &fds, NULL, NULL, &tv) > 0;
}

int clusterRoundTrip(const std::string& requestMsg, std::string& response, uint32 ip, uint16 port, uint32 timeoutMS)
{
	Network::EndPoint ep;
	ep.socket(SOCK_STREAM);
	if(!ep.good())
		return -1;

	ep.setnonblocking(true);

	// EndPoint::connect 要求端口为网络字节序(见 endpoint.inl: sin_port = networkPort)
	if(ep.connect(htons(port), ip) == -1)
	{
		if(!clusterSelectWait(ep, true, timeoutMS))
		{
			ep.close();
			return -1;
		}
	}

	ep.setnodelay(true);

	std::string frame;
	ClusterInterface::Wire::putU32(frame, (uint32_t)requestMsg.size());
	frame += requestMsg;

	size_t sent = 0;
	while(sent < frame.size())
	{
		int n = ep.send(frame.c_str() + sent, (int)(frame.size() - sent));
		if(n > 0)
		{
			sent += (size_t)n;
			continue;
		}

		if(n < 0)
		{
			if(!clusterSelectWait(ep, true, 50))
			{
				ep.close();
				return -1;
			}
			continue;
		}

		ep.close();
		return -1;
	}

	char lenbuf[4];
	size_t got = 0;
	while(got < 4)
	{
		int n = ep.recv(lenbuf + got, 4 - (int)got);
		if(n > 0) { got += (size_t)n; continue; }
		if(n == 0) { ep.close(); return -1; }
		if(!clusterSelectWait(ep, false, 100)) { ep.close(); return -1; }
	}

	uint32_t plen = ((uint32_t)(uint8_t)lenbuf[0] << 24) | ((uint32_t)(uint8_t)lenbuf[1] << 16) |
		((uint32_t)(uint8_t)lenbuf[2] << 8) | (uint32_t)(uint8_t)lenbuf[3];
	if(plen == 0 || plen > 8 * 1024 * 1024)
	{
		ep.close();
		return -1;
	}

	response.assign(plen, '\0');
	got = 0;
	while(got < plen)
	{
		int n = ep.recv(&response[0] + got, (int)(plen - got));
		if(n > 0) { got += (size_t)n; continue; }
		if(n == 0) { ep.close(); return -1; }
		if(!clusterSelectWait(ep, false, 100)) { ep.close(); return -1; }
	}

	ep.close();
	return 0;
}

// 完整请求：自动重定向到 leader。返回 0 且 resp 非空表示取回了服务端应答。
int clusterRequest(const std::string& msg, std::string& resp, uint32 timeoutMS)
{
	const ENGINE_COMPONENT_INFO& cinfos = ServerConfig::getSingleton().getKCluster();

	uint16 servicePort = cinfos.clusterServicePort;
	std::vector<std::string> addrs = cinfos.cluster_addresses;
	if(addrs.size() == 0)
		addrs.push_back(std::string("127.0.0.1"));

	for(size_t i = 0; i < addrs.size(); ++i)
	{
		uint32 ip = inet_addr(addrs[i].c_str());
		if(ip == (uint32)INADDR_NONE)
			continue;

		for(int redirect = 0; redirect < 4; ++redirect)
		{
			resp.clear();
			if(clusterRoundTrip(msg, resp, ip, servicePort, timeoutMS) != 0)
				break;	// 该副本连不上，换下一个地址

			if(resp.size() < 1)
				return -1;

			uint8_t msgType = (uint8_t)resp[0];
			if(msgType == ClusterInterface::MSG_RESP_NOT_LEADER)
			{
				size_t off = 1;
				uint32_t lip = 0;
				uint16_t lp = 0;
				if(!ClusterInterface::Wire::getU32(resp.data(), resp.size(), off, lip))
					return -1;
				if(!ClusterInterface::Wire::getU16(resp.data(), resp.size(), off, lp))
					return -1;
				if(lip == 0)
					return -1;	// 集群选举中尚无 leader，交由上层稍后重试
				ip = lip;
				servicePort = lp;
				continue;
			}

			return 0;
		}
	}

	return -1;
}

// 检查本机 pid 进程是否存活(仅在冲突体与本机同机时使用)
bool clusterIsProcessRunning(uint32 pid)
{
	if(pid == 0)
		return true;

#if KBE_PLATFORM == PLATFORM_WIN32
	HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
	if(h == NULL)
		return GetLastError() == ERROR_ACCESS_DENIED;	// 权限不足视为存活
	CloseHandle(h);
	return true;
#else
	return kill((pid_t)pid, 0) == 0 || errno == EPERM;
#endif
}

// 将 cluster 返回的组件信息加入本地组件表(与machine回复解析行为一致)
void clusterAddFoundComponent(const ClusterInterface::ComponentData& cd)
{
	std::string extaddrEx = cd.extaddrEx;

	Components::getSingleton().addComponent(cd.uid, cd.username.c_str(),
		(KBEngine::COMPONENT_TYPE)cd.componentType, (COMPONENT_ID)cd.componentID,
		cd.globalOrder, cd.groupOrder, cd.gus,
		cd.intaddr, cd.intport, cd.extaddr, cd.extport, extaddrEx, cd.pid,
		cd.cpu, cd.mem, cd.usedmem,
		cd.extradata[0], cd.extradata[1], cd.extradata[2], cd.extradata[3]);
}

std::string clusterFindPayload(int32 uid, int32 componentType, COMPONENT_ID cid)
{
	std::string payload;
	ClusterInterface::Wire::putI32(payload, uid);
	ClusterInterface::Wire::putI32(payload, componentType);
	ClusterInterface::Wire::putU64(payload, (uint64_t)cid);
	return payload;
}

// 本组件在集群中的注册状态
bool clusterRegistered = false;
uint64 clusterLastRegisterMS = 0;
uint64 clusterLastRenewMS = 0;
uint64 clusterLastTickMS = 0;

} // anon namespace

int32 Components::ANY_UID = -1;

KBE_SINGLETON_INIT(Components);
Components _g_components;

//-------------------------------------------------------------------------------------
Components::Components():
Task(),
_baseapps(),
_cellapps(),
_dbmgrs(),
_loginapps(),
_cellappmgrs(),
_baseappmgrs(),
_machines(),
_loggers(),
_interfaceses(),
_bots(),
_consoles(),
_pNetworkInterface(NULL),
_globalOrderLog(),
_baseappGrouplOrderLog(),
_cellappGrouplOrderLog(),
_loginappGrouplOrderLog(),
_pHandler(NULL),
componentType_(UNKNOWN_COMPONENT_TYPE),
componentID_(0),
state_(0),
findIdx_(0),
extraData1_(0),
extraData2_(0),
extraData3_(0),
extraData4_(0)
{
}

//-------------------------------------------------------------------------------------
Components::~Components()
{
}

//-------------------------------------------------------------------------------------
void Components::initialize(Network::NetworkInterface * pNetworkInterface, COMPONENT_TYPE componentType, COMPONENT_ID componentID)
{ 
	KBE_ASSERT(pNetworkInterface != NULL); 
	_pNetworkInterface = pNetworkInterface; 

	componentType_ = componentType;
	componentID_ = componentID;

	for(uint8 i=0; i<8; ++i)
		findComponentTypes_[i] = UNKNOWN_COMPONENT_TYPE;

	switch(componentType_)
	{
	case CELLAPP_TYPE:
		findComponentTypes_[0] = LOGGER_TYPE;
		findComponentTypes_[1] = DBMGR_TYPE;
		findComponentTypes_[2] = CELLAPPMGR_TYPE;
		findComponentTypes_[3] = BASEAPPMGR_TYPE;
		break;
	case BASEAPP_TYPE:
		findComponentTypes_[0] = LOGGER_TYPE;
		findComponentTypes_[1] = DBMGR_TYPE;
		findComponentTypes_[2] = BASEAPPMGR_TYPE;
		findComponentTypes_[3] = CELLAPPMGR_TYPE;
		break;
	case BASEAPPMGR_TYPE:
		findComponentTypes_[0] = LOGGER_TYPE;
		findComponentTypes_[1] = DBMGR_TYPE;
		findComponentTypes_[2] = CELLAPPMGR_TYPE;
		break;
	case CELLAPPMGR_TYPE:
		findComponentTypes_[0] = LOGGER_TYPE;
		findComponentTypes_[1] = DBMGR_TYPE;
		findComponentTypes_[2] = BASEAPPMGR_TYPE;
		break;
	case LOGINAPP_TYPE:
		findComponentTypes_[0] = LOGGER_TYPE;
		findComponentTypes_[1] = DBMGR_TYPE;
		findComponentTypes_[2] = BASEAPPMGR_TYPE;
		break;
	case DBMGR_TYPE:
		findComponentTypes_[0] = LOGGER_TYPE;
		break;
	default:
		if(componentType_ != LOGGER_TYPE && 
			componentType_ != MACHINE_TYPE && 
			componentType_ != CLUSTER_TYPE &&
			componentType_ != INTERFACES_TYPE)
			findComponentTypes_[0] = LOGGER_TYPE;
		break;
	};
}

//-------------------------------------------------------------------------------------
void Components::finalise()
{
	clear(0, false);

	// 优雅退出: 通知cluster移除自己的注册(不需要应答)
	if(clusterRegistered)
	{
		std::string payload;
		ClusterInterface::Wire::putI32(payload, getUserUID());
		ClusterInterface::Wire::putI32(payload, (int32_t)componentType_);
		ClusterInterface::Wire::putU64(payload, (uint64_t)componentID_);

		std::string resp;
		clusterRequest(ClusterInterface::encodeMessage(ClusterInterface::MSG_UNREGISTER, payload), resp, 800);
		clusterRegistered = false;
	}
}

//-------------------------------------------------------------------------------------
bool Components::checkComponents(int32 uid, COMPONENT_ID componentID, uint32 pid)
{
	if(componentID <= 0)
		return true;

	int idx = 0;

	while(true)
	{
		COMPONENT_TYPE ct = ALL_COMPONENT_TYPES[idx++];
		if(ct == UNKNOWN_COMPONENT_TYPE)
			break;

		ComponentInfos* cinfos = findComponent(ct, uid, componentID);
		if(cinfos != NULL)
		{
			if(cinfos->componentType != MACHINE_TYPE && cinfos->pid != 0 /* 等于0通常是预设， 这种情况我们先不作比较 */ && pid != cinfos->pid)
			{
				ERROR_MSG(fmt::format("Components::checkComponents: uid:{}, componentType={}, componentID:{} exist.\n",
					uid, COMPONENT_NAME_EX(ct), componentID));

				KBE_ASSERT(false && "Components::checkComponents: componentID exist.\n");
			}
			return false;
		}
	}

	return true;
}

//-------------------------------------------------------------------------------------		
void Components::addComponent(int32 uid, const char* username, 
			COMPONENT_TYPE componentType, COMPONENT_ID componentID, COMPONENT_ORDER globalorderid, COMPONENT_ORDER grouporderid, COMPONENT_GUS gus,
			uint32 intaddr, uint16 intport, uint32 extaddr, uint16 extport, std::string& extaddrEx, uint32 pid,
			float cpu, float mem, uint32 usedmem, uint64 extradata, uint64 extradata1, uint64 extradata2, uint64 extradata3,
			Network::Channel* pChannel)
{
	COMPONENTS& components = getComponents(componentType);

	if(!checkComponents(uid, componentID, pid))
		return;

	ComponentInfos* cinfos = findComponent(componentType, uid, componentID);
	if(cinfos != NULL)
	{
		WARNING_MSG(fmt::format("Components::addComponent[{}]: uid:{}, username:{}, "
			"componentType:{}, componentID:{} is exist!\n",
			COMPONENT_NAME_EX(componentType), uid, username, (int32)componentType, componentID));
		return;
	}
	
	// 如果该uid下没有已经运行的任何相关组件，那么重置计数器
	if (getGameSrvComponentsSize(uid) == 0)
	{
		_globalOrderLog[uid] = 0;
		_baseappGrouplOrderLog[uid] = 0;
		_cellappGrouplOrderLog[uid] = 0;
		_loginappGrouplOrderLog[uid] = 0;

		INFO_MSG(fmt::format("Components::addComponent: reset orderLog, uid={}!\n",
			uid));
	}

	ComponentInfos componentInfos;

	componentInfos.pIntAddr.reset(new Network::Address(intaddr, intport));
	componentInfos.pExtAddr.reset(new Network::Address(extaddr, extport));

	if(extaddrEx.size() > 0)
		strncpy(componentInfos.externalAddressEx, extaddrEx.c_str(), MAX_NAME);
	
	componentInfos.uid = uid;
	componentInfos.cid = componentID;
	componentInfos.pChannel = pChannel;
	componentInfos.componentType = componentType;
	componentInfos.groupOrderid = 1;
	componentInfos.globalOrderid = 1;

	componentInfos.mem = mem;
	componentInfos.cpu = cpu;
	componentInfos.usedmem = usedmem;
	componentInfos.extradata = extradata;
	componentInfos.extradata1 = extradata1;
	componentInfos.extradata2 = extradata2;
	componentInfos.extradata3 = extradata3;
	componentInfos.pid = pid;

	if(pChannel)
		pChannel->componentID(componentID);

	strncpy(componentInfos.username, username, MAX_NAME);

	_globalOrderLog[uid]++;

	switch(componentType)
	{
	case BASEAPP_TYPE:
		_baseappGrouplOrderLog[uid]++;
		componentInfos.groupOrderid = _baseappGrouplOrderLog[uid];
		break;
	case CELLAPP_TYPE:
		_cellappGrouplOrderLog[uid]++;
		componentInfos.groupOrderid = _cellappGrouplOrderLog[uid];
		break;
	case LOGINAPP_TYPE:
		_loginappGrouplOrderLog[uid]++;
		componentInfos.groupOrderid = _loginappGrouplOrderLog[uid];
		break;
	default:
		break;
	};
	
	if(grouporderid > 0)
		componentInfos.groupOrderid = grouporderid;

	if(globalorderid > 0)
		componentInfos.globalOrderid = globalorderid;
	else
		componentInfos.globalOrderid = _globalOrderLog[uid];

	componentInfos.gus = gus;

	if(cinfos == NULL)
		components.push_back(componentInfos);
	else
		*cinfos = componentInfos;

	INFO_MSG(fmt::format("Components::addComponent[{}], uid={}, "
		"componentID={}, globalorderid={}, grouporderid={}, totalcount={}\n",
			COMPONENT_NAME_EX(componentType), 
			uid,
			componentID, 
			((int32)componentInfos.globalOrderid),
			((int32)componentInfos.groupOrderid),
			components.size()));
	
	if(_pHandler)
		_pHandler->onAddComponent(&componentInfos);
}

//-------------------------------------------------------------------------------------		
void Components::delComponent(int32 uid, COMPONENT_TYPE componentType, 
							  COMPONENT_ID componentID, bool ignoreComponentID, bool shouldShowLog)
{
	COMPONENTS& components = getComponents(componentType);
	COMPONENTS::iterator iter = components.begin();
	for(; iter != components.end();)
	{
		if((uid < 0 || (*iter).uid == uid) && (ignoreComponentID == true || (*iter).cid == componentID))
		{
			INFO_MSG(fmt::format("Components::delComponent[{}] componentID={}, component:totalcount={}.\n", 
				COMPONENT_NAME_EX(componentType), componentID, components.size()));

			ComponentInfos* componentInfos = &(*iter);

			//SAFE_RELEASE((*iter).pIntAddr);
			//SAFE_RELEASE((*iter).pExtAddr);
			//(*iter).pChannel->decRef();

			if(_pHandler)
				_pHandler->onRemoveComponent(componentInfos);

			iter = components.erase(iter);
			if(!ignoreComponentID)
				return;
		}
		else
			iter++;
	}

	if(shouldShowLog)
	{
		ERROR_MSG(fmt::format("Components::delComponent::not found [{}] component:totalcount:{}\n", 
			COMPONENT_NAME_EX(componentType), components.size()));
	}
}

//-------------------------------------------------------------------------------------		
void Components::removeComponentByChannel(Network::Channel * pChannel, bool isShutingdown)
{
	int ifind = 0;
	while(ALL_COMPONENT_TYPES[ifind] != UNKNOWN_COMPONENT_TYPE)
	{
		COMPONENT_TYPE componentType = ALL_COMPONENT_TYPES[ifind++];
		COMPONENTS& components = getComponents(componentType);
		COMPONENTS::iterator iter = components.begin();

		for(; iter != components.end();)
		{
			if((*iter).pChannel == pChannel)
			{
				//SAFE_RELEASE((*iter).pIntAddr);
				//SAFE_RELEASE((*iter).pExtAddr);
				// (*iter).pChannel->decRef();

				if (!isShutingdown && g_componentType != LOGGER_TYPE && g_componentType != INTERFACES_TYPE)
				{
					ERROR_MSG(fmt::format("Components::removeComponentByChannel: {} : {}, Abnormal exit(reason={})! Channel(timestamp={}, lastReceivedTime={}, inactivityExceptionPeriod={})\n",
						COMPONENT_NAME_EX(componentType), (*iter).cid, pChannel->condemnReason(), timestamp(), pChannel->lastReceivedTime(), pChannel->inactivityExceptionPeriod()));

#if KBE_PLATFORM == PLATFORM_WIN32
					printf("[ERROR]: %s.\n", (fmt::format("Components::removeComponentByChannel: {} : {}, Abnormal exit(reason={})!\n",
						COMPONENT_NAME_EX(componentType), (*iter).cid, pChannel->condemnReason())).c_str());
#endif
				}
				else
				{
					INFO_MSG(fmt::format("Components::removeComponentByChannel: {} : {}, Normal exit!\n",
						COMPONENT_NAME_EX(componentType), (*iter).cid));
				}

				ComponentInfos* componentInfos = &(*iter);

				if(_pHandler)
					_pHandler->onRemoveComponent(componentInfos);

				iter = components.erase(iter);
				return;
			}
			else
				iter++;
		}
	}

	// KBE_ASSERT(false && "channel is not found!\n");
}

//-------------------------------------------------------------------------------------		
int Components::connectComponent(COMPONENT_TYPE componentType, int32 uid, COMPONENT_ID componentID, bool printlog)
{
	Components::ComponentInfos* pComponentInfos = findComponent(componentType, uid, componentID);
	if (pComponentInfos == NULL)
	{
		if (printlog)
		{
			ERROR_MSG(fmt::format("Components::connectComponent: not found componentType={}, uid={}, componentID={}!\n",
				COMPONENT_NAME_EX(componentType), uid, componentID));
		}

		return -1;
	}

	Network::EndPoint * pEndpoint = Network::EndPoint::createPoolObject(OBJECTPOOL_POINT);
	pEndpoint->socket(SOCK_STREAM);
	if (!pEndpoint->good())
	{
		if (printlog)
		{
			ERROR_MSG("Components::connectComponent: couldn't create a socket\n");
		}

		Network::EndPoint::reclaimPoolObject(pEndpoint);
		return -1;
	}

	int ret = -1;
	pEndpoint->setnonblocking(true);
	pEndpoint->addr(*pComponentInfos->pIntAddr);

	for (int itry = 0; itry < 3; ++itry)
	{
		fd_set	frds, fwds;
		struct timeval tv = { 0, 1000000 };

		FD_ZERO(&frds);
		FD_ZERO(&fwds);
		FD_SET((int)(*pEndpoint), &frds);
		FD_SET((int)(*pEndpoint), &fwds);

		if (pEndpoint->connect(pComponentInfos->pIntAddr->port, pComponentInfos->pIntAddr->ip) == -1)
		{
			int selgot = select((*pEndpoint) + 1, &frds, &fwds, NULL, &tv);
			if (selgot > 0)
			{
				if (FD_ISSET((*pEndpoint), &frds) || FD_ISSET((*pEndpoint), &fwds))
				{
					pEndpoint->connect(pComponentInfos->pIntAddr->port, pComponentInfos->pIntAddr->ip);

					int error = kbe_lasterror();

#if KBE_PLATFORM == PLATFORM_WIN32
					if (error == WSAEISCONN || error == 0)
#else
					if (error == EISCONN)
#endif
					{
						ret = 0;
						break;
					}
				}

				ret = -1;
			}
			else
			{
				ret = 0;
				break;
			}
		}
	}

	if(ret == 0)
	{
		Network::Channel* pChannel = Network::Channel::createPoolObject(OBJECTPOOL_POINT);
		bool ret = pChannel->initialize(*_pNetworkInterface, pEndpoint, Network::Channel::INTERNAL);
		if(!ret)
		{
			if (printlog)
			{
				ERROR_MSG(fmt::format("Components::connectComponent: initialize({}) is failed!\n",
					pChannel->c_str()));
			}

			pChannel->destroy();
			Network::Channel::reclaimPoolObject(pChannel);
			return -1;
		}

		pComponentInfos->pChannel = pChannel;
		pComponentInfos->pChannel->componentID(componentID);
		if(!_pNetworkInterface->registerChannel(pComponentInfos->pChannel))
		{
			if (printlog)
			{
				ERROR_MSG(fmt::format("Components::connectComponent: registerChannel({}) is failed!\n",
					pComponentInfos->pChannel->c_str()));
			}

			pComponentInfos->pChannel->destroy();
			Network::Channel::reclaimPoolObject(pComponentInfos->pChannel);

			// 此时不可强制释放内存，destroy中已经对其减引用
			// SAFE_RELEASE(pComponentInfos->pChannel);
			pComponentInfos->pChannel = NULL;
			return -1;
		}
		else
		{
			Network::Bundle* pBundle = Network::Bundle::createPoolObject(OBJECTPOOL_POINT);
			if(componentType == BASEAPPMGR_TYPE)
			{
				(*pBundle).newMessage(BaseappmgrInterface::onRegisterNewApp);
				
				BaseappmgrInterface::onRegisterNewAppArgs11::staticAddToBundle((*pBundle), getUserUID(), getUsername(), 
					componentType_, componentID_, 
					g_componentGlobalOrder, g_componentGroupOrder,
					_pNetworkInterface->intTcpAddr().ip, _pNetworkInterface->intTcpAddr().port,
					_pNetworkInterface->extTcpAddr().ip, _pNetworkInterface->extTcpAddr().port, g_kbeSrvConfig.getConfig().externalAddress);
			}
			else if(componentType == CELLAPPMGR_TYPE)
			{
				(*pBundle).newMessage(CellappmgrInterface::onRegisterNewApp);
				
				CellappmgrInterface::onRegisterNewAppArgs11::staticAddToBundle((*pBundle), getUserUID(), getUsername(), 
					componentType_, componentID_, 
					g_componentGlobalOrder, g_componentGroupOrder,
					_pNetworkInterface->intTcpAddr().ip, _pNetworkInterface->intTcpAddr().port,
					_pNetworkInterface->extTcpAddr().ip, _pNetworkInterface->extTcpAddr().port, g_kbeSrvConfig.getConfig().externalAddress);
			}
			else if(componentType == CELLAPP_TYPE)
			{
				(*pBundle).newMessage(CellappInterface::onRegisterNewApp);
				
				CellappInterface::onRegisterNewAppArgs11::staticAddToBundle((*pBundle), getUserUID(), getUsername(), 
					componentType_, componentID_, 
					g_componentGlobalOrder, g_componentGroupOrder,
						_pNetworkInterface->intTcpAddr().ip, _pNetworkInterface->intTcpAddr().port,
					_pNetworkInterface->extTcpAddr().ip, _pNetworkInterface->extTcpAddr().port, g_kbeSrvConfig.getConfig().externalAddress);
			}
			else if(componentType == BASEAPP_TYPE)
			{
				(*pBundle).newMessage(BaseappInterface::onRegisterNewApp);
				
				BaseappInterface::onRegisterNewAppArgs11::staticAddToBundle((*pBundle), getUserUID(), getUsername(), 
					componentType_, componentID_, 
					g_componentGlobalOrder, g_componentGroupOrder,
					_pNetworkInterface->intTcpAddr().ip, _pNetworkInterface->intTcpAddr().port,
					_pNetworkInterface->extTcpAddr().ip, _pNetworkInterface->extTcpAddr().port, g_kbeSrvConfig.getConfig().externalAddress);
			}
			else if(componentType == DBMGR_TYPE)
			{
				(*pBundle).newMessage(DbmgrInterface::onRegisterNewApp);
				
				DbmgrInterface::onRegisterNewAppArgs11::staticAddToBundle((*pBundle), getUserUID(), getUsername(), 
					componentType_, componentID_, 
					g_componentGlobalOrder, g_componentGroupOrder,
					_pNetworkInterface->intTcpAddr().ip, _pNetworkInterface->intTcpAddr().port,
					_pNetworkInterface->extTcpAddr().ip, _pNetworkInterface->extTcpAddr().port, g_kbeSrvConfig.getConfig().externalAddress);
			}
			else if(componentType == LOGGER_TYPE)
			{
				(*pBundle).newMessage(LoggerInterface::onRegisterNewApp);
				
				LoggerInterface::onRegisterNewAppArgs11::staticAddToBundle((*pBundle), getUserUID(), getUsername(), 
					componentType_, componentID_, 
					g_componentGlobalOrder, g_componentGroupOrder,
					_pNetworkInterface->intTcpAddr().ip, _pNetworkInterface->intTcpAddr().port,
					_pNetworkInterface->extTcpAddr().ip, _pNetworkInterface->extTcpAddr().port, g_kbeSrvConfig.getConfig().externalAddress);
			}
			else
			{
				KBE_ASSERT(false && "invalid componentType.\n");
			}

			pComponentInfos->pChannel->send(pBundle);
		}
	}
	else
	{
		if (printlog)
		{
			ERROR_MSG(fmt::format("Components::connectComponent: connect({}) is failed! {}.\n",
				pComponentInfos->pIntAddr->c_str(), kbe_strerror()));
		}

		Network::EndPoint::reclaimPoolObject(pEndpoint);
		return -1;
	}

	return ret;
}

//-------------------------------------------------------------------------------------		
void Components::clear(int32 uid, bool shouldShowLog)
{
	delComponent(uid, DBMGR_TYPE, uid, true, shouldShowLog);
	delComponent(uid, BASEAPPMGR_TYPE, uid, true, shouldShowLog);
	delComponent(uid, CELLAPPMGR_TYPE, uid, true, shouldShowLog);
	delComponent(uid, CELLAPP_TYPE, uid, true, shouldShowLog);
	delComponent(uid, BASEAPP_TYPE, uid, true, shouldShowLog);
	delComponent(uid, LOGINAPP_TYPE, uid, true, shouldShowLog);
	//delComponent(uid, LOGGER_TYPE, uid, true, shouldShowLog);
}

//-------------------------------------------------------------------------------------		
Components::COMPONENTS& Components::getComponents(COMPONENT_TYPE componentType)
{
	switch(componentType)
	{
	case DBMGR_TYPE:
		return _dbmgrs;
	case LOGINAPP_TYPE:
		return _loginapps;
	case BASEAPPMGR_TYPE:
		return _baseappmgrs;
	case CELLAPPMGR_TYPE:
		return _cellappmgrs;
	case CELLAPP_TYPE:
		return _cellapps;
	case BASEAPP_TYPE:
		return _baseapps;
	case MACHINE_TYPE:
		return _machines;
	case LOGGER_TYPE:
		return _loggers;			
	case INTERFACES_TYPE:
		return _interfaceses;	
	case BOTS_TYPE:
		return _bots;	
	default:
		break;
	};

	return _consoles;
}

//-------------------------------------------------------------------------------------		
Components::ComponentInfos* Components::findComponent(COMPONENT_TYPE componentType, int32 uid,
																			COMPONENT_ID componentID)
{
	COMPONENTS& components = getComponents(componentType);
	COMPONENTS::iterator iter = components.begin();
	for(; iter != components.end(); ++iter)
	{
		if((*iter).uid == uid && (componentID == 0 || (*iter).cid == componentID))
			return &(*iter);
	}

	return NULL;
}

//-------------------------------------------------------------------------------------		
Components::ComponentInfos* Components::findComponent(COMPONENT_TYPE componentType, COMPONENT_ID componentID)
{
	COMPONENTS& components = getComponents(componentType);
	COMPONENTS::iterator iter = components.begin();
	for(; iter != components.end(); ++iter)
	{
		if(componentID == 0 || (*iter).cid == componentID)
			return &(*iter);
	}

	return NULL;
}

//-------------------------------------------------------------------------------------		
Components::ComponentInfos* Components::findComponent(COMPONENT_ID componentID)
{
	int idx = 0;
	int32 uid = getUserUID();

	while(true)
	{
		COMPONENT_TYPE ct = ALL_COMPONENT_TYPES[idx++];
		if(ct == UNKNOWN_COMPONENT_TYPE)
			break;

		ComponentInfos* cinfos = findComponent(ct, uid, componentID);
		if(cinfos != NULL)
		{
			return cinfos;
		}
	}

	return NULL;
}

//-------------------------------------------------------------------------------------		
Components::ComponentInfos* Components::findComponent(Network::Channel * pChannel)
{
	int ifind = 0;

	while(ALL_COMPONENT_TYPES[ifind] != UNKNOWN_COMPONENT_TYPE)
	{
		COMPONENT_TYPE componentType = ALL_COMPONENT_TYPES[ifind++];
		COMPONENTS& components = getComponents(componentType);
		COMPONENTS::iterator iter = components.begin();

		for(; iter != components.end(); ++iter)
		{
			if((*iter).pChannel == pChannel)
			{
				return &(*iter);
			}
		}
	}

	return NULL;
}

//-------------------------------------------------------------------------------------		
Components::ComponentInfos* Components::findComponent(Network::Address* pAddress)
{
	int ifind = 0;

	while(ALL_COMPONENT_TYPES[ifind] != UNKNOWN_COMPONENT_TYPE)
	{
		COMPONENT_TYPE componentType = ALL_COMPONENT_TYPES[ifind++];
		COMPONENTS& components = getComponents(componentType);
		COMPONENTS::iterator iter = components.begin();

		for(; iter != components.end(); ++iter)
		{
			if((*iter).pChannel && (*iter).pChannel->addr() == *pAddress)
			{
				return &(*iter);
			}
		}
	}

	return NULL;
}

//-------------------------------------------------------------------------------------		
Components::ComponentInfos* Components::findLocalComponent(uint32 pid)
{
	int ifind = 0;
	while(ALL_COMPONENT_TYPES[ifind] != UNKNOWN_COMPONENT_TYPE)
	{
		COMPONENT_TYPE componentType = ALL_COMPONENT_TYPES[ifind++];
		COMPONENTS& components = getComponents(componentType);
		COMPONENTS::iterator iter = components.begin();

		for(; iter != components.end(); ++iter)
		{
			if(isLocalComponent(&(*iter)) && (*iter).pid == pid)
			{
				return &(*iter);
			}
		}
	}

	return NULL;
}

//-------------------------------------------------------------------------------------		
bool Components::isLocalComponent(const Components::ComponentInfos* info)
{
	return _pNetworkInterface->intTcpAddr().ip == info->pIntAddr->ip ||
			_pNetworkInterface->extTcpAddr().ip == info->pIntAddr->ip;
}

//-------------------------------------------------------------------------------------		
const Components::ComponentInfos* Components::lookupLocalComponentRunning(uint32 pid)
{
	if(pid > 0)
	{
		SystemInfo::PROCESS_INFOS sysinfos = SystemInfo::getSingleton().getProcessInfo(pid);
		if(sysinfos.error)
		{
			return NULL;
		}
		else
		{
			Components::ComponentInfos* winfo = findLocalComponent(pid);

			if(winfo)
			{
				winfo->cpu = sysinfos.cpu;
				winfo->usedmem = (uint32)sysinfos.memused;

				winfo->mem = float((winfo->usedmem * 1.0 / SystemInfo::getSingleton().totalmem()) * 100.0);
			}

			return winfo;
		}
	}

	return NULL;
}

//-------------------------------------------------------------------------------------		
bool Components::updateComponentInfos(const Components::ComponentInfos* info)
{
	// 不对其他machine做处理
	if(info->componentType == MACHINE_TYPE)
	{
		return true;
	}

	if (!lookupLocalComponentRunning(info->pid))
		return false;

	Network::EndPoint epListen;
	epListen.socket(SOCK_STREAM);
	if (!epListen.good())
	{
		ERROR_MSG("Components::updateComponentInfos: couldn't create a socket\n");
		return true;
	}
	
	epListen.setnonblocking(true);

	while(true)
	{
		fd_set	frds, fwds;
		struct timeval tv = { 0, 300000 }; // 100ms

		FD_ZERO( &frds );
		FD_ZERO( &fwds );
		FD_SET((int)epListen, &frds);
		FD_SET((int)epListen, &fwds);

		if(epListen.connect(info->pIntAddr->port, info->pIntAddr->ip) == -1)
		{
			int selgot = select(epListen+1, &frds, &fwds, NULL, &tv);
			if(selgot > 0)
			{
				break;
			}

			WARNING_MSG(fmt::format("Components::updateComponentInfos: couldn't connect to:{}\n", 
				info->pIntAddr->c_str()));

			return false;
		}
		else
		{
			break;
		}
	}
	
	epListen.setnodelay(true);

	Network::Bundle* pBundle = Network::Bundle::createPoolObject(OBJECTPOOL_POINT);

	// 由于COMMON_NETWORK_MESSAGE不包含client， 如果是bots， 我们需要单独处理
	if(info->componentType != BOTS_TYPE)
	{
		COMMON_NETWORK_MESSAGE(info->componentType, (*pBundle), lookApp);
	}
	else
	{
		(*pBundle).newMessage(BotsInterface::lookApp);
	}

	epListen.send(pBundle->pCurrPacket()->data(), pBundle->pCurrPacket()->wpos());
	Network::Bundle::reclaimPoolObject(pBundle);

	fd_set	fds;
	struct timeval tv = { 0, 300000 }; // 100ms

	FD_ZERO( &fds );
	FD_SET((int)epListen, &fds);

	int selgot = select(epListen+1, &fds, NULL, NULL, &tv);
	if(selgot == 0)
	{
		// 超时, 可能对方繁忙
		return true;	
	}
	else if(selgot == -1)
	{
		return true;
	}
	else
	{
		COMPONENT_TYPE ctype;
		COMPONENT_ID cid;
		int8 istate = 0;
		ArraySize entitySize = 0, cellSize = 0;
		int32 clientsSize = 0, proxicesSize = 0;
		uint32 telnet_port = 0;

		Network::TCPPacket packet;
		packet.resize(255);
		int recvsize = sizeof(ctype) + sizeof(cid) + sizeof(istate);

		if(info->componentType == CELLAPP_TYPE)
		{
			recvsize += sizeof(entitySize) + sizeof(cellSize) + sizeof(telnet_port);
		}

		if(info->componentType == BASEAPP_TYPE)
		{
			recvsize += sizeof(entitySize) + sizeof(clientsSize) + sizeof(proxicesSize) + sizeof(telnet_port);
		}

		int len = epListen.recv(packet.data(), recvsize);
		packet.wpos(len);
		
		if(recvsize != len)
		{
			WARNING_MSG(fmt::format("Components::updateComponentInfos: packet invalid(recvsize({}) != ctype_cid_len({}).\n" 
				, len, recvsize));
			
			if(len == 0)
				return false;

			return true;
		}

		packet >> ctype >> cid >> istate;
		
		if(ctype == CELLAPP_TYPE)
		{
			packet >> entitySize >> cellSize >> telnet_port;
		}

		if(ctype == BASEAPP_TYPE)
		{
			packet >> entitySize >> clientsSize >> proxicesSize >> telnet_port;
		}

		if(ctype != info->componentType || cid != info->cid)
		{
			WARNING_MSG(fmt::format("Components::updateComponentInfos: invalid component(ctype={}, cid={}).\n",
				ctype, cid));

			return false;
		}

		Components::ComponentInfos* winfo = findComponent(info->componentType, info->cid);
		if(winfo)
		{
			winfo->state = (COMPONENT_STATE)istate;

			if(ctype == CELLAPP_TYPE)
			{
				winfo->extradata = entitySize;
				winfo->extradata1 = cellSize;
				winfo->extradata3 = telnet_port;
			}
			else if(ctype == BASEAPP_TYPE)
			{
				winfo->extradata = entitySize;
				winfo->extradata1 = clientsSize;
				winfo->extradata2 = proxicesSize;
				winfo->extradata3 = telnet_port;
			}
		}
	}

	return true;
}

//-------------------------------------------------------------------------------------		
Components::ComponentInfos* Components::getBaseappmgr()
{
	return findComponent(BASEAPPMGR_TYPE, getUserUID(), 0);
}

//-------------------------------------------------------------------------------------		
Components::ComponentInfos* Components::getCellappmgr()
{
	return findComponent(CELLAPPMGR_TYPE, getUserUID(), 0);
}

//-------------------------------------------------------------------------------------		
Components::ComponentInfos* Components::getDbmgr()
{
	return findComponent(DBMGR_TYPE, getUserUID(), 0);
}

//-------------------------------------------------------------------------------------		
Components::ComponentInfos* Components::getLogger()
{
	return findComponent(LOGGER_TYPE, getUserUID(), 0);
}

//-------------------------------------------------------------------------------------		
Components::ComponentInfos* Components::getInterfaceses()
{
	return findComponent(INTERFACES_TYPE, getUserUID(), 0);
}

//-------------------------------------------------------------------------------------		
Network::Channel* Components::getBaseappmgrChannel()
{
	Components::ComponentInfos* cinfo = getBaseappmgr();
	if(cinfo == NULL)
		 return NULL;

	return cinfo->pChannel;
}

//-------------------------------------------------------------------------------------		
Network::Channel* Components::getCellappmgrChannel()
{
	Components::ComponentInfos* cinfo = getCellappmgr();
	if(cinfo == NULL)
		 return NULL;

	return cinfo->pChannel;
}

//-------------------------------------------------------------------------------------		
Network::Channel* Components::getDbmgrChannel()
{
	Components::ComponentInfos* cinfo = getDbmgr();
	if(cinfo == NULL)
		 return NULL;

	return cinfo->pChannel;
}

//-------------------------------------------------------------------------------------		
Network::Channel* Components::getLoggerChannel()
{
	Components::ComponentInfos* cinfo = getLogger();
	if(cinfo == NULL)
		 return NULL;

	return cinfo->pChannel;
}

//-------------------------------------------------------------------------------------	
size_t Components::getGameSrvComponentsSize(int32 uid)
{
	size_t size = 0;

	COMPONENTS::iterator iter = _baseapps.begin();
	for (; iter != _baseapps.end(); ++iter)
	{
		if ((*iter).uid == uid)
			++size;
	}

	iter = _baseappmgrs.begin();
	for (; iter != _baseappmgrs.end(); ++iter)
	{
		if ((*iter).uid == uid)
			++size;
	}

	iter = _cellapps.begin();
	for (; iter != _cellapps.end(); ++iter)
	{
		if ((*iter).uid == uid)
			++size;
	}

	iter = _cellappmgrs.begin();
	for (; iter != _cellappmgrs.end(); ++iter)
	{
		if ((*iter).uid == uid)
			++size;
	}

	iter = _dbmgrs.begin();
	for (; iter != _dbmgrs.end(); ++iter)
	{
		if ((*iter).uid == uid)
			++size;
	}

	iter = _loginapps.begin();
	for (; iter != _loginapps.end(); ++iter)
	{
		if ((*iter).uid == uid)
			++size;
	}

	return size;
}

//-------------------------------------------------------------------------------------	
size_t Components::getGameSrvComponentsSize()
{
	return _baseapps.size() + _cellapps.size() + _dbmgrs.size() + 
		_loginapps.size() + _cellappmgrs.size() + _baseappmgrs.size();
}

//-------------------------------------------------------------------------------------
Network::EventDispatcher & Components::dispatcher()
{
	return pNetworkInterface()->dispatcher();
}

//-------------------------------------------------------------------------------------
void Components::onChannelDeregister(Network::Channel * pChannel, bool isShutingdown)
{
	removeComponentByChannel(pChannel, isShutingdown);
}

//-------------------------------------------------------------------------------------
bool Components::findLogger()
{
	if (g_componentType == LOGGER_TYPE || g_componentType == MACHINE_TYPE || g_componentType == CLUSTER_TYPE ||
		g_componentType == TOOL_TYPE || g_componentType == CONSOLE_TYPE || g_componentType == CLIENT_TYPE || g_componentType == BOTS_TYPE ||
		g_componentType == WATCHER_TYPE || componentType_ == INTERFACES_TYPE)
	{
		DebugHelper::getSingleton().onNoLogger();
		return true;
	}
	
	// 从cluster注册表中查找logger并连接
	int retryCount = 0;
	while(retryCount++ < 5)
	{
		std::string resp;
		if(clusterRequest(ClusterInterface::encodeMessage(ClusterInterface::MSG_FIND,
				clusterFindPayload(getUserUID(), (int32)LOGGER_TYPE, 0)), resp, 800) == 0 &&
			resp.size() > 0 && (uint8_t)resp[0] == ClusterInterface::MSG_RESP_ENTRY_LIST)
		{
			size_t off = 1;
			uint16_t count16 = 0;
			bool found = false;

			if(ClusterInterface::Wire::getU16(resp.data(), resp.size(), off, count16))
			{
				for(uint16_t i = 0; i < count16; ++i)
				{
					ClusterInterface::ComponentData cd;
					if(!ClusterInterface::decodeComponentData(resp.data(), resp.size(), off, cd))
						break;

					if((int32)cd.componentType != (int32)LOGGER_TYPE)
						continue;

					INFO_MSG(fmt::format("Components::findLogger: found {}, addr:{}:{}\n",
						COMPONENT_NAME_EX((COMPONENT_TYPE)cd.componentType),
						inet_ntoa((struct in_addr&)cd.intaddr),
						ntohs(cd.intport)));

					clusterAddFoundComponent(cd);
					found = true;
				}
			}

			if(found)
			{
				for(int iconn = 0; iconn < 5; iconn++)
				{
					if(connectComponent(LOGGER_TYPE, getUserUID(), 0, false) != 0)
					{
						KBEngine::sleep(200);
					}
					else
					{
						// 已连接logger, 从后续查找序列中移除logger
						for(size_t ic = 1; ic < sizeof(findComponentTypes_) - 1; ++ic)
						{
							findComponentTypes_[ic - 1] = findComponentTypes_[ic];
						}

						return true;
					}
				}
			}
		}

		// logger可能还在启动中, 等待后重试
		KBEngine::sleep(300);
	}

	return false;
}

//-------------------------------------------------------------------------------------
bool Components::findComponents()
{
	// 阶段1: 从cluster注册表按类型逐个发现组件
	if(state_ == 1)
	{
		while(findIdx_ < 8)
		{
			if(dispatcher().hasBreakProcessing() || dispatcher().waitingBreakProcessing())
				return false;

			COMPONENT_TYPE findComponentType = (COMPONENT_TYPE)findComponentTypes_[findIdx_];

			// 所有类型都处理完毕
			if(findComponentType == UNKNOWN_COMPONENT_TYPE)
			{
				state_ = 2;
				findIdx_ = 0;
				break;
			}

			// 已被跳过(辅助组件缺失)的类型
			if(findComponentType == (COMPONENT_TYPE)-1)
			{
				findIdx_++;
				continue;
			}

			// 该类型组件已经在本地表中(可能同一类型被查找到多次)
			if(getComponents(findComponentType).size() > 0)
			{
				findIdx_++;
				continue;
			}

			INFO_MSG(fmt::format("Components::findComponents: find {}...\n",
				COMPONENT_NAME_EX(findComponentType)));

			std::string resp;
			if(clusterRequest(ClusterInterface::encodeMessage(ClusterInterface::MSG_FIND,
					clusterFindPayload(getUserUID(), (int32)findComponentType, 0)), resp, 800) != 0 || resp.size() < 1)
			{
				// cluster暂不可用, 下轮重试
				return false;
			}

			uint8_t rt = (uint8_t)resp[0];
			bool foundOne = false;

			if(rt == ClusterInterface::MSG_RESP_ENTRY_LIST)
			{
				size_t off = 1;
				uint16_t count16 = 0;
				if(ClusterInterface::Wire::getU16(resp.data(), resp.size(), off, count16))
				{
					for(uint16_t i = 0; i < count16; ++i)
					{
						ClusterInterface::ComponentData cd;
						if(!ClusterInterface::decodeComponentData(resp.data(), resp.size(), off, cd))
							break;

						// 忽略自己
						if(cd.uid == getUserUID() && (COMPONENT_TYPE)cd.componentType == componentType_ &&
							(COMPONENT_ID)cd.componentID == componentID_)
						{
							continue;
						}

						if((int32)cd.componentType == (int32)findComponentType)
						{
							INFO_MSG(fmt::format("Components::findComponents: found {}, addr:{}:{}\n",
								COMPONENT_NAME_EX(findComponentType),
								inet_ntoa((struct in_addr&)cd.intaddr),
								ntohs(cd.intport)));

							clusterAddFoundComponent(cd);
							foundOne = true;
						}
					}
				}
			}
			else if(rt == ClusterInterface::MSG_RESP_ENTRY)
			{
				size_t off = 1;
				ClusterInterface::ComponentData cd;
				if(ClusterInterface::decodeComponentData(resp.data(), resp.size(), off, cd) &&
					(int32)cd.componentType == (int32)findComponentType &&
					!(cd.uid == getUserUID() && (COMPONENT_ID)cd.componentID == componentID_))
				{
					INFO_MSG(fmt::format("Components::findComponents: found {}, addr:{}:{}\n",
						COMPONENT_NAME_EX(findComponentType),
						inet_ntoa((struct in_addr&)cd.intaddr),
						ntohs(cd.intport)));

					clusterAddFoundComponent(cd);
					foundOne = true;
				}
			}

			if(foundOne)
			{
				// logger特例: 找到后立即连接, 以便尽早同步日志
				if(findComponentType == LOGGER_TYPE)
				{
					if(connectComponent(LOGGER_TYPE, getUserUID(), 0) == 0)
					{
						findComponentTypes_[findIdx_] = -1;
						findIdx_++;
						continue;
					}

					findComponentTypes_[findIdx_] = -1;
					findIdx_++;
					return false;
				}

				findIdx_++;
				continue;
			}

			// 该类型当前尚未注册到cluster
			// 辅助类组件(如logger/interfaces)缺失则跳过, 其它类型等待重试
			bool isHelper = false;
			for(int helperIdx = 0; ALL_HELPER_COMPONENT_TYPE[helperIdx] != UNKNOWN_COMPONENT_TYPE; ++helperIdx)
			{
				if(findComponentType == ALL_HELPER_COMPONENT_TYPE[helperIdx])
				{
					isHelper = true;
					break;
				}
			}

			if(isHelper)
			{
				WARNING_MSG(fmt::format("Components::findComponents: not found {}!\n",
					COMPONENT_NAME_EX(findComponentType)));

				findComponentTypes_[findIdx_] = -1; // 跳过标志
				findIdx_++;
				continue;
			}

			// 需要的组件尚未出现, 保持继续等待
			return false;
		}
	}

	// 阶段2: 向已发现的组件注册自己
	if(state_ == 2)
	{
		while(findIdx_ < 8)
		{
			if(dispatcher().hasBreakProcessing())
				return false;

			int8 findComponentType = findComponentTypes_[findIdx_];
			if(findComponentType == UNKNOWN_COMPONENT_TYPE)
				break;

			if(findComponentType == -1)
			{
				findIdx_++;
				continue;
			}

			INFO_MSG(fmt::format("Components::findComponents: register self to {}...\n",
				COMPONENT_NAME_EX((COMPONENT_TYPE)findComponentType)));

			if(connectComponent(static_cast<COMPONENT_TYPE>(findComponentType), getUserUID(), 0) != 0)
			{
				ERROR_MSG(fmt::format("Components::findComponents: register self to {} error!\n",
					COMPONENT_NAME_EX((COMPONENT_TYPE)findComponentType)));
				return false;
			}

			findIdx_++;
		}

		return true;
	}

	return false;
}

//-------------------------------------------------------------------------------------
void Components::onFoundAllComponents()
{
	INFO_MSG("Components::process(): Found all the components!\n");

#if KBE_PLATFORM == PLATFORM_WIN32
		DebugHelper::getSingleton().set_normalcolor();
		printf("[INFO]: Found all the components!\n");
		DebugHelper::getSingleton().set_normalcolor();
#endif
}

//-------------------------------------------------------------------------------------
void Components::broadcastSelf()
{
	// 原实现: 向machine广播自身最新信息(startGlobalOrder/startGroupOrder等被dbmgr修正后)。
	// 集群模式下不再有machine, 改为向cluster重新提交一次自身注册信息(同pid视为更新)。
	// 供本机重新注册用, 与process状态0共用同一套ComponentData字段。
	if (dispatcher().hasBreakProcessing() || dispatcher().waitingBreakProcessing())
		return;

	ClusterInterface::ComponentData cd;
	cd.uid = getUserUID();
	cd.username = getUsername();
	cd.componentType = (int32_t)componentType_;
	cd.componentID = (uint64_t)componentID_;
	cd.componentIDEx = 0;
	cd.globalOrder = g_componentGlobalOrder;
	cd.groupOrder = g_componentGroupOrder;
	cd.gus = g_genuuid_sections > 0 ? (uint16_t)g_genuuid_sections : (uint16_t)0;
	cd.intaddr = pNetworkInterface()->intTcpAddr().ip;
	cd.intport = pNetworkInterface()->intTcpAddr().port;
	cd.extaddr = pNetworkInterface()->extTcpAddr().ip;
	cd.extport = pNetworkInterface()->extTcpAddr().port;
	cd.extaddrEx = g_kbeSrvConfig.getConfig().externalAddress;
	cd.pid = getProcessPID();
	cd.cpu = SystemInfo::getSingleton().getCPUPerByPID();
	cd.mem = 0.f;
	cd.usedmem = (uint32_t)SystemInfo::getSingleton().getMemUsedByPID();
	cd.state = 1;
	cd.machineID = (uint32_t)getMacMD5();
	cd.extradata[0] = extraData1_;
	cd.extradata[1] = extraData2_;
	cd.extradata[2] = extraData3_;
	cd.extradata[3] = extraData4_;

	std::string resp;
	if(clusterRequest(ClusterInterface::encodeMessage(ClusterInterface::MSG_REGISTER,
			ClusterInterface::encodeComponentData(cd)), resp, 600) != 0 || resp.size() < 1)
	{
		ERROR_MSG("Components::broadcastSelf: update self info to cluster error!\n");
	}
	else if((uint8_t)resp[0] == ClusterInterface::MSG_RESP_IDENTITY_CONFLICT)
	{
		// 说明本地出现了重复的进程身份
		size_t off = 1;
		ClusterInterface::ComponentData exist;
		if(ClusterInterface::decodeComponentData(resp.data(), resp.size(), off, exist))
		{
			ERROR_MSG(fmt::format("Components::broadcastSelf: found {}, addr:{}:{}\n",
				COMPONENT_NAME_EX((COMPONENT_TYPE)exist.componentType),
				inet_ntoa((struct in_addr&)exist.intaddr),
				ntohs(exist.intport)));

			if(_pHandler)
			{
				_pHandler->onIdentityillegal((COMPONENT_TYPE)exist.componentType,
					(COMPONENT_ID)exist.componentID, exist.pid,
					inet_ntoa((struct in_addr&)exist.intaddr));
			}
		}
	}

	// 只有真正取回OK应答才认为已注册, 否则保留原状态(后续由process重新注册)
	if(!clusterRegistered && resp.size() > 0 && (uint8_t)resp[0] == ClusterInterface::MSG_RESP_OK)
	{
		clusterRegistered = true;
	}
}

//-------------------------------------------------------------------------------------
bool Components::process()
{
	// machine/cluster 组件自身不参与集群注册与发现
	if(componentType_ == MACHINE_TYPE || componentType_ == CLUSTER_TYPE)
	{
		onFoundAllComponents();
		return false;
	}

	if(dispatcher().hasBreakProcessing() || dispatcher().waitingBreakProcessing())
		return false;

	uint64 now = timestamp();

	// 状态0: 注册到cluster。注册成功后才允许发现其它组件,
	// 保证本组件在集群中"可见"的时序与machine广播提交身份一致。
	if(state_ == 0)
	{
		if(clusterRegistered)
		{
			state_ = 1;
			findIdx_ = 0;
			return true;
		}

		if(clusterLastRegisterMS == 0 ||
			now - clusterLastRegisterMS >= uint64(stampsPerSecond() / 2))
		{
			clusterLastRegisterMS = now;

			DEBUG_MSG(fmt::format("Components::process: register self {}:{}(pid={}) to cluster...\n",
				COMPONENT_NAME_EX(componentType_), componentID_, getProcessPID()));

			ClusterInterface::ComponentData cd;
			cd.uid = getUserUID();
			cd.username = getUsername();
			cd.componentType = (int32_t)componentType_;
			cd.componentID = (uint64_t)componentID_;
			cd.componentIDEx = 0;
			cd.globalOrder = g_componentGlobalOrder;
			cd.groupOrder = g_componentGroupOrder;
			cd.gus = g_genuuid_sections > 0 ? (uint16_t)g_genuuid_sections : (uint16_t)0;
			cd.intaddr = pNetworkInterface()->intTcpAddr().ip;
			cd.intport = pNetworkInterface()->intTcpAddr().port;
			cd.extaddr = pNetworkInterface()->extTcpAddr().ip;
			cd.extport = pNetworkInterface()->extTcpAddr().port;
			cd.extaddrEx = g_kbeSrvConfig.getConfig().externalAddress;
			cd.pid = getProcessPID();
			cd.cpu = SystemInfo::getSingleton().getCPUPerByPID();
			cd.mem = 0.f;
			cd.usedmem = (uint32_t)SystemInfo::getSingleton().getMemUsedByPID();
			cd.state = 1;
			cd.machineID = (uint32_t)getMacMD5();
			cd.extradata[0] = extraData1_;
			cd.extradata[1] = extraData2_;
			cd.extradata[2] = extraData3_;
			cd.extradata[3] = extraData4_;

			std::string resp;
			if(clusterRequest(ClusterInterface::encodeMessage(ClusterInterface::MSG_REGISTER,
					ClusterInterface::encodeComponentData(cd)), resp, 600) == 0 && resp.size() > 0)
			{
				uint8_t rt = (uint8_t)resp[0];
				if(rt == ClusterInterface::MSG_RESP_OK)
				{
					INFO_MSG(fmt::format("Components::process: register self {}:{}(pid={}) success.\n",
						COMPONENT_NAME_EX(componentType_), componentID_, getProcessPID()));

					clusterRegistered = true;
					clusterLastRenewMS = timestamp();
					state_ = 1;
					findIdx_ = 0;
					return true;
				}
				else if(rt == ClusterInterface::MSG_RESP_IDENTITY_CONFLICT)
				{
					size_t off = 1;
					ClusterInterface::ComponentData exist;
					if(!ClusterInterface::decodeComponentData(resp.data(), resp.size(), off, exist))
					{
						ERROR_MSG("Components::process: register conflict, but conflict data invalid!\n");
						return false;
					}

					// 冲突体与本机同机、且其进程已经不存在时, 视为崩溃残留,
					// 等待cluster的租约(TTL)过期后被自动移除, 然后重新注册即可。
					if(exist.machineID == (uint32_t)getMacMD5() && !clusterIsProcessRunning(exist.pid))
					{
						DEBUG_MSG(fmt::format("Components::process: register self conflict with {} {}:{} pid={} (process not exist), "
							"wait lease expire then retry...\n",
							COMPONENT_NAME_EX((COMPONENT_TYPE)exist.componentType), exist.componentID,
							inet_ntoa((struct in_addr&)exist.intaddr), exist.pid));
						return true;
					}

					ERROR_MSG(fmt::format("Components::process: found {}, addr:{}:{}\n",
						COMPONENT_NAME_EX((COMPONENT_TYPE)exist.componentType),
						inet_ntoa((struct in_addr&)exist.intaddr),
						ntohs(exist.intport)));

					// 存在相同身份, 程序该退出了
					if(_pHandler)
					{
						_pHandler->onIdentityillegal((COMPONENT_TYPE)exist.componentType,
							(COMPONENT_ID)exist.componentID, exist.pid,
							inet_ntoa((struct in_addr&)exist.intaddr));
					}

					return false;
				}

				DEBUG_MSG(fmt::format("Components::process: register self return msgType={}.\n", (int)rt));
				return true;
			}

			// cluster未就绪(选举中或不可达), 半秒后继续重试
			if(!clusterRegistered)
			{
				DEBUG_MSG("Components::process: cluster not ready, will retry register self...\n");
			}
		}

		return true;
	}

	// 每1s推进: 发现其它组件并向其注册自己; 完成后按心跳间隔续租
	if(state_ == 1 || state_ == 2)
	{
		if(now - clusterLastTickMS >= uint64(stampsPerSecond()))
		{
			clusterLastTickMS = now;

			if(state_ != 2 && findComponents())
			{
				// findComponents返回true表示所有组件均已连接完成
				INFO_MSG("Components::process: found all components success!\n");
				onFoundAllComponents();
				state_ = 2;
				findIdx_ = 0;
			}

			// 租约续期(心跳间隔从<cluster>配置读取, 默认5s)。
			// 续期不依赖发现进度: 即使仍在等待某个组件上线, 也要持续续租,
			// 避免自身注册因TTL被清理。
			// (发现组件失败时findComponents返回false, 这里继续执行续租)
			uint32 heartbeatSec = (uint32_t)g_kbeSrvConfig.getKCluster().clusterComponentHeartbeatInterval;
			if(clusterRegistered && heartbeatSec > 0 &&
				now - clusterLastRenewMS >= uint64(stampsPerSecond()) * heartbeatSec)
			{
				clusterLastRenewMS = now;

				std::string payload;
				ClusterInterface::Wire::putI32(payload, getUserUID());
				ClusterInterface::Wire::putI32(payload, (int32_t)componentType_);
				ClusterInterface::Wire::putU64(payload, (uint64_t)componentID_);
				ClusterInterface::Wire::putU32(payload, getProcessPID());

				std::string resp;
				if(clusterRequest(ClusterInterface::encodeMessage(ClusterInterface::MSG_RENEW, payload), resp, 400) != 0)
				{
					DEBUG_MSG("Components::process: renew lease failed, will retry later.\n");
				}
				else if(resp.size() > 0 && (uint8_t)resp[0] == ClusterInterface::MSG_RESP_NOT_FOUND)
				{
					// 自身注册已丢失(例如leader切换/快照恢复), 回到状态0重新注册
					DEBUG_MSG("Components::process: self component not found in cluster, re-register.\n");
					clusterRegistered = false;
					state_ = 0;
				}
			}
		}
	}

	return true;
}

//-------------------------------------------------------------------------------------		
	
}
