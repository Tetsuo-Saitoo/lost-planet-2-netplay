// XSocket: real Winsock sockets, with LP2Net address translation on datagram sockets.
// The game binds port P; we bind P + slot offset. Every datagram gets a PacketHeader; incoming datagrams are
// filtered and their source is replaced with the sender's virtual address and *game* port.
#include "config.h"
#include "gfwl.h"
#include "log.h"
#include "net.h"
#include "upnp.h"
#include "util.h"

#include <map>
#include <mutex>
#include <vector>

using namespace lp2net;

namespace {
struct SockInfo {
	int type = SOCK_DGRAM;
	int protocol = IPPROTO_UDP;
	bool vdp = false;
	uint16_t gamePort = 0;  // as the game believes it is bound (host order); 0 = not bound yet
	bool connected = false;
	sockaddr_in peer{};     // connect() target as the game sees it (datagram sockets)
};
std::mutex g_mutex;
std::map<SOCKET, SockInfo> g_socks;

bool Lookup(SOCKET s, SockInfo* out) {
	std::lock_guard lock(g_mutex);
	auto it = g_socks.find(s);
	if (it == g_socks.end()) {
		return false;
	}
	*out = it->second;
	return true;
}

bool IsDatagram(SOCKET s, SockInfo* info) {
	return Lookup(s, info) && info->type == SOCK_DGRAM;
}

// The game port a datagram socket sends from; sockets used without bind() get an ephemeral real port.
uint16_t GamePortOf(SOCKET s) {
	std::lock_guard lock(g_mutex);
	SockInfo& si = g_socks[s];
	if (!si.gamePort) {
		sockaddr_in a{};
		int len = sizeof(a);
		if (getsockname(s, (sockaddr*)&a, &len) == 0) {
			si.gamePort = ntohs(a.sin_port);  // ephemeral: no offset applied
		}
	}
	return si.gamePort;
}

thread_local std::vector<BYTE> t_buf(65536);
}  // namespace

extern "C" int WINAPI XWSAStartup(WORD wVersionRequested, LPWSADATA lpWSAData) {
	NetStartup();
	return WSAStartup(wVersionRequested, lpWSAData);
}

extern "C" int WINAPI XWSACleanup() {
	return WSACleanup();
}

extern "C" int WINAPI XWSAGetLastError() {
	return WSAGetLastError();
}

extern "C" SOCKET WINAPI XSocketCreate(int af, int type, int protocol) {
	bool vdp = protocol == IPPROTO_VDP;
	if (type == 0) {
		type = protocol == IPPROTO_TCP ? SOCK_STREAM : SOCK_DGRAM;
	}
	if (vdp || (type == SOCK_DGRAM && protocol == 0)) {
		protocol = IPPROTO_UDP;  // VDP is UDP with a game/voice split the title does itself
	}
	if (type == SOCK_STREAM && protocol == 0) {
		protocol = IPPROTO_TCP;
	}
	SOCKET s = socket(af, type, protocol);
	if (s == INVALID_SOCKET) {
		LOG("XSocketCreate(%d, %d, %d) failed: %d", af, type, protocol, WSAGetLastError());
		return s;
	}
	if (type == SOCK_DGRAM) {
		BOOL yes = TRUE;
		setsockopt(s, SOL_SOCKET, SO_BROADCAST, (const char*)&yes, sizeof(yes));
	}
	{
		std::lock_guard lock(g_mutex);
		g_socks[s] = SockInfo{type, protocol, vdp};
	}
	LOG("XSocketCreate(af=%d, type=%d, proto=%d%s) -> %u", af, type, protocol, vdp ? " VDP" : "", (unsigned)s);
	return s;
}

extern "C" int WINAPI XSocketClose(SOCKET s) {
	{
		std::lock_guard lock(g_mutex);
		g_socks.erase(s);
	}
	LOG("XSocketClose(%u)", (unsigned)s);
	return closesocket(s);
}

extern "C" int WINAPI XSocketShutdown(SOCKET s, int how) {
	return shutdown(s, how);
}

extern "C" int WINAPI XSocketIOCTLSocket(SOCKET s, long cmd, u_long* argp) {
	return ioctlsocket(s, cmd, argp);
}

