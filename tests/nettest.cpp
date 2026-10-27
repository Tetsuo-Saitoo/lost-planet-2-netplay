// End-to-end transport test without the game: two processes load xlive.dll (they become slots 0 and 1) and
// play out a System Link exchange through the GFWL API exactly as a title would.
//   nettest host     (start first)
//   nettest client   -> prints PASS or FAIL
#include "../src/xlive/gfwl.h"

#include <cstdio>
#include <cstring>
#include <string>

namespace {
HMODULE g_dll;
template <typename T>
T Ord(WORD n) {
	auto p = (T)GetProcAddress(g_dll, MAKEINTRESOURCEA(n));
	if (!p) {
		printf("missing ordinal %u\n", n);
		ExitProcess(2);
	}
	return p;
}

using XLiveInitializeEx_t = HRESULT(WINAPI*)(XLIVE_INITIALIZE_INFO*, DWORD);
using XNetStartup_t = INT(WINAPI*)(const void*);
using XNetGetTitleXnAddr_t = DWORD(WINAPI*)(XNADDR*);
using XNetXnAddrToInAddr_t = INT(WINAPI*)(const XNADDR*, const XNKID*, IN_ADDR*);
using XNetQosListen_t = INT(WINAPI*)(const XNKID*, const BYTE*, UINT, DWORD, DWORD);
using XNetQosLookup_t = INT(WINAPI*)(UINT, const XNADDR**, const XNKID**, const XNKEY**, UINT, const IN_ADDR*,
                                     const DWORD*, UINT, DWORD, DWORD, WSAEVENT, XNQOS**);
using XNetQosRelease_t = INT(WINAPI*)(XNQOS*);
using XSessionCreate_t = DWORD(WINAPI*)(DWORD, DWORD, DWORD, DWORD, ULONGLONG*, XSESSION_INFO*, XOVERLAPPED*, HANDLE*);
using XSessionJoinRemote_t = DWORD(WINAPI*)(HANDLE, DWORD, const XUID*, const BOOL*, XOVERLAPPED*);
using XSessionJoinLocal_t = DWORD(WINAPI*)(HANDLE, DWORD, const DWORD*, const BOOL*, XOVERLAPPED*);
using XSessionGetDetails_t = DWORD(WINAPI*)(HANDLE, DWORD*, XSESSION_LOCAL_DETAILS*, XOVERLAPPED*);
using XUserGetXUID_t = DWORD(WINAPI*)(DWORD, XUID*);
using XSocketCreate_t = SOCKET(WINAPI*)(int, int, int);
using XSocketBind_t = int(WINAPI*)(SOCKET, const sockaddr*, int);
using XSocketSendTo_t = int(WINAPI*)(SOCKET, const char*, int, int, const sockaddr*, int);
using XSocketRecvFrom_t = int(WINAPI*)(SOCKET, char*, int, int, sockaddr*, int*);
using XSocketIOCTLSocket_t = int(WINAPI*)(SOCKET, long, u_long*);

struct Api {
	XNetStartup_t XNetStartup;
	XNetGetTitleXnAddr_t XNetGetTitleXnAddr;
	XNetXnAddrToInAddr_t XNetXnAddrToInAddr;
	XNetQosListen_t XNetQosListen;
	XNetQosLookup_t XNetQosLookup;
	XNetQosRelease_t XNetQosRelease;
	XSessionCreate_t XSessionCreate;
	XSessionJoinRemote_t XSessionJoinRemote;
	XSessionJoinLocal_t XSessionJoinLocal;
	XSessionGetDetails_t XSessionGetDetails;
	XUserGetXUID_t XUserGetXUID;
	XSocketCreate_t XSocketCreate;
	XSocketBind_t XSocketBind;
	XSocketSendTo_t XSocketSendTo;
	XSocketRecvFrom_t XSocketRecvFrom;
	XSocketIOCTLSocket_t XSocketIOCTLSocket;
} x;

constexpr uint16_t kPort = 3074;  // what LP2 uses for System Link
constexpr char kQosData[] = "LP2 test lobby";

std::string Ip(IN_ADDR a) {
	char b[32];
	snprintf(b, sizeof(b), "%u.%u.%u.%u", a.S_un.S_un_b.s_b1, a.S_un.S_un_b.s_b2, a.S_un.S_un_b.s_b3,
	         a.S_un.S_un_b.s_b4);
	return b;
}

SOCKET OpenSocket() {
	SOCKET s = x.XSocketCreate(AF_INET, SOCK_DGRAM, IPPROTO_VDP);
	sockaddr_in a{};
	a.sin_family = AF_INET;
	a.sin_port = htons(kPort);
	if (x.XSocketBind(s, (sockaddr*)&a, sizeof(a)) != 0) {
		printf("bind failed %d\n", WSAGetLastError());
		ExitProcess(3);
	}
	u_long nb = 1;
	x.XSocketIOCTLSocket(s, FIONBIO, &nb);
	return s;
}

int Recv(SOCKET s, char* buf, int len, sockaddr_in* from, DWORD timeoutMs) {
	DWORD start = GetTickCount();
	while (GetTickCount() - start < timeoutMs) {
		int fl = sizeof(*from);
		int n = x.XSocketRecvFrom(s, buf, len, 0, (sockaddr*)from, &fl);
		if (n >= 0) {
			buf[n < len ? n : len - 1] = 0;
			return n;
		}
		Sleep(5);
	}
	return -1;
}

int Host() {
	XNADDR me;
	x.XNetGetTitleXnAddr(&me);
	XSESSION_INFO info{};
	ULONGLONG nonce;
	HANDLE hs;
	x.XSessionCreate(XSESSION_CREATE_HOST | XSESSION_CREATE_USES_PEER_NETWORK, 0, 4, 0, &nonce, &info, nullptr, &hs);
	DWORD user = 0;
	x.XSessionJoinLocal(hs, 1, &user, nullptr, nullptr);
	x.XNetQosListen(&info.sessionID, (const BYTE*)kQosData, sizeof(kQosData),
	                0, XNET_QOS_LISTEN_ENABLE | XNET_QOS_LISTEN_SET_DATA);
	SOCKET s = OpenSocket();
	printf("host: listening, lan %s\n", Ip(me.ina).c_str());
	char buf[512];
	sockaddr_in from;
	int pongs = 0;
	DWORD deadline = GetTickCount() + 30000;
	while (GetTickCount() < deadline && pongs < 10) {
		if (Recv(s, buf, sizeof(buf), &from, 200) < 0) {
			continue;
		}
		if (strcmp(buf, "SEARCH") == 0) {
			char reply[8 + sizeof(XSESSION_INFO)];
			memcpy(reply, "SESSION", 8);
			memcpy(reply + 8, &info, sizeof(info));
			x.XSocketSendTo(s, reply, sizeof(reply), 0, (sockaddr*)&from, sizeof(from));
			printf("host: search from %s:%u, answered\n", Ip(from.sin_addr).c_str(), ntohs(from.sin_port));
		}
		else if (strncmp(buf, "PING", 4) == 0) {
			buf[1] = 'O';
			x.XSocketSendTo(s, buf, (int)strlen(buf) + 1, 0, (sockaddr*)&from, sizeof(from));
			++pongs;
		}
		else if (strncmp(buf, "JOIN", 4) == 0) {
			XUID who;
			memcpy(&who, buf + 5, sizeof(who));
			x.XSessionJoinRemote(hs, 1, &who, nullptr, nullptr);
			BYTE det[sizeof(XSESSION_LOCAL_DETAILS) + 8 * sizeof(XSESSION_MEMBER)];
			DWORD cb = sizeof(det);
			x.XSessionGetDetails(hs, &cb, (XSESSION_LOCAL_DETAILS*)det, nullptr);
			printf("host: %016llX joined, session has %lu members\n", who,
			       ((XSESSION_LOCAL_DETAILS*)det)->dwActualMemberCount);
		}
	}
	printf("host: answered %d pings\n", pongs);
	return pongs == 10 ? 0 : 1;
}

int Client() {
	SOCKET s = OpenSocket();
	sockaddr_in bcast{};
	bcast.sin_family = AF_INET;
	bcast.sin_port = htons(kPort);
	bcast.sin_addr.s_addr = INADDR_BROADCAST;
	char buf[512];
	sockaddr_in from{};
	XSESSION_INFO info{};
	bool found = false;
	for (int i = 0; i < 20 && !found; ++i) {
		x.XSocketSendTo(s, "SEARCH", 7, 0, (sockaddr*)&bcast, sizeof(bcast));
		int n = Recv(s, buf, sizeof(buf), &from, 500);
		if (n == 8 + (int)sizeof(XSESSION_INFO) && strcmp(buf, "SESSION") == 0) {
			memcpy(&info, buf + 8, sizeof(info));
			found = true;
		}
	}
	if (!found) {
		printf("client: FAIL no answer to the System Link broadcast\n");
		return 1;
	}
	printf("client: found session, reply came from %s:%u\n", Ip(from.sin_addr).c_str(), ntohs(from.sin_port));

	IN_ADDR hostIna{};
	if (x.XNetXnAddrToInAddr(&info.hostAddress, &info.sessionID, &hostIna) != 0) {
		printf("client: FAIL XNetXnAddrToInAddr\n");
		return 1;
	}
	bool sameAddr = hostIna.s_addr == from.sin_addr.s_addr;
	printf("client: host XNADDR -> %s (%s the reply's source)\n", Ip(hostIna).c_str(), sameAddr ? "matches" : "DIFFERS from");

	const XNADDR* addrs[] = {&info.hostAddress};
	const XNKID* kids[] = {&info.sessionID};
	const XNKEY* keys[] = {&info.keyExchangeKey};
	WSAEVENT ev = WSACreateEvent();
	XNQOS* q = nullptr;
	x.XNetQosLookup(1, addrs, kids, keys, 0, nullptr, nullptr, 2, 0, 0, ev, &q);
	DWORD w = WaitForSingleObject(ev, 5000);
	const XNQOSINFO& qi = q->axnqosinfo[0];
	bool qosOk = w == WAIT_OBJECT_0 && (qi.bFlags & XNET_XNQOSINFO_TARGET_CONTACTED) && qi.cbData == sizeof(kQosData) &&
	             memcmp(qi.pbData, kQosData, sizeof(kQosData)) == 0;
	printf("client: QoS flags=0x%02X rtt=%ums data='%.*s' -> %s\n", qi.bFlags, qi.wRttMedInMsecs, (int)qi.cbData,
	       (const char*)qi.pbData, qosOk ? "ok" : "FAIL");
	x.XNetQosRelease(q);

	ULONGLONG nonce;
	HANDLE hs;
	x.XSessionCreate(XSESSION_CREATE_USES_PEER_NETWORK, 0, 4, 0, &nonce, &info, nullptr, &hs);
	XUID me = 0;
	x.XUserGetXUID(0, &me);
	sockaddr_in host{};
	host.sin_family = AF_INET;
	host.sin_port = htons(kPort);
	host.sin_addr = hostIna;
	char join[16] = "JOIN";
	memcpy(join + 5, &me, sizeof(me));
	x.XSocketSendTo(s, join, sizeof(join), 0, (sockaddr*)&host, sizeof(host));

	int ok = 0;
	DWORD worst = 0;
	for (int i = 0; i < 10; ++i) {
		char ping[32];
		snprintf(ping, sizeof(ping), "PING %d", i);
		DWORD t0 = GetTickCount();
		x.XSocketSendTo(s, ping, (int)strlen(ping) + 1, 0, (sockaddr*)&host, sizeof(host));
		int n = Recv(s, buf, sizeof(buf), &from, 2000);
		DWORD rtt = GetTickCount() - t0;
		if (n > 0 && buf[1] == 'O' && atoi(buf + 5) == i && from.sin_addr.s_addr == hostIna.s_addr &&
		    ntohs(from.sin_port) == kPort) {
			++ok;
			worst = rtt > worst ? rtt : worst;
		}
	}
	printf("client: %d/10 pongs from %s:%u, worst rtt %lums\n", ok, Ip(hostIna).c_str(), kPort, worst);
	bool pass = sameAddr && qosOk && ok == 10;
	printf("%s\n", pass ? "PASS" : "FAIL");
	return pass ? 0 : 1;
}
}  // namespace

