// XNet: addresses, keys, QoS and link status, backed by the LP2Net core.
#include "config.h"
#include "gfwl.h"
#include "log.h"
#include "net.h"
#include "util.h"

#include <ws2tcpip.h>

using namespace lp2net;

namespace {
WORD g_systemLinkPort = 3074;  // host order

#pragma pack(push, 1)
struct XNetStartupParams {
	BYTE cfgSizeOfStruct;
	BYTE cfgFlags;
	BYTE cfgSockMaxDgramSockets;
	BYTE cfgSockMaxStreamSockets;
	BYTE cfgSockDefaultRecvBufsizeInK;
	BYTE cfgSockDefaultSendBufsizeInK;
	BYTE cfgKeyRegMax;
	BYTE cfgSecRegMax;
	BYTE cfgQosDataLimitDiv4;
	BYTE cfgQosProbeTimeoutInSeconds;
	BYTE cfgQosProbeRetries;
	BYTE cfgQosSrvMaxSimultaneousResponses;
	BYTE cfgQosPairWaitTimeInSeconds;
};
#pragma pack(pop)
constexpr DWORD XNET_OPTID_STARTUP_PARAMS = 1;

// GFWL defaults; the title's own XNetStartup values override them. LP2 reads cfgQosDataLimitDiv4 back through
// XNetGetOpt to decide whether its 53-byte session description fits in the QoS reply; with 0 it publishes
// nothing and joiners reject the session.
XNetStartupParams g_startup{sizeof(XNetStartupParams), 0, 8, 32, 16, 16, 8, 32, 64, 2, 3, 8, 2};
}  // namespace

extern "C" INT WINAPI XNetStartup(const XNetStartupParams* pxnsp) {
	if (pxnsp && pxnsp->cfgSizeOfStruct) {
		const BYTE* in = (const BYTE*)pxnsp;
		BYTE* out = (BYTE*)&g_startup;
		for (size_t i = 1; i < sizeof(g_startup) && i < pxnsp->cfgSizeOfStruct; ++i) {
			if (in[i]) {
				out[i] = in[i];  // zero means "use the default"
			}
		}
	}
	LOG("XNetStartup(%p): qosDataLimit=%u bytes, probe timeout %us x%u", pxnsp, g_startup.cfgQosDataLimitDiv4 * 4u,
	    g_startup.cfgQosProbeTimeoutInSeconds, g_startup.cfgQosProbeRetries);
	NetStartup();
	return 0;
}

extern "C" INT WINAPI XNetCleanup() {
	LOG("XNetCleanup");
	return 0;
}

extern "C" INT WINAPI XNetRandom(BYTE* pb, UINT cb) {
	if (pb && cb) {
		RandomBytes(pb, cb);
	}
	return 0;
}

extern "C" INT WINAPI XNetRegisterKey(const XNKID* pxnkid, const XNKEY* pxnkey) {
	RegisterKey(pxnkid, pxnkey);
	return 0;
}

extern "C" INT WINAPI XNetUnregisterKey(const XNKID* pxnkid) {
	UnregisterKey(pxnkid);
	return 0;
}

extern "C" INT WINAPI XNetXnAddrToInAddr(const XNADDR* pxna, const XNKID* pxnkid, IN_ADDR* pina) {
	if (!pxna || !pina) {
		return WSAEINVAL;
	}
	if (!XnAddrToVirtual(*pxna, pina)) {
		pina->s_addr = 0;
		return WSAEINVAL;
	}
	LOG("XNetXnAddrToInAddr(%s) -> %s", XnAddrToString(*pxna).c_str(),
	    AddrToString(sockaddr_in{AF_INET, 0, *pina}).c_str());
	return 0;
}

extern "C" INT WINAPI XNetInAddrToXnAddr(const IN_ADDR ina, XNADDR* pxna, XNKID* pxnkid) {
	if (!pxna) {
		return WSAEINVAL;
	}
	if (!VirtualToXnAddr(ina, pxna)) {
		LOG("XNetInAddrToXnAddr(%s): unknown address", AddrToString(sockaddr_in{AF_INET, 0, ina}).c_str());
		memset(pxna, 0, sizeof(*pxna));
		return WSAEINVAL;
	}
	if (pxnkid) {
		memset(pxnkid, 0, sizeof(*pxnkid));
	}
	return 0;
}

extern "C" INT WINAPI XNetUnregisterInAddr(const IN_ADDR ina) {
	ForgetVirtual(ina);
	return 0;
}

extern "C" INT WINAPI XNetXnAddrToMachineId(const XNADDR* pxnaddr, ULONGLONG* pqwMachineId) {
	if (!pxnaddr || !pqwMachineId) {
		return WSAEINVAL;
	}
	uint32_t id;
	memcpy(&id, pxnaddr->abEnet, 4);
	*pqwMachineId = 0xFA00000000000000ull | id;
	return id ? 0 : WSAEINVAL;
}

extern "C" INT WINAPI XNetConnect(const IN_ADDR ina) {
	return 0;  // peers are reachable as soon as we know their address
}

extern "C" DWORD WINAPI XNetGetConnectStatus(const IN_ADDR ina) {
	return IsVirtualAddr(ina) ? XNET_CONNECT_STATUS_CONNECTED : XNET_CONNECT_STATUS_IDLE;
}