extern "C" int WINAPI XSocketSetSockOpt(SOCKET s, int level, int optname, const char* optval, int optlen) {
	int r = setsockopt(s, level, optname, optval, optlen);
	LOG("XSocketSetSockOpt(%u, %d, 0x%X, len %d) -> %d", (unsigned)s, level, optname, optlen, r);
	return r;
}

extern "C" int WINAPI XSocketGetSockOpt(SOCKET s, int level, int optname, char* optval, int* optlen) {
	return getsockopt(s, level, optname, optval, optlen);
}

extern "C" int WINAPI XSocketBind(SOCKET s, const sockaddr* name, int namelen) {
	if (!name || namelen < (int)sizeof(sockaddr_in) || name->sa_family != AF_INET) {
		return bind(s, name, namelen);
	}
	sockaddr_in a = *(const sockaddr_in*)name;
	uint16_t gamePort = ntohs(a.sin_port);
	SockInfo si{};
	Lookup(s, &si);
	if (gamePort) {
		a.sin_port = htons(RealGamePort(gamePort, Me().portOffset));
	}
	a.sin_addr.s_addr = INADDR_ANY;  // the game may bind its XNADDR address; we listen everywhere
	int r = bind(s, (const sockaddr*)&a, sizeof(a));
	int err = WSAGetLastError();
	LOG("XSocketBind(%u, game port %u -> real %u) -> %d%s", (unsigned)s, gamePort, ntohs(a.sin_port), r,
	    r ? (" err " + std::to_string(err)).c_str() : "");
	if (r == 0) {
		uint16_t actual = gamePort;
		if (!actual) {
			sockaddr_in b{};
			int len = sizeof(b);
			getsockname(s, (sockaddr*)&b, &len);
			actual = ntohs(b.sin_port);
		}
		{
			std::lock_guard lock(g_mutex);
			g_socks[s].gamePort = actual;
		}
		if (gamePort) {
			if (si.type == SOCK_DGRAM) {
				UpnpMapUdp(RealGamePort(gamePort, Me().portOffset));
			}
		}
	}
	WSASetLastError(err);
	return r;
}

extern "C" int WINAPI XSocketSendTo(SOCKET s, const char* buf, int len, int flags, const sockaddr* to, int tolen) {
	SockInfo si;
	if (!IsDatagram(s, &si) || !to || tolen < (int)sizeof(sockaddr_in) || to->sa_family != AF_INET) {
		return sendto(s, buf, len, flags, to, tolen);
	}
	const sockaddr_in& dst = *(const sockaddr_in*)to;
	if (len < 0 || len > 65507 - (int)sizeof(PacketHeader)) {
		WSASetLastError(WSAEMSGSIZE);
		return SOCKET_ERROR;
	}
	bool bcast = false;
	auto targets = ResolveSend(dst, &bcast);
	PacketHeader h{kMagic, kVersion, bcast ? PacketType::GameBcast : PacketType::Game, Me().instanceId,
	               GamePortOf(s), Me().portOffset};
	std::vector<BYTE>& pkt = t_buf;
	memcpy(pkt.data(), &h, sizeof(h));
	memcpy(pkt.data() + sizeof(h), buf, len);
	int ok = 0, lastErr = 0;
	for (const auto& t : targets) {
		if (t.relayPeer) {
			// through Steam: the destination port travels in the relay frame
			RelaySendGame(t.relayPeer, ntohs(dst.sin_port), pkt.data(), len + (int)sizeof(h)) ? ++ok : 0;
			continue;
		}
		if (sendto(s, (const char*)pkt.data(), len + (int)sizeof(h), flags, (const sockaddr*)&t.addr,
		           sizeof(t.addr)) == SOCKET_ERROR) {
			lastErr = WSAGetLastError();
		}
		else {
			++ok;
		}
	}
	if (Cfg().logPackets) {
		LOG("send %u: %d bytes %s:%u -> %zu target(s)%s", (unsigned)s, len, bcast ? "BCAST" : "",
		    ntohs(dst.sin_port), targets.size(),
		    targets.empty() ? "" : (" first " + (targets[0].relayPeer ? std::string("Steam relay")
		                                                               : AddrToString(targets[0].addr))).c_str());
	}
	if (!ok && !targets.empty()) {
		// broadcasts to slots nobody listens on can fail harmlessly; only report total failure
		WSASetLastError(lastErr);
		return SOCKET_ERROR;
	}
	return len;
}

