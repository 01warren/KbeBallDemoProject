#include "ClusterClient.h"

#include "network/endpoint.h"

namespace KBEngine {
namespace ClusterClient {

namespace {

// 等待可读/可写(select)。返回 true 表示就绪。
bool selectWait(Network::EndPoint& ep, bool writable, uint32 timeoutMS)
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

// 一次短连接往返。ip 为网络字节序，servicePort 为主机字节序。
int roundTrip(const std::string& requestMsg, std::string& response,
	uint32 ip, uint16 servicePort, uint32 timeoutMS)
{
	Network::EndPoint ep;
	ep.socket(SOCK_STREAM);
	if(!ep.good())
		return -1;

	ep.setnonblocking(true);

	// EndPoint::connect 要求端口为网络字节序(见 endpoint.inl: sin_port = networkPort)
	if(ep.connect(htons(servicePort), ip) == -1)
	{
		if(!selectWait(ep, true, timeoutMS))
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
			if(!selectWait(ep, true, 50))
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
		if(!selectWait(ep, false, 100)) { ep.close(); return -1; }
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
		if(!selectWait(ep, false, 100)) { ep.close(); return -1; }
	}

	ep.close();
	return 0;
}

} // 匿名

bool request(const std::string& requestMsg, const std::string& ip, uint16 servicePort,
	CtlResult& result, uint32 timeoutMS)
{
	result = CtlResult();

	uint32 ipnet = inet_addr(ip.c_str());
	if(ipnet == (uint32)INADDR_NONE)
		return false;

	// 最多跟随一次 NOT_LEADER 重定向
	for(int redirect = 0; redirect < 2; ++redirect)
	{
		std::string resp;
		if(roundTrip(requestMsg, resp, ipnet, servicePort, timeoutMS) != 0)
			return false;

		if(resp.size() < 1)
			return false;

		result.reached = true;
		result.type = (uint8)resp[0];

		if(result.type != ClusterInterface::MSG_RESP_NOT_LEADER)
		{
			if(result.type == ClusterInterface::MSG_RESP_CMD_RESULT)
			{
				size_t off = 1;
				int32_t code = 0;
				std::string message;
				if(!ClusterInterface::Wire::getI32(resp.data(), resp.size(), off, code))
					return false;
				ClusterInterface::Wire::getString(resp.data(), resp.size(), off, message);
				result.code = code;
				result.message = message;
			}

			return true;
		}

		// NOT_LEADER：载荷 = uint32 leaderIP(网络序) + uint16 leaderServicePort(主机序)
		result.redirected = true;
		size_t off = 1;
		uint32_t lip = 0;
		uint16_t lp = 0;
		if(!ClusterInterface::Wire::getU32(resp.data(), resp.size(), off, lip))
			return false;
		if(!ClusterInterface::Wire::getU16(resp.data(), resp.size(), off, lp))
			return false;

		if(lip == 0 || lp == 0)
			return false;	// 集群选举中尚无 leader，交由上层稍后重试

		ipnet = lip;
		servicePort = lp;
	}

	return false;
}

bool sendServerCommand(uint8 op, int32 uid, int32 componentType, uint64 cid, uint16 gus,
	const std::string& targetHost, const std::string& ip, uint16 servicePort,
	CtlResult& result, uint32 timeoutMS)
{
	ClusterInterface::ServerCommandData cmd;
	cmd.uid = uid;
	cmd.componentType = componentType;
	cmd.cid = cid;
	cmd.gus = gus;
	cmd.targetHost = targetHost;

	std::string msg = ClusterInterface::encodeMessage(op, ClusterInterface::encodeServerCommand(cmd));
	return request(msg, ip, servicePort, result, timeoutMS);
}

bool queryAllComponents(int32 uid, const std::string& ip, uint16 servicePort,
	std::vector<ClusterInterface::ComponentData>& out, uint32 timeoutMS)
{
	out.clear();

	uint32 ipnet = inet_addr(ip.c_str());
	if(ipnet == (uint32)INADDR_NONE)
		return false;

	std::string payload;
	ClusterInterface::Wire::putU32(payload, (uint32)uid);
	std::string requestMsg = ClusterInterface::encodeMessage(ClusterInterface::MSG_QUERY_ALL, payload);

	// 最多跟随一次 NOT_LEADER 重定向
	for(int redirect = 0; redirect < 2; ++redirect)
	{
		std::string resp;
		if(roundTrip(requestMsg, resp, ipnet, servicePort, timeoutMS) != 0)
			return false;

		if(resp.size() < 1)
			return false;

		uint8_t type = (uint8_t)resp[0];

		if(type == ClusterInterface::MSG_RESP_NOT_LEADER)
		{
			size_t off = 1;
			uint32_t lip = 0;
			uint16_t lp = 0;
			if(!ClusterInterface::Wire::getU32(resp.data(), resp.size(), off, lip))
				return false;
			if(!ClusterInterface::Wire::getU16(resp.data(), resp.size(), off, lp))
				return false;

			if(lip == 0 || lp == 0)
				return false;	// 集群选举中尚无 leader，交由上层稍后重试

			ipnet = lip;
			servicePort = lp;
			continue;
		}

		if(type != ClusterInterface::MSG_RESP_ENTRY_LIST)
			return false;

		size_t off = 1;
		uint16_t count = 0;
		if(!ClusterInterface::Wire::getU16(resp.data(), resp.size(), off, count))
			return false;

		for(uint16_t i = 0; i < count; i++)
		{
			ClusterInterface::ComponentData cd;
			if(!ClusterInterface::decodeComponentData(resp.data(), resp.size(), off, cd))
				return false;
			out.push_back(cd);
		}

		return true;
	}

	return false;
}

}
}
