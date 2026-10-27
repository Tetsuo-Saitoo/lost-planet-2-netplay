// LP2Net core. See net.h for the model: per-instance identity, virtual peer addresses, the packet header, and a
// control ("core") UDP socket per instance for hello/keepalive, QoS probes and session beacons.
#include "net.h"

#include "config.h"
#include "log.h"
#include "matchmaking.h"
#include "overlay.h"
#include "presence.h"
#include "stats.h"
#include "steam.h"
#include "upnp.h"
#include "util.h"

#include <ws2tcpip.h>

#include <algorithm>
#include <atomic>
#include <map>
#include <mutex>
#include <set>

namespace lp2net {
namespace {

// ---- state ---------------------------------------------------------------------------------------------------
struct Peer {
	uint32_t id = 0;
	IN_ADDR vaddr{};
	XNADDR xnaddr{};
	bool haveXnaddr = false;
	IN_ADDR addr{};          // IP we last heard them from (or best guess from their XNADDR)
	uint16_t offset = 0;     // their slot port offset
	std::map<uint16_t, sockaddr_in> seen;  // their game port -> real endpoint we last received it from
	std::string name;
	ULONGLONG lastHeard = 0;
	ULONGLONG lastHelloSent = 0;
	ULONGLONG firstHelloSent = 0;  // for peers we were only introduced to: give up after 30 s without an answer
	int pingMs = -1;
	bool fromManual = false;
	uint64_t steamId = 0;      // known when they run Steam: lets us reach them through the Steam relay
	ULONGLONG lastDirect = 0;      // last packet over plain UDP
	ULONGLONG lastDirectGame = 0;  // last game packet over plain UDP
	ULONGLONG lastRelayGame = 0;   // last game packet through Steam
	ULONGLONG lastDirectAck = 0;   // last HelloAck over plain UDP: they hear us directly too
	ULONGLONG lastRelay = 0;       // last packet through Steam
	bool reportsGame = false;      // their hellos say whether our game packets reach them over UDP (0.2+)
	bool hearOurGame = false;      // ...and the last one said yes
	bool toldHear = false;         // what our hellos tell them about their game packets
};

struct ManualPeer {
	std::string text;
	sockaddr_in core{};  // their core socket
	bool resolved = false;
	uint32_t id = 0;     // instance id once they answered
};

struct RemoteSession {
	uint32_t id;  // host instance id
	std::string host;
	sockaddr_in core{};
	AdvertisedSession s;
	ULONGLONG lastSeen = 0;
	int pingMs = -1;
	bool lan = false;
	uint64_t steamLobby = 0, hostSteamId = 0;
};

struct QosListenEntry {
	bool enabled = false;
	std::vector<BYTE> data;
};

struct QosJob {
	XNQOS* q;
	WSAEVENT event;
	struct Target {
		uint32_t peerId;
		ULONGLONG firstSent = 0;
		int sent = 0, got = 0;
		std::vector<int> rtts;
		bool done = false;
	};
	std::vector<Target> targets;
};

constexpr size_t kQosDataMax = 1024;  // reserved per XNQOSINFO entry for listen data

std::recursive_mutex g_mutex;
LocalIdentity g_me;
std::atomic<bool> g_started{false};
HANDLE g_slotMutex = nullptr;
SOCKET g_core = INVALID_SOCKET;
// plain Win32 threads: a std::thread still running at process exit aborts the game (0xC0000409)
HANDLE g_worker = nullptr;
std::atomic<bool> g_stop{false};
std::map<uint32_t, Peer> g_peers;
std::map<uint32_t, uint32_t> g_byVaddr;  // vaddr (network order) -> peer id
std::vector<ManualPeer> g_manual;
std::map<uint32_t, RemoteSession> g_remoteSessions;
std::map<ULONGLONG, QosListenEntry> g_qosListen;  // by XNKID
std::vector<QosJob*> g_qosJobs;
std::map<ULONGLONG, XNKEY> g_keys;
bool g_hosting = false;
AdvertisedSession g_advert;
ULONGLONG g_lastBeacon = 0, g_lastQuery = 0;
uint32_t g_qosSeq = 0;
// Steam: relayed game datagrams are handed to the game's sockets over loopback from this socket
SOCKET g_inject = INVALID_SOCKET;
uint16_t g_injectPort = 0;
ULONGLONG g_lobbyStamp = 0, g_lastFriendScan = 0;
// A peer that answered our UDP hello this recently is reached over UDP only. Steam peers get a hello every
// kSteamHelloMs, so a broken UDP path falls back to the relay well inside the game's 20 s receive timeout.
constexpr ULONGLONG kDirectFresh = 10000;
constexpr ULONGLONG kSteamHelloMs = 3000;
constexpr ULONGLONG kRelayDuplicateMs = 1500;  // their UDP packets arrived this recently: drop relayed copies
constexpr ULONGLONG kPeerGoneMs = 120000;      // stop saying hello to peers silent this long

ULONGLONG KidKey(const XNKID* k) {
	ULONGLONG v;
	memcpy(&v, k->ab, 8);
	return v;
}

sockaddr_in MakeAddr(IN_ADDR ip, uint16_t port) {
	sockaddr_in a{};
	a.sin_family = AF_INET;
	a.sin_addr = ip;
	a.sin_port = htons(port);
	return a;
}

IN_ADDR Ip(uint32_t hostOrder) {
	IN_ADDR a;
	a.s_addr = htonl(hostOrder);
	return a;
}

bool IsPrivate(IN_ADDR a) {
	uint32_t h = ntohl(a.s_addr);
	return (h >> 24) == 10 || (h >> 20) == 0xAC1 || (h >> 16) == 0xC0A8 || (h >> 24) == 127 || (h >> 16) == 0xA9FE;
}

IN_ADDR ChooseLanAddress() {
	IN_ADDR a{};
	if (!Cfg().lanAddress.empty() && inet_pton(AF_INET, Cfg().lanAddress.c_str(), &a) == 1) {
		return a;
	}
	// the source address the OS would use for the default route; connect() on UDP sends nothing
	SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	sockaddr_in probe = MakeAddr(Ip(0x08080808), 53);
	sockaddr_in local{};
	int len = sizeof(local);
	if (connect(s, (sockaddr*)&probe, sizeof(probe)) == 0 && getsockname(s, (sockaddr*)&local, &len) == 0) {
		a = local.sin_addr;
	}
	else {
		a = Ip(INADDR_LOOPBACK);
	}
	closesocket(s);
	return a;
}

IN_ADDR MakeVaddr(uint32_t id) {
	return Ip(0x0A000000 | (id & 0x00FFFFFF));
}

Peer& UpsertPeer(uint32_t id) {
	auto it = g_peers.find(id);
	if (it != g_peers.end()) {
		return it->second;
	}
	Peer& p = g_peers[id];
	p.id = id;
	p.vaddr = MakeVaddr(id);
	g_byVaddr[p.vaddr.s_addr] = id;
	LOG("peer %08X: new, virtual %s", id, AddrToString(MakeAddr(p.vaddr, 0)).c_str());
	return p;
}

Peer* PeerByVaddr(IN_ADDR v) {
	auto it = g_byVaddr.find(v.s_addr);
	if (it == g_byVaddr.end()) {
		return nullptr;
	}
	auto p = g_peers.find(it->second);
	return p == g_peers.end() ? nullptr : &p->second;
}

sockaddr_in PeerCore(const Peer& p) {
	return MakeAddr(p.addr, kCorePortBase + p.offset);
}

void SetPublicIp(const std::string& ip) {
	std::lock_guard lock(g_mutex);
	IN_ADDR a{};
	if (inet_pton(AF_INET, ip.c_str(), &a) == 1 && !IsPrivate(a) && a.s_addr != g_me.wan.s_addr) {
		g_me.wan = a;
		LOG("public address: %s", ip.c_str());
	}
}

// ---- core socket I/O ---------------------------------------------------------------------------------------------
#pragma pack(push, 1)
struct HelloBody {
	char name[16];
	XNADDR xnaddr;
	sockaddr_in yourAddr;  // in acks: where the sender saw this packet come from (lets peers learn their WAN IP)
	uint64_t steamId;      // added later: older builds send the body without it
	uint8_t hearYourGame;  // added in 0.2: your game packets reached me over UDP lately (absent in older builds)
};
constexpr int kHelloBodyV1 = (int)offsetof(HelloBody, steamId);
struct QosBody {
	uint32_t seq;
	ULONGLONG sentTick;
	XNKID kid;
	uint8_t enabled;
	uint16_t cbData;
	// data follows in replies
};
struct SessionBody {
	char host[16];
	XSESSION_INFO info;
	uint32_t flags;
	uint32_t gameType, gameMode;
	uint8_t players, maxPlayers;
	ULONGLONG queryTick;  // echoed back so the asker can compute ping
};
// Appended to SessionBody by newer builds (older ones ignore it): what LIVE matchmaking needs.
struct SessionExt {
	uint8_t filledPublic, filledPrivate, maxPublic, maxPrivate;
	uint8_t inGame;
	uint64_t hostSteamId;
	uint16_t cbAttrs;  // packed attributes follow
};
#pragma pack(pop)

void SendCore(const sockaddr_in& to, PacketType type, const void* body, size_t cb) {
	BYTE buf[sizeof(PacketHeader) + 1500];
	if (cb > 1500 || g_core == INVALID_SOCKET) {
		return;
	}
	PacketHeader h{kMagic, kVersion, type, g_me.instanceId, (uint16_t)(kCorePortBase), g_me.portOffset};
	memcpy(buf, &h, sizeof(h));
	memcpy(buf + sizeof(h), body, cb);
	sendto(g_core, (const char*)buf, (int)(sizeof(h) + cb), 0, (const sockaddr*)&to, sizeof(to));
}

// Every LP2Net instance that might be listening: all local slots on the LAN broadcast, manual peers, known peers.
std::vector<sockaddr_in> CoreTargets() {
	std::vector<sockaddr_in> out;
	for (int k = 0; k < kMaxSlots; ++k) {
		out.push_back(MakeAddr(Ip(INADDR_BROADCAST), (uint16_t)(kCorePortBase + k * kPortStride)));
	}
	for (const auto& m : g_manual) {
		if (m.resolved) {
			out.push_back(m.core);
		}
	}
	for (const auto& [id, p] : g_peers) {
		if (p.lastHeard) {
			out.push_back(PeerCore(p));
		}
	}
	return out;
}

// ---- Steam relay -------------------------------------------------------------------------------------------------
// Relay frame: [uint16 destination game port, 0 = core socket][PacketHeader][payload].
// Plain UDP whenever we have an address for them: it costs nothing, it opens the NAT on our side, and it is the
// only way to learn that UDP works. (Sending through Steam alone kept two PCs on one LAN on the relay for good.)
bool UseDirect(const Peer& p, ULONGLONG) {
	return p.addr.s_addr != 0;
}

bool UseRelay(const Peer& p, ULONGLONG now, bool game) {
	// Until they have answered a UDP hello over UDP, the relay carries a copy as well; game packets also until the
	// peer reports that they arrive over UDP (their game ports may not be open when the core port is). Never to our
	// own Steam account (several copies on one PC share it, and Steam would hand the packet to any of them).
	if (!p.steamId || p.steamId == SteamMyId() || !SteamReady() || !Cfg().steamRelay) {
		return false;
	}
	bool udpWorks = p.lastDirectAck && now - p.lastDirectAck < kDirectFresh;
	if (game && p.reportsGame) {
		udpWorks = udpWorks && p.hearOurGame;
	}
	return !udpWorks;
}

// Best UDP address for a peer we only know from their XNADDR: same public address (or none known) = same LAN.
IN_ADDR GuessAddr(const XNADDR& x) {
	bool sameSite = x.inaOnline.s_addr == g_me.wan.s_addr || x.inaOnline.s_addr == x.ina.s_addr;
	return sameSite ? x.ina : x.inaOnline;
}

bool RelayFrame(uint64_t steamId, uint16_t dstPort, const PacketHeader& h, const void* body, size_t cb) {
	BYTE buf[2 + sizeof(PacketHeader) + 1500];
	if (cb > 1500) {
		return false;
	}
	memcpy(buf, &dstPort, 2);
	memcpy(buf + 2, &h, sizeof(h));
	memcpy(buf + 2 + sizeof(h), body, cb);
	return SteamRelaySend(steamId, buf, (uint32_t)(2 + sizeof(h) + cb));
}

// Core packet to a known peer: plain UDP, the Steam relay, or both while we don't know which one works.
void SendCorePeer(const Peer& p, PacketType type, const void* body, size_t cb) {
	ULONGLONG now = GetTickCount64();
	if (UseDirect(p, now)) {
		SendCore(PeerCore(p), type, body, cb);
	}
	if (UseRelay(p, now, false)) {
		PacketHeader h{kMagic, kVersion, type, g_me.instanceId, (uint16_t)(kCorePortBase), g_me.portOffset};
		RelayFrame(p.steamId, 0, h, body, cb);
	}
}

HelloBody MakeHello(const sockaddr_in* yourAddr, Peer* to = nullptr) {
	HelloBody b{};
	strncpy_s(b.name, g_me.name.c_str(), _TRUNCATE);
	b.xnaddr = LocalXnAddr();
	if (yourAddr) {
		b.yourAddr = *yourAddr;
	}
	b.steamId = SteamMyId();
	if (to) {
		// Changes only on evidence: their game packets over UDP (yes), or only through Steam (no). A quiet lobby
		// keeps the last answer, so the relay does not restart every time the game stops talking for a while.
		ULONGLONG now = GetTickCount64();
		if (to->lastDirectGame && now - to->lastDirectGame < kDirectFresh) {
			to->toldHear = true;
		}
		else if (to->lastRelayGame && now - to->lastRelayGame < kDirectFresh) {
			to->toldHear = false;
		}
		b.hearYourGame = to->toldHear;
	}
	return b;
}

void SendHello(const sockaddr_in& to, PacketType type, const sockaddr_in* yourAddr) {
	HelloBody b = MakeHello(yourAddr);
	SendCore(to, type, &b, sizeof(b));
}

void SendHelloPeer(Peer& p, PacketType type) {
	HelloBody b = MakeHello(nullptr, &p);
	SendCorePeer(p, type, &b, sizeof(b));
	p.lastHelloSent = GetTickCount64();
	if (!p.firstHelloSent) {
		p.firstHelloSent = p.lastHelloSent;
	}
}

std::vector<BYTE> MakeSessionBody(ULONGLONG queryTick) {
	std::vector<BYTE> out(sizeof(SessionBody) + sizeof(SessionExt) + g_advert.attrs.size());
	auto* b = (SessionBody*)out.data();
	strncpy_s(b->host, g_me.name.c_str(), _TRUNCATE);
	b->info = g_advert.info;
	b->flags = g_advert.flags;
	b->gameType = g_advert.gameType;
	b->gameMode = g_advert.gameMode;
	b->players = (uint8_t)g_advert.players;
	b->maxPlayers = (uint8_t)g_advert.maxPlayers;
	b->queryTick = queryTick;
	auto* x = (SessionExt*)(b + 1);
	x->filledPublic = (uint8_t)g_advert.filledPublic;
	x->filledPrivate = (uint8_t)g_advert.filledPrivate;
	x->maxPublic = (uint8_t)g_advert.maxPublic;
	x->maxPrivate = (uint8_t)g_advert.maxPrivate;
	x->inGame = g_advert.inGame;
	x->hostSteamId = SteamMyId();
	x->cbAttrs = (uint16_t)g_advert.attrs.size();
	memcpy(x + 1, g_advert.attrs.data(), g_advert.attrs.size());
	if (out.size() > 1400) {
		out.resize(sizeof(SessionBody) + sizeof(SessionExt));  // never fragment; attrs are ~0.5 KB in practice
		((SessionExt*)(out.data() + sizeof(SessionBody)))->cbAttrs = 0;
	}
	return out;
}

// Steam lobby keys describing the session we host (see SessionFromSteamLobby for the reading side).
std::map<std::string, std::string> MakeLobbyData() {
	auto hex = [](const void* p, size_t n) { return ToHex(p, n); };
	char buf[64];
	std::map<std::string, std::string> d;
	d["v"] = "1";
	d["kind"] = (g_advert.flags & XSESSION_CREATE_USES_MATCHMAKING) ? "live" : "link";
	d["name"] = g_me.name;
	snprintf(buf, sizeof(buf), "%08X", g_me.instanceId);
	d["inst"] = buf;
	d["sinfo"] = hex(&g_advert.info, sizeof(g_advert.info));
	snprintf(buf, sizeof(buf), "%lu %lu %lu %d %d %d %d %d", g_advert.flags, g_advert.gameType, g_advert.gameMode,
	         g_advert.filledPublic, g_advert.filledPrivate, g_advert.maxPublic, g_advert.maxPrivate,
	         g_advert.inGame ? 1 : 0);
	d["slots"] = buf;
	d["attrs"] = hex(g_advert.attrs.data(), g_advert.attrs.size());
	d["mode"] = std::to_string(CurrentPresence().mode);  // RICH_MODE: tells a System Link guest which menu to open
	return d;
}

void FinishQosTarget(QosJob& job, size_t i, bool contacted, bool disabled, const BYTE* data, uint16_t cb) {
	QosJob::Target& t = job.targets[i];
	if (t.done) {
		return;
	}
	t.done = true;
	XNQOSINFO& e = job.q->axnqosinfo[i];
	e.cProbesXmit = (WORD)t.sent;
	e.cProbesRecv = (WORD)t.got;
	if (!t.rtts.empty()) {
		std::sort(t.rtts.begin(), t.rtts.end());
		e.wRttMinInMsecs = (WORD)t.rtts.front();
		e.wRttMedInMsecs = (WORD)t.rtts[t.rtts.size() / 2];
	}
	e.dwUpBitsPerSec = e.dwDnBitsPerSec = contacted ? 10'000'000 : 0;
	if (data && cb) {
		cb = (uint16_t)std::min<size_t>(cb, kQosDataMax);
		memcpy(e.pbData, data, cb);
		e.cbData = cb;
	}
	e.bFlags = XNET_XNQOSINFO_COMPLETE | (contacted ? XNET_XNQOSINFO_TARGET_CONTACTED : 0) |
	           (disabled ? XNET_XNQOSINFO_TARGET_DISABLED : 0) | (e.cbData ? XNET_XNQOSINFO_DATA_RECEIVED : 0);
	if (InterlockedDecrement((volatile LONG*)&job.q->cxnqosPending) == 0 && job.event) {
		WSASetEvent(job.event);
	}
}

// Core packet from a peer, over UDP (`from` = their real address) or through the Steam relay (relaySteamId set).
void HandleCore(const sockaddr_in& from, const PacketHeader& h, const BYTE* body, int cb, uint64_t relaySteamId = 0) {
	std::lock_guard lock(g_mutex);
	if (h.instanceId == g_me.instanceId) {
		return;  // our own broadcast
	}
	ULONGLONG now = GetTickCount64();
	Peer& p = UpsertPeer(h.instanceId);
	p.offset = h.srcOffset;
	p.lastHeard = now;
	if (relaySteamId) {
		p.steamId = relaySteamId;
		p.lastRelay = now;
	}
	else {
		p.addr = from.sin_addr;
		p.lastDirect = now;
	}
	auto reply = [&](PacketType type, const void* data, size_t len) {
		if (relaySteamId) {
			PacketHeader rh{kMagic, kVersion, type, g_me.instanceId, (uint16_t)(kCorePortBase), g_me.portOffset};
			RelayFrame(relaySteamId, 0, rh, data, len);
		}
		else {
			SendCore(from, type, data, len);
		}
	};

	switch (h.type) {
	case PacketType::Hello:
	case PacketType::HelloAck: {
		if (cb < kHelloBodyV1) {
			return;
		}
		HelloBody b{};
		memcpy(&b, body, std::min<size_t>(cb, sizeof(b)));
		bool isNew = p.name.empty();
		p.name.assign(b.name, strnlen(b.name, sizeof(b.name)));
		p.xnaddr = b.xnaddr;
		p.haveXnaddr = true;
		if (b.steamId && !p.steamId) {
			p.steamId = b.steamId;
		}
		if (!p.addr.s_addr) {
			p.addr = GuessAddr(b.xnaddr);  // first heard through Steam: try UDP to where they say they are
		}
		if (h.type == PacketType::HelloAck && !relaySteamId) {
			if (!p.lastDirectAck && p.lastRelay) {
				LOG("peer %08X: UDP works, leaving the Steam relay", h.instanceId);
			}
			p.lastDirectAck = now;
		}
		if (cb >= (int)sizeof(HelloBody)) {
			if (p.reportsGame && p.hearOurGame != (b.hearYourGame != 0)) {
				LOG("peer %08X: our game packets %s them over UDP", h.instanceId,
				    b.hearYourGame ? "now reach" : "no longer reach");
			}
			p.reportsGame = true;
			p.hearOurGame = b.hearYourGame != 0;
		}
		for (auto& m : g_manual) {
			if (m.resolved && !relaySteamId && m.core.sin_addr.s_addr == from.sin_addr.s_addr &&
			    m.core.sin_port == from.sin_port) {
				m.id = h.instanceId;
				p.fromManual = true;
			}
		}
		if (h.type == PacketType::Hello) {
			HelloBody ack = MakeHello(relaySteamId ? nullptr : &from, &p);
			reply(PacketType::HelloAck, &ack, sizeof(ack));
		}
		else if (relaySteamId) {
			// acks through Steam carry no address information
		}
		else if (b.yourAddr.sin_family == AF_INET && !IsPrivate(b.yourAddr.sin_addr)) {
			char ip[INET_ADDRSTRLEN];
			inet_ntop(AF_INET, &b.yourAddr.sin_addr, ip, sizeof(ip));
			if (Cfg().publicAddress.empty()) {
				SetPublicIp(ip);
			}
		}
		if (isNew) {
			LOG("peer %08X: hello from %s %s %s (%s)%s", h.instanceId, p.name.c_str(),
			    relaySteamId ? "through Steam, account" : "at",
			    relaySteamId ? std::to_string(relaySteamId).c_str() : AddrToString(from).c_str(),
			    XnAddrToString(p.xnaddr).c_str(), p.steamId ? " [Steam]" : "");
			OverlayToast(p.name + (relaySteamId ? " is reachable through Steam" : " is reachable"));
			for (const auto& s : StatsPackets()) {
				SendCorePeer(p, PacketType::Stats, s.data(), s.size());
			}
		}
		break;
	}
	case PacketType::Stats:
		StatsReceive(h.instanceId, body, cb);
		break;
	case PacketType::QosProbe: {
		if (cb < (int)sizeof(QosBody)) {
			return;
		}
		BYTE out[sizeof(QosBody) + kQosDataMax];
		auto* r = (QosBody*)out;
		*r = *(const QosBody*)body;
		auto it = g_qosListen.find(KidKey(&r->kid));
		r->enabled = it != g_qosListen.end() && it->second.enabled;
		r->cbData = 0;
		if (r->enabled) {
			r->cbData = (uint16_t)std::min(it->second.data.size(), kQosDataMax);
			memcpy(out + sizeof(QosBody), it->second.data.data(), r->cbData);
		}
		reply(PacketType::QosReply, out, sizeof(QosBody) + r->cbData);
		break;
	}
	case PacketType::QosReply: {
		if (cb < (int)sizeof(QosBody)) {
			return;
		}
		auto* r = (const QosBody*)body;
		int rtt = (int)(now - r->sentTick);
		p.pingMs = rtt;
		for (QosJob* job : g_qosJobs) {
			for (size_t i = 0; i < job->targets.size(); ++i) {
				auto& t = job->targets[i];
				if (t.peerId != h.instanceId || t.done) {
					continue;
				}
				t.got++;
				t.rtts.push_back(rtt);
				if (t.got >= 2 || t.got >= t.sent) {
					FinishQosTarget(*job, i, true, !r->enabled, (const BYTE*)(r + 1),
					                (uint16_t)std::min<int>(r->cbData, cb - (int)sizeof(QosBody)));
				}
			}
		}
		break;
	}
	case PacketType::SessionQuery: {
		if (g_hosting && cb >= (int)sizeof(ULONGLONG)) {
			std::vector<BYTE> b = MakeSessionBody(*(const ULONGLONG*)body);
			reply(PacketType::SessionInfo, b.data(), b.size());
		}
		break;
	}
	case PacketType::SessionInfo: {
		if (cb < (int)sizeof(SessionBody)) {
			return;
		}
		auto* b = (const SessionBody*)body;
		RemoteSession& rs = g_remoteSessions[h.instanceId];
		rs.id = h.instanceId;
		rs.host.assign(b->host, strnlen(b->host, sizeof(b->host)));
		rs.core = relaySteamId ? PeerCore(p) : from;
		rs.s.info = b->info;
		rs.s.flags = b->flags;
		rs.s.gameType = b->gameType;
		rs.s.gameMode = b->gameMode;
		rs.s.players = b->players;
		rs.s.maxPlayers = b->maxPlayers;
		rs.lastSeen = now;
		rs.lan = !relaySteamId && IsPrivate(from.sin_addr);
		if (b->queryTick) {
			rs.pingMs = (int)(now - b->queryTick);
		}
		if (cb >= (int)(sizeof(SessionBody) + sizeof(SessionExt))) {
			auto* x = (const SessionExt*)(b + 1);
			rs.s.filledPublic = x->filledPublic;
			rs.s.filledPrivate = x->filledPrivate;
			rs.s.maxPublic = x->maxPublic;
			rs.s.maxPrivate = x->maxPrivate;
			rs.s.inGame = x->inGame != 0;
			if (x->hostSteamId) {
				rs.hostSteamId = x->hostSteamId;
				p.steamId = x->hostSteamId;
			}
			size_t avail = cb - sizeof(SessionBody) - sizeof(SessionExt);
			rs.s.attrs.assign((const BYTE*)(x + 1), (const BYTE*)(x + 1) + std::min<size_t>(x->cbAttrs, avail));
		}
		else {
			// older build: no slot details; derive them from the player counts
			rs.s.maxPublic = b->maxPlayers;
			rs.s.filledPublic = b->players;
		}
		break;
	}
	default:
		break;
	}
}

void ResolveManual(ManualPeer& m) {
	std::string host = m.text;
	uint16_t port = kCorePortBase;
	size_t colon = host.rfind(':');
	if (colon != std::string::npos) {
		port = (uint16_t)atoi(host.c_str() + colon + 1);
		host.resize(colon);
	}
	addrinfo hints{}, *res = nullptr;
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_DGRAM;
	if (getaddrinfo(host.c_str(), nullptr, &hints, &res) == 0 && res) {
		m.core = *(sockaddr_in*)res->ai_addr;
		m.core.sin_port = htons(port);
		m.resolved = true;
		freeaddrinfo(res);
		LOG("manual peer %s -> %s", m.text.c_str(), AddrToString(m.core).c_str());
	}
	else {
		LOG("manual peer %s: cannot resolve", m.text.c_str());
	}
}

void Tick() {
	std::lock_guard lock(g_mutex);
	ULONGLONG now = GetTickCount64();
	// keep manual peers (and NAT mappings) alive
	for (auto& m : g_manual) {
		if (!m.resolved) {
			ResolveManual(m);
		}
		if (m.resolved) {
			auto it = g_peers.find(m.id);
			ULONGLONG last = it == g_peers.end() ? 0 : it->second.lastHelloSent;
			if (now - last > (m.id ? 10000u : 2000u)) {
				SendHello(m.core, PacketType::Hello, nullptr);
				if (it != g_peers.end()) {
					it->second.lastHelloSent = now;
				}
				else {
					m.id = 0;
				}
			}
		}
	}
	for (auto& [id, p] : g_peers) {
		// peers we heard from, and Steam players we were introduced to but have not heard from yet
		bool introduced = p.steamId && !p.lastHeard && p.lastHelloSent && now - p.lastHelloSent > 2000 &&
		                  now - p.firstHelloSent < 30000;
		bool alive = p.lastHeard && now - p.lastHeard < kPeerGoneMs;
		if ((alive && now - p.lastHelloSent > (p.steamId ? kSteamHelloMs : 10000)) || introduced) {
			SendHelloPeer(p, PacketType::Hello);
		}
	}
	// beacon our session to the LAN and peers
	if (g_hosting && now - g_lastBeacon > 3000) {
		g_lastBeacon = now;
		std::vector<BYTE> b = MakeSessionBody(0);
		for (int k = 0; k < kMaxSlots; ++k) {
			SendCore(MakeAddr(Ip(INADDR_BROADCAST), (uint16_t)(kCorePortBase + k * kPortStride)),
			         PacketType::SessionInfo, b.data(), b.size());
		}
		for (const auto& m : g_manual) {
			if (m.resolved && !m.id) {
				SendCore(m.core, PacketType::SessionInfo, b.data(), b.size());
			}
		}
		for (const auto& [id, p] : g_peers) {
			if (p.lastHeard && now - p.lastHeard < kPeerGoneMs) {
				SendCorePeer(p, PacketType::SessionInfo, b.data(), b.size());
			}
		}
	}
	// Steam: absorb finished lobby searches, look for friends' games now and then
	std::vector<LobbyEntry> lobbies;
	if (SteamLobbyResults(&g_lobbyStamp, &lobbies)) {
		for (const auto& l : lobbies) {
			SessionFromSteamLobby(l, false);
		}
	}
	if (SteamReady() && now - g_lastFriendScan > 15000) {
		g_lastFriendScan = now;
		for (const auto& f : SteamFriendsInGame()) {
			if (f.lobby) {
				SteamQueryLobby(f.lobby);
			}
		}
	}
	// QoS: resend probes, time out targets
	for (auto it = g_qosJobs.begin(); it != g_qosJobs.end();) {
		QosJob* job = *it;
		bool allDone = true;
		for (size_t i = 0; i < job->targets.size(); ++i) {
			auto& t = job->targets[i];
			if (t.done) {
				continue;
			}
			auto p = g_peers.find(t.peerId);
			if (p == g_peers.end() || (t.firstSent && now - t.firstSent > 2500)) {
				FinishQosTarget(*job, i, t.got > 0, false, nullptr, 0);
				continue;
			}
			allDone = false;
			if (t.sent < 4 && (!t.firstSent || now - t.firstSent > (ULONGLONG)t.sent * 300)) {
				QosBody b{};
				b.seq = ++g_qosSeq;
				b.sentTick = now;
				// the kid was stored in the probe template when the job started
				memcpy(&b.kid, job->q->axnqosinfo[i].pbData + kQosDataMax - sizeof(XNKID), sizeof(XNKID));
				SendCorePeer(p->second, PacketType::QosProbe, &b, sizeof(b));
				if (!t.firstSent) {
					t.firstSent = now;
				}
				t.sent++;
			}
		}
		if (allDone) {
			it = g_qosJobs.erase(it);  // the XNQOS block itself is freed by XNetQosRelease
		}
		else {
			++it;
		}
	}
	// expire stale sessions (Steam lobbies are refreshed by searches, so they live longer)
	for (auto it = g_remoteSessions.begin(); it != g_remoteSessions.end();) {
		ULONGLONG ttl = it->second.steamLobby ? 60000 : 12000;
		it = now - it->second.lastSeen > ttl ? g_remoteSessions.erase(it) : std::next(it);
	}
}

// A relayed frame from Steam: core packets are handled here, game packets go to the game's socket over loopback.
void OnRelayFrame(uint64_t from, const BYTE* data, uint32_t cb) {
	if (cb < 2 + sizeof(PacketHeader)) {
		return;
	}
	uint16_t dstPort;
	PacketHeader h;
	memcpy(&dstPort, data, 2);
	memcpy(&h, data + 2, sizeof(h));
	if (h.magic != kMagic || h.version != kVersion || h.instanceId == g_me.instanceId) {
		return;
	}
	if (dstPort == 0) {
		HandleCore(sockaddr_in{}, h, data + 2 + sizeof(h), (int)(cb - 2 - sizeof(h)), from);
		return;
	}
	{
		std::lock_guard lock(g_mutex);
		Peer& p = UpsertPeer(h.instanceId);
		ULONGLONG now = GetTickCount64();
		p.steamId = from;
		p.lastRelay = p.lastRelayGame = now;
		p.offset = h.srcOffset;
		if (p.lastDirectGame && now - p.lastDirectGame < kRelayDuplicateMs) {
			return;  // their UDP packets reach us: this is the relay's copy of one the game already has
		}
	}
	sockaddr_in to = MakeAddr(Ip(INADDR_LOOPBACK), RealGamePort(dstPort, g_me.portOffset));
	sendto(g_inject, (const char*)data + 2, (int)(cb - 2), 0, (const sockaddr*)&to, sizeof(to));
}

void Worker() {
	BYTE buf[2048];
	ULONGLONG lastTick = 0;
	while (!g_stop) {
		fd_set rd;
		FD_ZERO(&rd);
		FD_SET(g_core, &rd);
		// relayed packets wait in Steam until we poll: poll often while Steam is on
		timeval tv{0, (SteamReady() ? 5 : 100) * 1000};
		SteamTick();
		MatchmakingTick();
		if (select(0, &rd, nullptr, nullptr, &tv) > 0) {
			sockaddr_in from{};
			int fl = sizeof(from);
			int n = recvfrom(g_core, (char*)buf, sizeof(buf), 0, (sockaddr*)&from, &fl);
			if (n >= (int)sizeof(PacketHeader)) {
				PacketHeader h;
				memcpy(&h, buf, sizeof(h));
				if (h.magic == kMagic && h.version == kVersion) {
					HandleCore(from, h, buf + sizeof(h), n - (int)sizeof(h));
				}
			}
		}
		ULONGLONG now = GetTickCount64();
		if (now - lastTick >= 100) {
			lastTick = now;
			Tick();
		}
	}
}
}  // namespace

// ---- public API ---------------------------------------------------------------------------------------------------
const LocalIdentity& Me() {
	return g_me;
}

bool NetStarted() {
	return g_started;
}

bool NetStartup() {
	static std::once_flag once;
	std::call_once(once, [] {
		WSADATA wsa;
		WSAStartup(MAKEWORD(2, 2), &wsa);
		int slot = 0;
		for (; slot < kMaxSlots; ++slot) {
			wchar_t name[64];
			swprintf_s(name, L"Local\\LP2Net.Slot.%d", slot);
			HANDLE m = CreateMutexW(nullptr, TRUE, name);
			if (m && GetLastError() != ERROR_ALREADY_EXISTS) {
				g_slotMutex = m;
				break;
			}
			if (m) {
				CloseHandle(m);
			}
		}
		slot = std::min(slot, kMaxSlots - 1);
		const Config& c = Cfg();
		g_me.slot = slot;
		g_me.portOffset = (uint16_t)(slot * kPortStride);
		do {
			RandomBytes(&g_me.instanceId, sizeof(g_me.instanceId));
		} while ((g_me.instanceId & 0x00FFFFFF) == 0);
		g_me.name = c.name;
		g_me.xuid = c.xuid;
		if (SteamStart() && c.steamName) {
			std::string steamName = SanitizeGamertag(SteamMyName());
			if (!steamName.empty()) {
				g_me.name = steamName;
			}
		}
		if (slot > 0) {
			// a second copy on the same PC is a different player
			g_me.name = g_me.name.substr(0, 12) + " " + std::to_string(slot + 1);
			g_me.xuid = c.xuid ^ ((ULONGLONG)slot << 32);
		}
		g_me.lan = ChooseLanAddress();
		g_me.wan = g_me.lan;
		g_me.vaddr = MakeVaddr(g_me.instanceId);
		if (!c.publicAddress.empty()) {
			inet_pton(AF_INET, c.publicAddress.c_str(), &g_me.wan);
		}

		g_core = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
		BOOL yes = TRUE;
		setsockopt(g_core, SOL_SOCKET, SO_BROADCAST, (const char*)&yes, sizeof(yes));
		sockaddr_in bindAddr = MakeAddr(Ip(INADDR_ANY), (uint16_t)(kCorePortBase + g_me.portOffset));
		if (bind(g_core, (sockaddr*)&bindAddr, sizeof(bindAddr)) != 0) {
			LOG("core socket: bind %u failed (%d)", ntohs(bindAddr.sin_port), WSAGetLastError());
		}
		for (const auto& p : c.peers) {
			g_manual.push_back(ManualPeer{p});
		}
		g_inject = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
		sockaddr_in injectAddr = MakeAddr(Ip(INADDR_LOOPBACK), 0);
		int il = sizeof(injectAddr);
		if (bind(g_inject, (sockaddr*)&injectAddr, sizeof(injectAddr)) == 0 &&
		    getsockname(g_inject, (sockaddr*)&injectAddr, &il) == 0) {
			g_injectPort = ntohs(injectAddr.sin_port);
		}
		SteamSetRelayHandler(OnRelayFrame);
		SteamSetLobbyHandler(SessionFromSteamLobby);
		UpnpSetPublicIpCallback(SetPublicIp);
		UpnpMapUdp((uint16_t)(kCorePortBase + g_me.portOffset));
		g_worker = CreateThread(nullptr, 0, [](void*) -> DWORD { Worker(); return 0; }, nullptr, 0, nullptr);
		g_started = true;
		LOG("net: slot %d, instance %08X, name '%s', xuid %016llX, lan %s, core port %u", slot, g_me.instanceId,
		    g_me.name.c_str(), g_me.xuid, AddrToString(MakeAddr(g_me.lan, 0)).c_str(),
		    kCorePortBase + g_me.portOffset);
	});
	return g_started;
}

void NetShutdown() {
	if (!g_started.exchange(false)) {
		return;
	}
	g_stop = true;
	if (g_worker) {
		WaitForSingleObject(g_worker, 3000);
		CloseHandle(g_worker);
		g_worker = nullptr;
	}
	closesocket(g_core);
	g_core = INVALID_SOCKET;
	closesocket(g_inject);
	g_inject = INVALID_SOCKET;
	SteamStop();
	UpnpUnmapAll();
	if (g_slotMutex) {
		CloseHandle(g_slotMutex);
	}
}

XNADDR LocalXnAddr() {
	XNADDR x{};
	x.ina = g_me.lan;
	x.inaOnline = g_me.wan;
	x.wPortOnline = htons((uint16_t)(kCorePortBase + g_me.portOffset));
	memcpy(x.abEnet, &g_me.instanceId, 4);
	memcpy(x.abEnet + 4, &g_me.portOffset, 2);
	memcpy(x.abOnline, "LP2Net", 6);
	memcpy(x.abOnline + 6, &g_me.instanceId, 4);
	memcpy(x.abOnline + 10, &g_me.xuid, 8);
	return x;
}

bool XnAddrIsLocal(const XNADDR& x) {
	uint32_t id;
	memcpy(&id, x.abEnet, 4);
	return id == g_me.instanceId;
}

bool XnAddrToVirtual(const XNADDR& x, IN_ADDR* out) {
	uint32_t id;
	uint16_t offset;
	memcpy(&id, x.abEnet, 4);
	memcpy(&offset, x.abEnet + 4, 2);
	if (id == 0) {
		LOG("XnAddrToVirtual: not an LP2Net address (%s)", XnAddrToString(x).c_str());
		return false;
	}
	if (id == g_me.instanceId) {
		*out = g_me.vaddr;
		return true;
	}
	std::lock_guard lock(g_mutex);
	Peer& p = UpsertPeer(id);
	p.xnaddr = x;
	p.haveXnaddr = true;
	p.offset = offset;
	if (!p.steamId) {
		// a session we found through Steam tells us the host's Steam account
		for (const auto& [sid, rs] : g_remoteSessions) {
			if (sid == id && rs.hostSteamId) {
				p.steamId = rs.hostSteamId;
			}
		}
	}
	if (!p.lastHeard) {
		p.addr = GuessAddr(x);  // not heard from them yet
		SendHelloPeer(p, PacketType::Hello);
	}
	*out = p.vaddr;
	return true;
}

bool VirtualToXnAddr(IN_ADDR v, XNADDR* out) {
	if (v.s_addr == g_me.vaddr.s_addr) {
		*out = LocalXnAddr();
		return true;
	}
	std::lock_guard lock(g_mutex);
	Peer* p = PeerByVaddr(v);
	if (!p) {
		return false;
	}
	if (p->haveXnaddr) {
		*out = p->xnaddr;
		return true;
	}
	// only seen their packets so far: describe them from what we observed
	XNADDR x{};
	x.ina = x.inaOnline = p->addr;
	x.wPortOnline = htons((uint16_t)(kCorePortBase + p->offset));
	memcpy(x.abEnet, &p->id, 4);
	memcpy(x.abEnet + 4, &p->offset, 2);
	*out = x;
	return true;
}

bool IsVirtualAddr(IN_ADDR a) {
	if (a.s_addr == g_me.vaddr.s_addr) {
		return true;
	}
	std::lock_guard lock(g_mutex);
	return g_byVaddr.count(a.s_addr) != 0;
}

void ForgetVirtual(IN_ADDR) {
	// Mappings are kept for the whole run: late packets from a peer must still map to the same address.
}

void RegisterKey(const XNKID* id, const XNKEY* key) {
	std::lock_guard lock(g_mutex);
	if (id && key) {
		g_keys[KidKey(id)] = *key;
	}
}

void UnregisterKey(const XNKID* id) {
	std::lock_guard lock(g_mutex);
	if (id) {
		g_keys.erase(KidKey(id));
	}
}

std::vector<SendTarget> ResolveSend(const sockaddr_in& dst, bool* isBroadcast) {
	std::vector<SendTarget> out;
	uint16_t port = ntohs(dst.sin_port);
	uint32_t ip = ntohl(dst.sin_addr.s_addr);
	*isBroadcast = ip == INADDR_BROADCAST || (ip & 0xFF) == 0xFF;
	ULONGLONG now = GetTickCount64();
	std::lock_guard lock(g_mutex);
	auto toPeer = [&](const Peer& p) {
		if (UseDirect(p, now)) {
			auto s = p.seen.find(port);
			out.push_back({s != p.seen.end() ? s->second : MakeAddr(p.addr, RealGamePort(port, p.offset))});
		}
		if (UseRelay(p, now, true)) {
			out.push_back({dst, p.id});
		}
	};
	if (*isBroadcast) {
		// LAN (every local slot), plus everyone we know about directly or through Steam: this is what lets the
		// game's own System Link search find hosts over the internet.
		std::set<std::pair<uint32_t, uint16_t>> seen;
		auto add = [&](IN_ADDR a, uint16_t p) {
			if (seen.insert({a.s_addr, p}).second) {
				out.push_back({MakeAddr(a, p)});
			}
		};
		for (int k = 0; k < kMaxSlots; ++k) {
			add(Ip(INADDR_BROADCAST), RealGamePort(port, (uint16_t)(k * kPortStride)));
		}
		for (const auto& m : g_manual) {
			if (m.resolved && !m.id) {
				add(m.core.sin_addr, RealGamePort(port, (uint16_t)(ntohs(m.core.sin_port) - kCorePortBase)));
			}
		}
		for (const auto& [id, p] : g_peers) {
			if (p.lastHeard) {
				if (UseDirect(p, now)) {
					auto s = p.seen.find(port);
					s != p.seen.end() ? add(s->second.sin_addr, ntohs(s->second.sin_port))
					                  : add(p.addr, RealGamePort(port, p.offset));
				}
				if (UseRelay(p, now, true)) {
					out.push_back({dst, p.id});
				}
			}
		}
		return out;
	}
	if (dst.sin_addr.s_addr == g_me.vaddr.s_addr) {
		out.push_back({MakeAddr(Ip(INADDR_LOOPBACK), RealGamePort(port, g_me.portOffset))});
		return out;
	}
	if (Peer* p = PeerByVaddr(dst.sin_addr)) {
		toPeer(*p);
		return out;
	}
	out.push_back({dst});  // a real address the game got some other way: send as is
	return out;
}

bool RelaySendGame(uint32_t peerId, uint16_t dstGamePort, const void* packet, int cb) {
	uint64_t steamId = 0;
	{
		std::lock_guard lock(g_mutex);
		auto it = g_peers.find(peerId);
		if (it == g_peers.end() || !it->second.steamId) {
			return false;
		}
		steamId = it->second.steamId;
	}
	if (cb < (int)sizeof(PacketHeader)) {
		return false;
	}
	PacketHeader h;
	memcpy(&h, packet, sizeof(h));
	return RelayFrame(steamId, dstGamePort, h, (const BYTE*)packet + sizeof(h), cb - sizeof(h));
}

// A broadcast can reach a peer twice (LAN broadcast + direct copy to a known peer); keep the first.
static bool DuplicateBroadcast(const PacketHeader& h, uint32_t payloadHash) {
	struct Recent {
		uint32_t id;
		uint16_t port;
		uint32_t hash;
		ULONGLONG at;
	};
	static std::vector<Recent> recent;
	ULONGLONG now = GetTickCount64();
	std::erase_if(recent, [&](const Recent& r) { return now - r.at > 100; });
	for (const auto& r : recent) {
		if (r.id == h.instanceId && r.port == h.srcPort && r.hash == payloadHash) {
			return true;
		}
	}
	recent.push_back({h.instanceId, h.srcPort, payloadHash, now});
	return false;
}

bool AcceptIncoming(const sockaddr_in& realFrom, const PacketHeader& h, uint32_t payloadHash, sockaddr_in* gameFrom) {
	if (h.magic != kMagic || h.version != kVersion ||
	    (h.type != PacketType::Game && h.type != PacketType::GameBcast)) {
		return false;
	}
	if (h.instanceId == g_me.instanceId) {
		if (h.type == PacketType::GameBcast) {
			return false;  // our own broadcast coming back
		}
		*gameFrom = MakeAddr(g_me.vaddr, h.srcPort);
		return true;
	}
	std::lock_guard lock(g_mutex);
	if (h.type == PacketType::GameBcast && DuplicateBroadcast(h, payloadHash)) {
		return false;
	}
	Peer& p = UpsertPeer(h.instanceId);
	ULONGLONG now = GetTickCount64();
	bool relayed = realFrom.sin_addr.s_addr == htonl(INADDR_LOOPBACK) && ntohs(realFrom.sin_port) == g_injectPort &&
	               g_injectPort;
	p.offset = h.srcOffset;
	p.lastHeard = now;
	if (relayed) {
		p.lastRelay = now;  // came through Steam (OnRelayFrame): there is no real address to learn
	}
	else {
		bool firstContact = p.seen.empty();
		p.seen[h.srcPort] = realFrom;
		p.addr = realFrom.sin_addr;
		p.lastDirect = p.lastDirectGame = now;
		if (firstContact && !p.lastHelloSent) {
			SendHelloPeer(p, PacketType::Hello);
		}
	}
	*gameFrom = MakeAddr(p.vaddr, h.srcPort);
	return true;
}

sockaddr_in GameToReal(const sockaddr_in& game) {
	bool bcast;
	auto v = ResolveSend(game, &bcast);
	for (const auto& t : v) {
		if (!t.relayPeer) {
			return t.addr;
		}
	}
	return game;
}

sockaddr_in RealToGame(const sockaddr_in& real) {
	std::lock_guard lock(g_mutex);
	for (const auto& [id, p] : g_peers) {
		for (const auto& [gp, ep] : p.seen) {
			if (ep.sin_addr.s_addr == real.sin_addr.s_addr && ep.sin_port == real.sin_port) {
				return MakeAddr(p.vaddr, gp);
			}
		}
	}
	for (const auto& [id, p] : g_peers) {
		if (p.addr.s_addr == real.sin_addr.s_addr) {
			return MakeAddr(p.vaddr, (uint16_t)(ntohs(real.sin_port) - kGamePortShift - p.offset));
		}
	}
	return real;
}

void QosListen(const XNKID* xnkid, const BYTE* data, UINT cb, DWORD flags) {
	if (!xnkid) {
		return;
	}
	std::lock_guard lock(g_mutex);
	ULONGLONG k = KidKey(xnkid);
	if (flags & XNET_QOS_LISTEN_RELEASE) {
		g_qosListen.erase(k);
		return;
	}
	QosListenEntry& e = g_qosListen[k];
	if (flags & XNET_QOS_LISTEN_ENABLE) {
		e.enabled = true;
	}
	if (flags & XNET_QOS_LISTEN_DISABLE) {
		e.enabled = false;
	}
	if ((flags & XNET_QOS_LISTEN_SET_DATA) && data) {
		e.data.assign(data, data + cb);
	}
	LOG("QosListen(kid=%016llX, cb=%u, flags=0x%lX) enabled=%d", k, cb, flags, e.enabled);
}

XNQOS* QosLookup(UINT count, const XNADDR* const* addrs, const XNKID* const* kids, WSAEVENT hEvent) {
	size_t entries = std::max<UINT>(count, 1);
	size_t size = sizeof(XNQOS) + (entries - 1) * sizeof(XNQOSINFO) + entries * kQosDataMax;
	auto* q = (XNQOS*)calloc(1, size);
	q->cxnqos = count;
	q->cxnqosPending = count;
	BYTE* data = (BYTE*)&q->axnqosinfo[entries];
	auto* job = new QosJob{q, hEvent};
	std::lock_guard lock(g_mutex);
	for (UINT i = 0; i < count; ++i) {
		XNQOSINFO& e = q->axnqosinfo[i];
		e.pbData = data + i * kQosDataMax;
		IN_ADDR v{};
		QosJob::Target t{};
		if (addrs && addrs[i] && XnAddrToVirtual(*addrs[i], &v) && !XnAddrIsLocal(*addrs[i])) {
			memcpy(&t.peerId, addrs[i]->abEnet, 4);
		}
		// stash the session id at the end of this entry's data area for the probe packets
		if (kids && kids[i]) {
			memcpy(e.pbData + kQosDataMax - sizeof(XNKID), kids[i], sizeof(XNKID));
		}
		job->targets.push_back(t);
	}
	g_qosJobs.push_back(job);
	for (UINT i = 0; i < count; ++i) {
		if (!job->targets[i].peerId) {
			FinishQosTarget(*job, i, false, false, nullptr, 0);
		}
	}
	if (count == 0 && hEvent) {
		WSASetEvent(hEvent);
	}
	LOG("QosLookup(%u targets)", count);
	return q;
}

void QosRelease(XNQOS* q) {
	if (!q) {
		return;
	}
	std::lock_guard lock(g_mutex);
	for (auto it = g_qosJobs.begin(); it != g_qosJobs.end(); ++it) {
		if ((*it)->q == q) {
			delete *it;
			g_qosJobs.erase(it);
			break;
		}
	}
	free(q);
}

void SetAdvertisedSession(const AdvertisedSession* s) {
	std::lock_guard lock(g_mutex);
	bool was = g_hosting;
	g_hosting = s != nullptr;
	if (s) {
		g_advert = *s;
		g_lastBeacon = 0;  // beacon on the next tick
		// Steam lobby: LIVE-style sessions are listed for matchmaking; System Link ones are for friends (Join Game,
		// invites) only
		LobbyInfo li;
		li.data = MakeLobbyData();
		li.maxMembers = std::max(2, s->maxPublic + s->maxPrivate);
		li.listed = (s->flags & XSESSION_CREATE_USES_MATCHMAKING) != 0;
		SteamHostLobby(&li);
	}
	else {
		SteamHostLobby(nullptr);
	}
	if (was != g_hosting) {
		LOG("hosting: %s", g_hosting ? "yes" : "no");
	}
}

// A hosted session described by Steam lobby data (lobby search, a friend's lobby, an invite or Join Game).
void SessionFromSteamLobby(const LobbyEntry& lobby, bool joinNow) {
	auto get = [&](const char* k) {
		auto it = lobby.data.find(k);
		return it == lobby.data.end() ? std::string() : it->second;
	};
	std::vector<BYTE> sinfo, attrs;
	if (get("lp2net") != "1" || !FromHex(get("sinfo"), &sinfo) || sinfo.size() != sizeof(XSESSION_INFO)) {
		LOG("steam: lobby %llu is not an LP2Net session", lobby.id);
		return;
	}
	FromHex(get("attrs"), &attrs);
	XSESSION_INFO info;
	memcpy(&info, sinfo.data(), sizeof(info));
	uint32_t inst = 0;
	memcpy(&inst, info.hostAddress.abEnet, 4);
	if (!inst || inst == g_me.instanceId) {
		return;
	}
	unsigned long flags = 0, gameType = 0, gameMode = 0;
	int fp = 0, fv = 0, mp = 0, mv = 0, ing = 0;
	sscanf_s(get("slots").c_str(), "%lu %lu %lu %d %d %d %d %d", &flags, &gameType, &gameMode, &fp, &fv, &mp, &mv,
	         &ing);
	std::string hostName = get("name");
	{
		std::lock_guard lock(g_mutex);
		RemoteSession& rs = g_remoteSessions[inst];
		bool fresherLan = rs.lastSeen && !rs.steamLobby && GetTickCount64() - rs.lastSeen < 12000;
		if (!fresherLan) {  // a LAN/peer announcement is more current than lobby data
			rs.id = inst;
			rs.host = hostName;
			rs.s.info = info;
			rs.s.flags = flags;
			rs.s.gameType = gameType;
			rs.s.gameMode = gameMode;
			rs.s.filledPublic = fp;
			rs.s.filledPrivate = fv;
			rs.s.maxPublic = mp;
			rs.s.maxPrivate = mv;
			rs.s.players = fp + fv;
			rs.s.maxPlayers = mp + mv;
			rs.s.inGame = ing != 0;
			rs.s.attrs = attrs;
			rs.core = MakeAddr(info.hostAddress.inaOnline, ntohs(info.hostAddress.wPortOnline));
			rs.lan = false;
		}
		rs.steamLobby = lobby.id;
		rs.hostSteamId = lobby.owner;
		rs.lastSeen = GetTickCount64();
		// introduce the host so the game's own System Link search (and its QoS probes) reach them
		Peer& p = UpsertPeer(inst);
		p.steamId = lobby.owner;
		if (!p.haveXnaddr) {
			p.xnaddr = info.hostAddress;
			p.haveXnaddr = true;
			uint16_t offset;
			memcpy(&offset, info.hostAddress.abEnet + 4, 2);
			p.offset = offset;
			p.addr = GuessAddr(info.hostAddress);
		}
		if (!p.lastHeard && (!p.lastHelloSent || GetTickCount64() - p.lastHelloSent > 2000)) {
			SendHelloPeer(p, PacketType::Hello);
		}
	}
	if (joinNow) {
		std::string mode = get("mode");
		JoinSessionFromSteam(info, flags, lobby.owner, hostName, mode.empty() ? 0 : strtoul(mode.c_str(), nullptr, 10));
	}
}

ULONGLONG SessionsStamp() {
	return g_lobbyStamp;
}

void SendStatsToPeers(const std::vector<std::vector<BYTE>>& packets) {
	if (packets.empty()) {
		return;
	}
	std::lock_guard lock(g_mutex);
	ULONGLONG now = GetTickCount64();
	for (const auto& [id, p] : g_peers) {
		if (p.name.empty() || now - p.lastHeard > kPeerGoneMs) {
			continue;
		}
		for (const auto& s : packets) {
			SendCorePeer(p, PacketType::Stats, s.data(), s.size());
		}
	}
}

std::vector<PeerView> ListPeers() {
	std::lock_guard lock(g_mutex);
	std::vector<PeerView> out;
	ULONGLONG now = GetTickCount64();
	std::set<uint32_t> listed;
	for (const auto& m : g_manual) {
		PeerView v;
		v.manual = true;
		v.endpoint = m.text;
		auto it = m.id ? g_peers.find(m.id) : g_peers.end();
		if (it != g_peers.end()) {
			v.name = it->second.name;
			v.pingMs = it->second.pingMs;
			v.alive = now - it->second.lastHeard < 15000;
			listed.insert(m.id);
		}
		out.push_back(v);
	}
	for (const auto& [id, p] : g_peers) {
		if (listed.count(id) || !p.lastHeard) {
			continue;
		}
		PeerView v;
		v.name = p.name;
		v.endpoint = AddrToString(PeerCore(p));
		v.pingMs = p.pingMs;
		v.alive = now - p.lastHeard < 15000;
		out.push_back(v);
	}
	return out;
}

std::vector<SessionView> ListSessions() {
	std::lock_guard lock(g_mutex);
	std::vector<SessionView> out;
	for (const auto& [id, rs] : g_remoteSessions) {
		SessionView v;
		v.host = rs.host;
		v.endpoint = rs.steamLobby && !rs.lan ? "Steam" : AddrToString(rs.core);
		v.players = rs.s.players;
		v.maxPlayers = rs.s.maxPlayers;
		v.gameType = rs.s.gameType;
		v.gameMode = rs.s.gameMode;
		v.pingMs = rs.pingMs;
		v.lan = rs.lan;
		v.info = rs.s.info;
		v.flags = rs.s.flags;
		v.filledPublic = rs.s.filledPublic;
		v.filledPrivate = rs.s.filledPrivate;
		v.maxPublic = rs.s.maxPublic;
		v.maxPrivate = rs.s.maxPrivate;
		v.inGame = rs.s.inGame;
		v.attrs = rs.s.attrs;
		v.steamLobby = rs.steamLobby;
		v.hostSteamId = rs.hostSteamId;
		out.push_back(v);
	}
	return out;
}

void RefreshSessions() {
	{
		std::lock_guard lock(g_mutex);
		ULONGLONG now = GetTickCount64();
		g_lastQuery = now;
		for (const auto& t : CoreTargets()) {
			SendCore(t, PacketType::SessionQuery, &now, sizeof(now));
		}
		for (const auto& [id, p] : g_peers) {
			if (p.lastHeard && UseRelay(p, now, false)) {
				SendCorePeer(p, PacketType::SessionQuery, &now, sizeof(now));  // UDP not confirmed: Steam too
			}
		}
	}
	SteamRequestLobbies();
}

void AddManualPeer(const std::string& hostPort) {
	{
		std::lock_guard lock(g_mutex);
		for (const auto& m : g_manual) {
			if (m.text == hostPort) {
				return;
			}
		}
		ManualPeer m{hostPort};
		ResolveManual(m);
		if (m.resolved) {
			SendHello(m.core, PacketType::Hello, nullptr);
			ULONGLONG now = GetTickCount64();
			SendCore(m.core, PacketType::SessionQuery, &now, sizeof(now));
		}
		g_manual.push_back(m);
	}
	Cfg().peers.push_back(hostPort);
	SaveConfig();
}

void RemoveManualPeer(const std::string& hostPort) {
	{
		std::lock_guard lock(g_mutex);
		std::erase_if(g_manual, [&](const ManualPeer& m) { return m.text == hostPort; });
	}
	std::erase(Cfg().peers, hostPort);
	SaveConfig();
}

std::string NetStatusLine() {
	char lan[INET_ADDRSTRLEN], wan[INET_ADDRSTRLEN];
	inet_ntop(AF_INET, &g_me.lan, lan, sizeof(lan));
	inet_ntop(AF_INET, &g_me.wan, wan, sizeof(wan));
	char buf[256];
	snprintf(buf, sizeof(buf), "%s  |  LAN %s  |  Internet %s  |  UDP %u%s  |  %s", g_me.name.c_str(), lan,
	         g_me.wan.s_addr == g_me.lan.s_addr ? "unknown" : wan, kCorePortBase + g_me.portOffset,
	         g_me.slot ? (" (copy " + std::to_string(g_me.slot + 1) + ")").c_str() : "", UpnpStatus().c_str());
	return buf;
}

}  // namespace lp2net