extern "C" int WINAPI XSocketRecvFrom(SOCKET s, char* buf, int len, int flags, sockaddr* from, int* fromlen) {
	SockInfo si;
	if (!IsDatagram(s, &si)) {
		return recvfrom(s, buf, len, flags, from, fromlen);
	}
	std::vector<BYTE>& pkt = t_buf;
	for (;;) {
		sockaddr_in real{};
		int rl = sizeof(real);
		int n = recvfrom(s, (char*)pkt.data(), (int)pkt.size(), flags, (sockaddr*)&real, &rl);
		if (n == SOCKET_ERROR) {
			int err = WSAGetLastError();
			if (err == WSAECONNRESET) {
				continue;  // ICMP port unreachable from an earlier send (e.g. broadcast to an empty slot)
			}
			return SOCKET_ERROR;
		}
		sockaddr_in gameFrom{};
		PacketHeader h{};
		uint32_t hash = 2166136261u;  // FNV-1a of the payload, used to drop duplicate broadcasts
		if (n >= (int)sizeof(h)) {
			memcpy(&h, pkt.data(), sizeof(h));
			for (int i = (int)sizeof(h); i < n; ++i) {
				hash = (hash ^ pkt[i]) * 16777619u;
			}
		}
		if (n < (int)sizeof(h) || !AcceptIncoming(real, h, hash, &gameFrom)) {
			if (flags & MSG_PEEK) {
				recvfrom(s, (char*)pkt.data(), (int)pkt.size(), 0, (sockaddr*)&real, &rl);  // discard it
			}
			continue;
		}
		int payload = n - (int)sizeof(h);
		if (Cfg().logPackets) {
			LOG("recv %u: %d bytes from %s (game %s)", (unsigned)s, payload, AddrToString(real).c_str(),
			    AddrToString(gameFrom).c_str());
		}
		if (from && fromlen && *fromlen >= (int)sizeof(sockaddr_in)) {
			memcpy(from, &gameFrom, sizeof(gameFrom));
			*fromlen = sizeof(gameFrom);
		}
		if (payload > len) {
			memcpy(buf, pkt.data() + sizeof(h), len);
			WSASetLastError(WSAEMSGSIZE);
			return SOCKET_ERROR;
		}
		memcpy(buf, pkt.data() + sizeof(h), payload);
		return payload;
	}
}

extern "C" int WINAPI XSocketConnect(SOCKET s, const sockaddr* name, int namelen) {
	SockInfo si;
	if (!Lookup(s, &si) || !name || namelen < (int)sizeof(sockaddr_in) || name->sa_family != AF_INET) {
		return connect(s, name, namelen);
	}
	const sockaddr_in& dst = *(const sockaddr_in*)name;
	if (si.type == SOCK_DGRAM) {
		// remember the target; sends go through XSocketSendTo so they get translated and a header
		std::lock_guard lock(g_mutex);
		g_socks[s].connected = true;
		g_socks[s].peer = dst;
		LOG("XSocketConnect(%u, UDP %s)", (unsigned)s, AddrToString(dst).c_str());
		return 0;
	}
	sockaddr_in real = GameToReal(dst);
	int r = connect(s, (const sockaddr*)&real, sizeof(real));
	int err = WSAGetLastError();
	LOG("XSocketConnect(%u, TCP %s -> %s) -> %d (%d)", (unsigned)s, AddrToString(dst).c_str(),
	    AddrToString(real).c_str(), r, r ? err : 0);
	WSASetLastError(err);
	return r;
}

extern "C" int WINAPI XSocketListen(SOCKET s, int backlog) {
	LOG("XSocketListen(%u, %d)", (unsigned)s, backlog);
	return listen(s, backlog);
}

