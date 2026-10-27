// LP2Net core: instance identity, peers, virtual addresses and the LP2Net packet header.
//
// Every game datagram we send carries a small header with the sender's instance id and the game port it was
// sent from. The receiver uses it to hand the game a stable *virtual* address for that peer (what GFWL called
// a secure address) no matter which real IP/port/NAT mapping the packet arrived from. Several game instances on
// one PC get different port offsets, so they can run side by side.
#pragma once

#include "gfwl.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace lp2net {

constexpr int kMaxSlots = 8;            // game instances per PC
constexpr uint16_t kPortStride = 100;   // per-slot port offset
constexpr uint16_t kCorePortBase = 3075;  // LP2Net control socket (discovery, QoS, keepalive) + slot offset
// LP2's own ports (1000, 1001) are below 1024, and home routers refuse UPnP for those, so game traffic travels on
// game port + 10000 + slot offset (11000/11001 for the first copy). The game only ever sees its own port numbers.
constexpr uint16_t kGamePortShift = 10000;

inline uint16_t RealGamePort(uint16_t gamePort, uint16_t slotOffset) {
	return (uint16_t)(gamePort + kGamePortShift + slotOffset);
}

struct LocalIdentity {
	std::string name;
	XUID xuid = 0;
	int slot = 0;
	uint32_t instanceId = 0;  // random per launch, never 0
	uint16_t portOffset = 0;  // slot * kPortStride
	IN_ADDR lan{};            // advertised LAN address
	IN_ADDR wan{};            // advertised public address (LAN address until learned)
	IN_ADDR vaddr{};          // our own virtual address
};
const LocalIdentity& Me();

bool NetStartup();
void NetShutdown();
bool NetStarted();

XNADDR LocalXnAddr();
bool XnAddrIsLocal(const XNADDR& x);
// XNADDR <-> virtual IN_ADDR handed to the game
bool XnAddrToVirtual(const XNADDR& x, IN_ADDR* out);
bool VirtualToXnAddr(IN_ADDR v, XNADDR* out);
bool IsVirtualAddr(IN_ADDR a);
void ForgetVirtual(IN_ADDR v);

// ---- packet header -------------------------------------------------------------------------------------------
enum class PacketType : uint8_t {
	Game = 0,       // game datagram, unicast
	GameBcast = 1,  // game datagram the title broadcast
	Hello = 2,      // core: "I exist" (+ name, XNADDR); answered with HelloAck
	HelloAck = 3,
	QosProbe = 4,
	QosReply = 5,
	SessionQuery = 6,  // core: ask hosts to describe their session
	SessionInfo = 7,   // core: session beacon / answer
};

#pragma pack(push, 1)
struct PacketHeader {
	uint16_t magic;  // kMagic
	uint8_t version;
	PacketType type;
	uint32_t instanceId;  // sender
	uint16_t srcPort;     // sender's *game* port (host order)
	uint16_t srcOffset;   // sender's port offset, so replies to other game ports can be computed
};
#pragma pack(pop)
constexpr uint16_t kMagic = 0x324C;  // "L2"
constexpr uint8_t kVersion = 1;
constexpr int kMaxPayload = 1400;

// ---- translation used by the XSocket layer ---------------------------------------------------------------------
// Where a game datagram addressed to `dst` (as the game sees it) must really go. Broadcasts fan out. A target with
// relayPeer set goes through Steam to that peer instead of the UDP socket.
struct SendTarget {
	sockaddr_in addr{};
	uint32_t relayPeer = 0;
};
std::vector<SendTarget> ResolveSend(const sockaddr_in& dst, bool* isBroadcast);
// Send one framed game datagram (header + payload) to a peer through the Steam relay; dstGamePort as the game sees it.
bool RelaySendGame(uint32_t peerId, uint16_t dstGamePort, const void* packet, int cb);
// Account for an incoming datagram and produce the address the game should see as its source.
// Returns false for packets that must be dropped (not ours, our own broadcast echo, duplicate broadcast, bad header).
bool AcceptIncoming(const sockaddr_in& realFrom, const PacketHeader& h, uint32_t payloadHash, sockaddr_in* gameFrom);
// Map a real address (TCP accept/getpeername) back to what the game should see.
sockaddr_in RealToGame(const sockaddr_in& real);
sockaddr_in GameToReal(const sockaddr_in& game);

// ---- QoS (XNetQosListen / XNetQosLookup) ----------------------------------------------------------------------
void QosListen(const XNKID* xnkid, const BYTE* data, UINT cb, DWORD flags);
XNQOS* QosLookup(UINT count, const XNADDR* const* addrs, const XNKID* const* kids, WSAEVENT hEvent);
void QosRelease(XNQOS* q);

// session keys (XNetRegisterKey); kept for bookkeeping, traffic is not encrypted
void RegisterKey(const XNKID* id, const XNKEY* key);
void UnregisterKey(const XNKID* id);

// ---- sessions and peers, for xsession.cpp and the overlay ----------------------------------------------------
struct AdvertisedSession {
	XSESSION_INFO info;
	DWORD flags = 0;
	DWORD gameType = 0, gameMode = 0;
	int players = 0, maxPlayers = 0;
	int filledPublic = 0, filledPrivate = 0, maxPublic = 0, maxPrivate = 0;
	bool inGame = false;
	std::vector<BYTE> attrs;  // PackAttrs (matchmaking.h)
};
void SetAdvertisedSession(const AdvertisedSession* s);  // nullptr = not hosting

struct PeerView {
	std::string name;
	std::string endpoint;
	int pingMs = -1;
	bool manual = false;
	bool alive = false;
};
std::vector<PeerView> ListPeers();

struct SessionView {
	std::string host;
	std::string endpoint;
	int players = 0, maxPlayers = 0;
	DWORD gameType = 0, gameMode = 0;
	int pingMs = -1;
	bool lan = false;
	XSESSION_INFO info{};
	// for matchmaking
	DWORD flags = 0;
	int filledPublic = 0, filledPrivate = 0, maxPublic = 0, maxPrivate = 0;
	bool inGame = false;
	std::vector<BYTE> attrs;
	uint64_t steamLobby = 0;  // found through Steam
	uint64_t hostSteamId = 0;
};
std::vector<SessionView> ListSessions();
void RefreshSessions();     // ask the LAN, peers and Steam (lobby search) for sessions now
ULONGLONG SessionsStamp();  // changes whenever a Steam lobby search finished
// The player picked a session from Steam (invite / Join Game): join it the way the game expects (xuser.cpp).
void JoinSessionFromSteam(const XSESSION_INFO& info, DWORD flags, uint64_t hostSteamId, const std::string& hostName);
// Steam lobby data describing a hosted session; joinNow = the player picked it from Steam (invite / Join Game).
struct LobbyEntry;  // steam.h
void SessionFromSteamLobby(const LobbyEntry& lobby, bool joinNow);

void AddManualPeer(const std::string& hostPort);
void RemoveManualPeer(const std::string& hostPort);
std::string NetStatusLine();  // one-line summary for the overlay header

// handles (xhandle.cpp)
HANDLE NewFakeHandle();
HANDLE CreateEnumerator(DWORD itemSize, std::vector<std::vector<BYTE>> items);
HANDLE CreateEmptyEnumerator();

// user contexts (xuser.cpp)
DWORD GetUserContext(DWORD id, DWORD def);
XUID MyOnlineXuid();    // signed in online: from the Steam account when Steam runs
XUID MySessionXuid();   // what session member lists hold for the local player

}  // namespace lp2net