extern "C" INT WINAPI XNetDnsLookup(const char* pszHost, WSAEVENT hEvent, XNDNS** ppxndns) {
	if (!ppxndns) {
		return WSAEINVAL;
	}
	auto* d = (XNDNS*)calloc(1, sizeof(XNDNS));
	addrinfo hints{}, *res = nullptr;
	hints.ai_family = AF_INET;
	if (pszHost && getaddrinfo(pszHost, nullptr, &hints, &res) == 0) {
		for (addrinfo* r = res; r && d->cina < 8; r = r->ai_next) {
			d->aina[d->cina++] = ((sockaddr_in*)r->ai_addr)->sin_addr;
		}
		freeaddrinfo(res);
	}
	d->iStatus = d->cina ? 0 : WSAHOST_NOT_FOUND;
	*ppxndns = d;
	if (hEvent) {
		WSASetEvent(hEvent);
	}
	LOG("XNetDnsLookup(%s) -> %u address(es)", pszHost ? pszHost : "", d->cina);
	return 0;
}

extern "C" INT WINAPI XNetDnsRelease(XNDNS* pxndns) {
	free(pxndns);
	return 0;
}

extern "C" INT WINAPI XNetQosListen(const XNKID* pxnkid, const BYTE* pb, UINT cb, DWORD dwBitsPerSec,
                                    DWORD dwFlags) {
	QosListen(pxnkid, pb, cb, dwFlags);
	return 0;
}

extern "C" INT WINAPI XNetQosLookup(UINT cxna, const XNADDR* apxna[], const XNKID* apxnkid[], const XNKEY* apxnkey[],
                                    UINT cina, const IN_ADDR aina[], const DWORD adwServiceId[], UINT cProbes,
                                    DWORD dwBitsPerSec, DWORD dwFlags, WSAEVENT hEvent, XNQOS** ppxnqos) {
	if (!ppxnqos) {
		return WSAEINVAL;
	}
	// title-server targets (cina) do not exist any more; they are reported as not contacted
	*ppxnqos = QosLookup(cxna, apxna, apxnkid, hEvent);
	return 0;
}

extern "C" INT WINAPI XNetQosServiceLookup(DWORD dwFlags, WSAEVENT hEvent, XNQOS** ppxnqos) {
	if (!ppxnqos) {
		return WSAEINVAL;
	}
	*ppxnqos = QosLookup(0, nullptr, nullptr, hEvent);
	return 0;
}

extern "C" INT WINAPI XNetQosRelease(XNQOS* pxnqos) {
	QosRelease(pxnqos);
	return 0;
}

extern "C" DWORD WINAPI XNetGetTitleXnAddr(XNADDR* pxna) {
	if (!NetStartup()) {
		return XNET_GET_XNADDR_PENDING;
	}
	if (pxna) {
		*pxna = LocalXnAddr();
	}
	DWORD flags = XNET_GET_XNADDR_ETHERNET | XNET_GET_XNADDR_DHCP | XNET_GET_XNADDR_GATEWAY | XNET_GET_XNADDR_DNS;
	return Cfg().onlineMode ? flags | XNET_GET_XNADDR_ONLINE : flags;  // "online" = connected to LIVE
}

extern "C" DWORD WINAPI XNetGetEthernetLinkStatus() {
	return XNET_ETHERNET_LINK_ACTIVE | XNET_ETHERNET_LINK_100MBPS | XNET_ETHERNET_LINK_FULL_DUPLEX;
}

extern "C" DWORD WINAPI XNetGetBroadcastVersionStatus(BOOL fReset) {
	return 0;
}

extern "C" INT WINAPI XNetGetOpt(DWORD dwOptId, BYTE* pbValue, DWORD* pdwValueSize) {
	if (dwOptId != XNET_OPTID_STARTUP_PARAMS || !pdwValueSize) {
		LOG("XNetGetOpt(%lu): unsupported", dwOptId);
		return WSAEINVAL;
	}
	if (!pbValue || *pdwValueSize < sizeof(g_startup)) {
		*pdwValueSize = sizeof(g_startup);
		return WSAEMSGSIZE;
	}
	memcpy(pbValue, &g_startup, sizeof(g_startup));
	*pdwValueSize = sizeof(g_startup);
	return 0;
}

extern "C" INT WINAPI XNetGetXnAddrPlatform(const XNADDR* pxnaddr, DWORD* pdwPlatform) {
	if (pdwPlatform) {
		*pdwPlatform = 1;  // XNET_XNADDR_PLATFORM_WINPC
	}
	return 0;
}

extern "C" DWORD WINAPI XNetGetSystemLinkPort(WORD* pwSystemLinkPort) {
	if (pwSystemLinkPort) {
		*pwSystemLinkPort = htons(g_systemLinkPort);
	}
	return 0;
}

extern "C" DWORD WINAPI XNetSetSystemLinkPort(WORD wSystemLinkPort) {
	g_systemLinkPort = ntohs(wSystemLinkPort);
	LOG("XNetSetSystemLinkPort(%u)", g_systemLinkPort);
	return 0;
}