int main(int argc, char** argv) {
	if (argc < 2) {
		printf("usage: nettest host|client\n");
		return 2;
	}
	g_dll = LoadLibraryW(L"xlive.dll");
	if (!g_dll) {
		printf("cannot load xlive.dll (%lu)\n", GetLastError());
		return 2;
	}
	x = Api{Ord<XNetStartup_t>(51),          Ord<XNetGetTitleXnAddr_t>(73), Ord<XNetXnAddrToInAddr_t>(57),
	        Ord<XNetQosListen_t>(69),        Ord<XNetQosLookup_t>(70),      Ord<XNetQosRelease_t>(72),
	        Ord<XSessionCreate_t>(5300),     Ord<XSessionJoinRemote_t>(5326), Ord<XSessionJoinLocal_t>(5327),
	        Ord<XSessionGetDetails_t>(5328), Ord<XUserGetXUID_t>(5261),     Ord<XSocketCreate_t>(3),
	        Ord<XSocketBind_t>(11),          Ord<XSocketSendTo_t>(24),      Ord<XSocketRecvFrom_t>(20),
	        Ord<XSocketIOCTLSocket_t>(6)};
	XLIVE_INITIALIZE_INFO init{sizeof(init)};
	Ord<XLiveInitializeEx_t>(5297)(&init, 0x31006300);
	x.XNetStartup(nullptr);
	return strcmp(argv[1], "host") == 0 ? Host() : Client();
}