extern "C" SOCKET WINAPI XSocketAccept(SOCKET s, sockaddr* addr, int* addrlen) {
	sockaddr_in real{};
	int rl = sizeof(real);
	SOCKET c = accept(s, (sockaddr*)&real, &rl);
	if (c == INVALID_SOCKET) {
		return c;
	}
	sockaddr_in game = RealToGame(real);
	if (addr && addrlen && *addrlen >= (int)sizeof(sockaddr_in)) {
		memcpy(addr, &game, sizeof(game));
		*addrlen = sizeof(game);
	}
	{
		std::lock_guard lock(g_mutex);
		g_socks[c] = SockInfo{SOCK_STREAM, IPPROTO_TCP};
	}
	LOG("XSocketAccept(%u) -> %u from %s (game %s)", (unsigned)s, (unsigned)c, AddrToString(real).c_str(),
	    AddrToString(game).c_str());
	return c;
}

extern "C" int WINAPI XSocketSelect(int nfds, fd_set* readfds, fd_set* writefds, fd_set* exceptfds,
                                    const timeval* timeout) {
	return select(nfds, readfds, writefds, exceptfds, timeout);
}

extern "C" int WINAPI XSocketRecv(SOCKET s, char* buf, int len, int flags) {
	SockInfo si;
	if (IsDatagram(s, &si)) {
		return XSocketRecvFrom(s, buf, len, flags, nullptr, nullptr);
	}
	return recv(s, buf, len, flags);
}

extern "C" int WINAPI XSocketSend(SOCKET s, const char* buf, int len, int flags) {
	SockInfo si;
	if (IsDatagram(s, &si) && si.connected) {
		return XSocketSendTo(s, buf, len, flags, (const sockaddr*)&si.peer, sizeof(si.peer));
	}
	return send(s, buf, len, flags);
}

extern "C" int WINAPI XSocketGetSockName(SOCKET s, sockaddr* name, int* namelen) {
	int r = getsockname(s, name, namelen);
	SockInfo si;
	if (r == 0 && name && name->sa_family == AF_INET && Lookup(s, &si) && si.gamePort) {
		((sockaddr_in*)name)->sin_port = htons(si.gamePort);
	}
	return r;
}

extern "C" int WINAPI XSocketGetPeerName(SOCKET s, sockaddr* name, int* namelen) {
	SockInfo si;
	if (Lookup(s, &si) && si.type == SOCK_DGRAM && si.connected && name && namelen &&
	    *namelen >= (int)sizeof(sockaddr_in)) {
		memcpy(name, &si.peer, sizeof(si.peer));
		*namelen = sizeof(si.peer);
		return 0;
	}
	int r = getpeername(s, name, namelen);
	if (r == 0 && name && name->sa_family == AF_INET) {
		*(sockaddr_in*)name = RealToGame(*(sockaddr_in*)name);
	}
	return r;
}

extern "C" int WINAPI XWSASendTo(SOCKET s, LPWSABUF lpBuffers, DWORD dwBufferCount, LPDWORD lpNumberOfBytesSent,
                                 DWORD dwFlags, const sockaddr* lpTo, int iTolen, LPWSAOVERLAPPED lpOverlapped,
                                 LPWSAOVERLAPPED_COMPLETION_ROUTINE lpCompletionRoutine) {
	SockInfo si;
	if (!IsDatagram(s, &si) || lpOverlapped || lpCompletionRoutine || !lpBuffers) {
		return WSASendTo(s, lpBuffers, dwBufferCount, lpNumberOfBytesSent, dwFlags, lpTo, iTolen, lpOverlapped,
		                 lpCompletionRoutine);
	}
	std::vector<char> joined;
	for (DWORD i = 0; i < dwBufferCount; ++i) {
		joined.insert(joined.end(), lpBuffers[i].buf, lpBuffers[i].buf + lpBuffers[i].len);
	}
	int n = XSocketSendTo(s, joined.data(), (int)joined.size(), (int)dwFlags, lpTo, iTolen);
	if (n == SOCKET_ERROR) {
		return SOCKET_ERROR;
	}
	if (lpNumberOfBytesSent) {
		*lpNumberOfBytesSent = n;
	}
	return 0;
}

extern "C" unsigned long WINAPI XSocketInet_Addr(const char* cp) {
	return inet_addr(cp);
}

extern "C" u_short WINAPI XSocketHTONS(u_short v) {
	return htons(v);
}

extern "C" u_short WINAPI XSocketNTOHS(u_short v) {
	return ntohs(v);
}

extern "C" int WINAPI XWSAFDIsSet(SOCKET s, fd_set* set) {
	return __WSAFDIsSet(s, set);
}
