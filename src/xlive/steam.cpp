// Steam integration without steam_api.dll. steam_api.dll is a thin loader around the Steam client's own DLL
// (steamclient.dll, part of every Steam install): it calls CreateInterface for the versioned ISteamClient, opens a
// pipe, connects to the logged-in user and pumps callbacks with Steam_BGetCallback. We do the same, so nothing from
// Valve ships with the mod. The interface layouts below follow Valve's published Steamworks headers for exactly the
// versions we ask for (SteamClient020, SteamUser023, SteamFriends017, SteamMatchMaking009, SteamNetworking006,
// SteamUtils010); unused slots are placeholders so the vtable indices line up.
#include "steam.h"

#include "config.h"
#include "log.h"
#include "xnotify.h"

#include <mutex>
#include <set>

namespace lp2net {
namespace {
constexpr uint32_t kAppId = 45750;  // Lost Planet 2 on Steam

// Returned through a hidden pointer by MSVC member functions, exactly like Valve's CSteamID class.
struct CSteamID {
	uint64_t v;
};
using HSteamPipe = int32_t;
using HSteamUser = int32_t;
using SteamAPICall_t = uint64_t;

// Placeholder slot: never called, only keeps the vtable layout.
#define SLOT(n) virtual void unused_##n() = 0

class ISteamClient {
public:
	virtual HSteamPipe CreateSteamPipe() = 0;                                                   // 0
	virtual bool BReleaseSteamPipe(HSteamPipe) = 0;                                              // 1
	virtual HSteamUser ConnectToGlobalUser(HSteamPipe) = 0;                                      // 2
	SLOT(3);                                                                                     // CreateLocalUser
	virtual void ReleaseUser(HSteamPipe, HSteamUser) = 0;                                        // 4
	virtual void* GetISteamUser(HSteamUser, HSteamPipe, const char*) = 0;                        // 5
	SLOT(6);                                                                                     // GetISteamGameServer
	SLOT(7);                                                                                     // SetLocalIPBinding
	virtual void* GetISteamFriends(HSteamUser, HSteamPipe, const char*) = 0;                     // 8
	virtual void* GetISteamUtils(HSteamPipe, const char*) = 0;                                   // 9
	virtual void* GetISteamMatchmaking(HSteamUser, HSteamPipe, const char*) = 0;                 // 10
	SLOT(11);
	SLOT(12);
	SLOT(13);
	SLOT(14);
	SLOT(15);
	virtual void* GetISteamNetworking(HSteamUser, HSteamPipe, const char*) = 0;                  // 16
};

class ISteamUser {
public:
	virtual HSteamUser GetHSteamUser() = 0;  // 0
	virtual bool BLoggedOn() = 0;            // 1
	virtual CSteamID GetSteamID() = 0;       // 2
	SLOT(3);
	SLOT(4);
	SLOT(5);
	SLOT(6);
	virtual void StartVoiceRecording() = 0;                                                  // 7
	virtual void StopVoiceRecording() = 0;                                                   // 8
	virtual int GetAvailableVoice(uint32_t* cbCompressed, uint32_t* unused, uint32_t unusedRate) = 0;  // 9
	virtual int GetVoice(bool wantCompressed, void* dest, uint32_t cbDest, uint32_t* written, bool wantRaw,
	                     void* rawDest, uint32_t cbRawDest, uint32_t* rawWritten, uint32_t rawRate) = 0;  // 10
	virtual int DecompressVoice(const void* compressed, uint32_t cb, void* dest, uint32_t cbDest, uint32_t* written,
	                            uint32_t sampleRate) = 0;                                    // 11
	virtual uint32_t GetVoiceOptimalSampleRate() = 0;                                        // 12
};

struct FriendGameInfo_t {
	uint64_t gameID;
	uint32_t gameIP;
	uint16_t gamePort;
	uint16_t queryPort;
	CSteamID lobby;
};
static_assert(sizeof(FriendGameInfo_t) == 24);

class ISteamFriends {
public:
	virtual const char* GetPersonaName() = 0;                                // 0
	SLOT(1);
	SLOT(2);
	virtual int GetFriendCount(int flags) = 0;                               // 3
	virtual CSteamID GetFriendByIndex(int i, int flags) = 0;                 // 4
	SLOT(5);
	SLOT(6);
	virtual const char* GetFriendPersonaName(CSteamID) = 0;                  // 7
	virtual bool GetFriendGamePlayed(CSteamID, FriendGameInfo_t*) = 0;       // 8
	SLOT(9);
	SLOT(10);
	SLOT(11);
	SLOT(12);
	SLOT(13);
	SLOT(14);
	SLOT(15);
	SLOT(16);
	SLOT(17);
	SLOT(18);
	SLOT(19);
	SLOT(20);
	SLOT(21);
	SLOT(22);
	SLOT(23);
	SLOT(24);
	SLOT(25);
	SLOT(26);
	SLOT(27);
	virtual void ActivateGameOverlay(const char* dialog) = 0;                // 28
	virtual void ActivateGameOverlayToUser(const char* dialog, CSteamID) = 0;  // 29
	SLOT(30);
	SLOT(31);
	virtual void SetPlayedWith(CSteamID) = 0;                                // 32
	virtual void ActivateGameOverlayInviteDialog(CSteamID lobby) = 0;        // 33
	SLOT(34);
	SLOT(35);
	SLOT(36);
	SLOT(37);
	SLOT(38);
	SLOT(39);
	SLOT(40);
	SLOT(41);
	SLOT(42);
	virtual bool SetRichPresence(const char* key, const char* value) = 0;   // 43
	virtual void ClearRichPresence() = 0;                                    // 44
};

class ISteamMatchmaking {
public:
	SLOT(0);
	SLOT(1);
	SLOT(2);
	SLOT(3);
	virtual SteamAPICall_t RequestLobbyList() = 0;                                          // 4
	virtual void AddRequestLobbyListStringFilter(const char* key, const char* value, int cmp) = 0;  // 5
	SLOT(6);
	SLOT(7);
	SLOT(8);
	virtual void AddRequestLobbyListDistanceFilter(int filter) = 0;                         // 9
	virtual void AddRequestLobbyListResultCountFilter(int max) = 0;                         // 10
	SLOT(11);
	virtual CSteamID GetLobbyByIndex(int i) = 0;                                            // 12
	virtual SteamAPICall_t CreateLobby(int type, int maxMembers) = 0;                       // 13
	virtual SteamAPICall_t JoinLobby(CSteamID) = 0;                                         // 14
	virtual void LeaveLobby(CSteamID) = 0;                                                  // 15
	SLOT(16);
	SLOT(17);
	SLOT(18);
	virtual const char* GetLobbyData(CSteamID, const char* key) = 0;                        // 19
	virtual bool SetLobbyData(CSteamID, const char* key, const char* value) = 0;            // 20
	virtual int GetLobbyDataCount(CSteamID) = 0;                                            // 21
	virtual bool GetLobbyDataByIndex(CSteamID, int i, char* key, int cbKey, char* value, int cbValue) = 0;  // 22
	SLOT(23);
	SLOT(24);
	SLOT(25);
	SLOT(26);
	SLOT(27);
	virtual bool RequestLobbyData(CSteamID) = 0;                                            // 28
	SLOT(29);
	SLOT(30);
	virtual bool SetLobbyMemberLimit(CSteamID, int) = 0;                                    // 31
	SLOT(32);
	virtual bool SetLobbyType(CSteamID, int type) = 0;                                      // 33
	virtual bool SetLobbyJoinable(CSteamID, bool) = 0;                                      // 34
	virtual CSteamID GetLobbyOwner(CSteamID) = 0;                                           // 35
};

class ISteamNetworking {
public:
	virtual bool SendP2PPacket(CSteamID to, const void* data, uint32_t cb, int sendType, int channel) = 0;  // 0
	virtual bool IsP2PPacketAvailable(uint32_t* cb, int channel) = 0;                                      // 1
	virtual bool ReadP2PPacket(void* dest, uint32_t cbDest, uint32_t* cb, CSteamID* from, int channel) = 0;  // 2
	virtual bool AcceptP2PSessionWithUser(CSteamID) = 0;                                                   // 3
	virtual bool CloseP2PSessionWithUser(CSteamID) = 0;                                                    // 4
	SLOT(5);
	SLOT(6);
	virtual bool AllowP2PPacketRelay(bool) = 0;                                                            // 7
};

class ISteamUtils {
public:
	SLOT(0);
	SLOT(1);
	SLOT(2);
	SLOT(3);
	SLOT(4);
	SLOT(5);
	SLOT(6);
	SLOT(7);
	SLOT(8);
	virtual uint32_t GetAppID() = 0;                                                         // 9
	SLOT(10);
	virtual bool IsAPICallCompleted(SteamAPICall_t, bool* failed) = 0;                       // 11
	SLOT(12);
	virtual bool GetAPICallResult(SteamAPICall_t, void* out, int cb, int expected, bool* failed) = 0;  // 13
	SLOT(14);
	SLOT(15);
	SLOT(16);
	virtual bool IsOverlayEnabled() = 0;                                                     // 17
};
#undef SLOT

struct CallbackMsg_t {
	HSteamUser user;
	int id;
	uint8_t* param;
	int cb;
};

// callback payloads (Windows packing: 8)
constexpr int kGameOverlayActivated = 331, kGameLobbyJoinRequested = 333, kGameRichPresenceJoinRequested = 337;
constexpr int kLobbyDataUpdate = 505, kLobbyMatchList = 510, kLobbyCreated = 513;
constexpr int kP2PSessionRequest = 1202, kP2PSessionConnectFail = 1203;
struct GameLobbyJoinRequested_t {
	CSteamID lobby, friendId;
};
struct GameRichPresenceJoinRequested_t {
	CSteamID friendId;
	char connect[256];
};
struct LobbyDataUpdate_t {
	uint64_t lobby, member;
	uint8_t success;
};
struct LobbyMatchList_t {
	uint32_t count;
};
struct LobbyCreated_t {
	int result;
	uint64_t lobby;
};
struct P2PSessionRequest_t {
	CSteamID remote;
};
struct P2PSessionConnectFail_t {
	CSteamID remote;
	uint8_t error;
};

constexpr int kLobbyFriendsOnly = 1, kLobbyPublic = 2;
constexpr int kLobbyCmpEqual = 0, kLobbyDistanceWorldwide = 3;
constexpr int kFriendFlagImmediate = 0x04;
constexpr int kP2PUnreliable = 0, kP2PReliable = 2;
constexpr uint32_t kP2PUnreliableMax = 1200;

using CreateInterfaceFn = void*(__cdecl*)(const char* version, int* returnCode);
using BGetCallbackFn = bool(__cdecl*)(HSteamPipe, CallbackMsg_t*);
using FreeLastCallbackFn = bool(__cdecl*)(HSteamPipe);

std::recursive_mutex g_mutex;
bool g_ready = false;
ISteamClient* g_client = nullptr;
ISteamUser* g_user = nullptr;
ISteamFriends* g_friends = nullptr;
ISteamMatchmaking* g_mm = nullptr;
ISteamNetworking* g_net = nullptr;
ISteamUtils* g_utils = nullptr;
HSteamPipe g_pipe = 0;
HSteamUser g_hUser = 0;
BGetCallbackFn g_getCallback = nullptr;
FreeLastCallbackFn g_freeCallback = nullptr;
uint64_t g_myId = 0;

RelayHandler g_relayHandler = nullptr;
LobbyHandler g_lobbyHandler = nullptr;

// call results we wait for
enum class Call { LobbyList, CreateLobby };
std::map<SteamAPICall_t, Call> g_calls;

// hosted lobby
uint64_t g_lobby = 0;
bool g_creatingLobby = false;
bool g_wantLobby = false;
LobbyInfo g_lobbyWanted;
std::map<std::string, std::string> g_lobbyPublished;

// lobby search
bool g_listPending = false;
ULONGLONG g_listStamp = 0;
std::vector<LobbyEntry> g_listResult;

// lobbies whose data we asked for
std::map<uint64_t, bool> g_lobbyWaiting;  // lobby -> asked for a join
std::vector<std::pair<LobbyEntry, bool>> g_lobbyArrived;

CSteamID Sid(uint64_t v) {
	return CSteamID{v};
}

std::wstring SteamClientPath() {
	wchar_t path[MAX_PATH] = L"";
	DWORD cb = sizeof(path);
	if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam\\ActiveProcess", L"SteamClientDll", RRF_RT_REG_SZ,
	                 nullptr, path, &cb) == ERROR_SUCCESS && path[0]) {
		return path;
	}
	cb = sizeof(path);
	if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath", RRF_RT_REG_SZ, nullptr, path, &cb) ==
	        ERROR_SUCCESS && path[0]) {
		return std::wstring(path) + L"\\steamclient.dll";
	}
	return L"";
}

LobbyEntry ReadLobby(uint64_t id) {
	LobbyEntry e;
	e.id = id;
	e.owner = g_mm->GetLobbyOwner(Sid(id)).v;
	int n = g_mm->GetLobbyDataCount(Sid(id));
	static char key[256], value[8192];
	for (int i = 0; i < n; ++i) {
		if (g_mm->GetLobbyDataByIndex(Sid(id), i, key, sizeof(key), value, sizeof(value))) {
			e.data[key] = value;
		}
	}
	return e;
}

void PublishLobbyData() {
	if (!g_lobby) {
		return;
	}
	for (const auto& [k, v] : g_lobbyWanted.data) {
		auto it = g_lobbyPublished.find(k);
		if (it == g_lobbyPublished.end() || it->second != v) {
			g_mm->SetLobbyData(Sid(g_lobby), k.c_str(), v.c_str());
			g_lobbyPublished[k] = v;
		}
	}
}

void OnCallback(const CallbackMsg_t& m) {
	switch (m.id) {
	case kP2PSessionRequest: {
		auto* r = (const P2PSessionRequest_t*)m.param;
		g_net->AcceptP2PSessionWithUser(r->remote);
		LOG("steam: relay session from %llu accepted", r->remote.v);
		break;
	}
	case kP2PSessionConnectFail: {
		auto* r = (const P2PSessionConnectFail_t*)m.param;
		LOG("steam: relay to %llu failed (error %u)", r->remote.v, r->error);
		break;
	}
	case kGameLobbyJoinRequested: {
		auto* r = (const GameLobbyJoinRequested_t*)m.param;
		LOG("steam: join requested: lobby %llu from friend %llu", r->lobby.v, r->friendId.v);
		SteamJoinLobbyById(r->lobby.v);
		break;
	}
	case kGameRichPresenceJoinRequested: {
		auto* r = (const GameRichPresenceJoinRequested_t*)m.param;
		LOG("steam: Join Game from friend %llu: '%.255s'", r->friendId.v, r->connect);
		unsigned long long lobby = 0;
		if (sscanf_s(r->connect, "+lp2net_lobby %llu", &lobby) == 1) {
			SteamJoinLobbyById(lobby);
		}
		break;
	}
	case kLobbyDataUpdate: {
		auto* r = (const LobbyDataUpdate_t*)m.param;
		auto it = g_lobbyWaiting.find(r->lobby);
		if (r->lobby == r->member && it != g_lobbyWaiting.end()) {
			bool join = it->second;
			g_lobbyWaiting.erase(it);
			LobbyEntry e = ReadLobby(r->lobby);
			// friends' lobbies are re-read every 15 s: log a lobby only when it is new, changed, or for a join
			static std::map<uint64_t, std::map<std::string, std::string>> logged;
			if (join || logged[r->lobby] != e.data) {
				logged[r->lobby] = e.data;
				LOG("steam: lobby %llu data (%zu keys)%s", r->lobby, e.data.size(), join ? " for a join" : "");
			}
			if (r->success) {
				g_lobbyArrived.push_back({e, join});  // handed out after the Steam lock is released
			}
		}
		break;
	}
	case kGameOverlayActivated:
		LOG("steam: overlay %s", *m.param ? "open" : "closed");
		PostNotification(XN_SYS_UI, *m.param ? TRUE : FALSE);  // the game pauses its input, as for the GFWL Guide
		break;
	default:
		break;
	}
}

void OnCallResult(Call kind, SteamAPICall_t call, bool failed) {
	if (kind == Call::LobbyList) {
		LobbyMatchList_t r{};
		bool ioFailed = failed;
		if (!failed) {
			g_utils->GetAPICallResult(call, &r, sizeof(r), kLobbyMatchList, &ioFailed);
		}
		std::vector<LobbyEntry> found;
		for (uint32_t i = 0; !ioFailed && i < r.count; ++i) {
			uint64_t id = g_mm->GetLobbyByIndex((int)i).v;
			if (id && id != g_lobby) {
				found.push_back(ReadLobby(id));
			}
		}
		g_listResult = std::move(found);
		g_listStamp = GetTickCount64();
		g_listPending = false;
		LOG("steam: lobby search: %zu LP2Net lobbies%s", g_listResult.size(), ioFailed ? " (failed)" : "");
	}
	else if (kind == Call::CreateLobby) {
		LobbyCreated_t r{};
		bool ioFailed = failed;
		if (!failed) {
			g_utils->GetAPICallResult(call, &r, sizeof(r), kLobbyCreated, &ioFailed);
		}
		g_creatingLobby = false;
		if (ioFailed || r.result != 1) {
			LOG("steam: lobby not created (result %d)", r.result);
			return;
		}
		if (!g_wantLobby) {
			g_mm->LeaveLobby(Sid(r.lobby));  // the session ended while Steam was creating it
			return;
		}
		g_lobby = r.lobby;
		g_lobbyPublished.clear();
		g_mm->SetLobbyType(Sid(g_lobby), g_lobbyWanted.listed ? kLobbyPublic : kLobbyFriendsOnly);
		g_mm->SetLobbyJoinable(Sid(g_lobby), true);
		PublishLobbyData();
		char connect[64];
		snprintf(connect, sizeof(connect), "+lp2net_lobby %llu", g_lobby);
		g_friends->SetRichPresence("connect", connect);
		LOG("steam: hosting lobby %llu (%s)", g_lobby, g_lobbyWanted.listed ? "listed" : "friends only");
	}
}
}  // namespace

bool SteamStart() {
	std::lock_guard lock(g_mutex);
	if (g_ready) {
		return true;
	}
	if (!Cfg().steam) {
		LOG("steam: disabled in lp2net.ini");
		return false;
	}
	wchar_t appId[16] = L"";
	if (!GetEnvironmentVariableW(L"SteamAppId", appId, 16)) {
		SetEnvironmentVariableW(L"SteamAppId", L"45750");  // what Steam sets for this game
	}
	HMODULE dll = GetModuleHandleW(L"steamclient.dll");
	std::wstring path = SteamClientPath();
	if (!dll && !path.empty()) {
		dll = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
	}
	if (!dll) {
		LOG("steam: steamclient.dll not found (%ls); Steam features off", path.c_str());
		return false;
	}
	auto create = (CreateInterfaceFn)GetProcAddress(dll, "CreateInterface");
	g_getCallback = (BGetCallbackFn)GetProcAddress(dll, "Steam_BGetCallback");
	g_freeCallback = (FreeLastCallbackFn)GetProcAddress(dll, "Steam_FreeLastCallback");
	if (!create || !g_getCallback || !g_freeCallback) {
		LOG("steam: steamclient.dll lacks the expected exports");
		return false;
	}
	g_client = (ISteamClient*)create("SteamClient020", nullptr);
	if (!g_client) {
		LOG("steam: SteamClient020 not available");
		return false;
	}
	g_pipe = g_client->CreateSteamPipe();
	g_hUser = g_pipe ? g_client->ConnectToGlobalUser(g_pipe) : 0;
	if (!g_hUser) {
		LOG("steam: not connected (is Steam running and logged in?)");
		if (g_pipe) {
			g_client->BReleaseSteamPipe(g_pipe);
		}
		return false;
	}
	g_user = (ISteamUser*)g_client->GetISteamUser(g_hUser, g_pipe, "SteamUser023");
	g_friends = (ISteamFriends*)g_client->GetISteamFriends(g_hUser, g_pipe, "SteamFriends017");
	g_mm = (ISteamMatchmaking*)g_client->GetISteamMatchmaking(g_hUser, g_pipe, "SteamMatchMaking009");
	g_net = (ISteamNetworking*)g_client->GetISteamNetworking(g_hUser, g_pipe, "SteamNetworking006");
	g_utils = (ISteamUtils*)g_client->GetISteamUtils(g_pipe, "SteamUtils010");
	if (!g_user || !g_friends || !g_mm || !g_net || !g_utils) {
		LOG("steam: interfaces missing (user %p friends %p mm %p net %p utils %p)", g_user, g_friends, g_mm, g_net,
		    g_utils);
		g_client->ReleaseUser(g_pipe, g_hUser);
		g_client->BReleaseSteamPipe(g_pipe);
		return false;
	}
	g_myId = g_user->GetSteamID().v;
	g_net->AllowP2PPacketRelay(true);
	g_ready = true;
	LOG("steam: connected as '%s' (%llu), app %u, logged on %d, overlay %d", g_friends->GetPersonaName(), g_myId,
	    g_utils->GetAppID(), g_user->BLoggedOn(), g_utils->IsOverlayEnabled());
	return true;
}

void SteamStop() {
	std::lock_guard lock(g_mutex);
	if (!g_ready) {
		return;
	}
	if (g_lobby) {
		g_mm->LeaveLobby(Sid(g_lobby));
		g_lobby = 0;
	}
	g_friends->ClearRichPresence();
	g_ready = false;
	g_client->ReleaseUser(g_pipe, g_hUser);
	g_client->BReleaseSteamPipe(g_pipe);
}

// Handlers run after the Steam lock is released: they take the net lock, and net code calls into Steam.
void SteamTick() {
	std::vector<std::pair<LobbyEntry, bool>> lobbies;
	std::vector<std::pair<uint64_t, std::vector<BYTE>>> packets;
	{
		std::lock_guard lock(g_mutex);
		if (!g_ready) {
			return;
		}
		CallbackMsg_t m{};
		for (int i = 0; i < 64 && g_getCallback(g_pipe, &m); ++i) {
			OnCallback(m);
			g_freeCallback(g_pipe);
		}
		for (auto it = g_calls.begin(); it != g_calls.end();) {
			bool failed = false;
			if (g_utils->IsAPICallCompleted(it->first, &failed)) {
				auto [call, kind] = *it;
				it = g_calls.erase(it);
				OnCallResult(kind, call, failed);
			}
			else {
				++it;
			}
		}
		uint32_t size = 0;
		static BYTE buf[2048];
		for (int i = 0; i < 256 && g_net->IsP2PPacketAvailable(&size, 0); ++i) {
			CSteamID from{};
			uint32_t got = 0;
			if (!g_net->ReadP2PPacket(buf, sizeof(buf), &got, &from, 0)) {
				break;
			}
			if (got <= sizeof(buf)) {
				packets.emplace_back(from.v, std::vector<BYTE>(buf, buf + got));
			}
		}
		lobbies.swap(g_lobbyArrived);
	}
	for (const auto& [from, data] : packets) {
		if (g_relayHandler) {
			g_relayHandler(from, data.data(), (uint32_t)data.size());
		}
	}
	for (const auto& [lobby, join] : lobbies) {
		if (g_lobbyHandler) {
			g_lobbyHandler(lobby, join);
		}
	}
}

bool SteamReady() {
	return g_ready;
}

uint64_t SteamMyId() {
	return g_myId;
}

std::string SteamMyName() {
	std::lock_guard lock(g_mutex);
	const char* n = g_ready ? g_friends->GetPersonaName() : nullptr;
	return n ? n : "";
}

std::string SteamUserName(uint64_t id) {
	std::lock_guard lock(g_mutex);
	const char* n = g_ready && id ? g_friends->GetFriendPersonaName(Sid(id)) : nullptr;
	return n && strcmp(n, "[unknown]") != 0 ? n : "";
}

// Steam accounts: SteamID64 = 0x0110000100000000 | account id. Online XUIDs carry 0x0009 in the top 16 bits.
bool SteamIsSteamXuid(uint64_t xuid) {
	return (xuid >> 48) == 0x0009 && ((xuid >> 32) & 0xFFFF) == 0x5354;  // "ST"
}

uint64_t SteamIdFromXuid(uint64_t xuid) {
	return SteamIsSteamXuid(xuid) ? 0x0110000100000000ull | (xuid & 0xFFFFFFFFull) : 0;
}

uint64_t SteamXuid() {
	return g_myId ? 0x0009535400000000ull | (g_myId & 0xFFFFFFFFull) : 0;
}

void SteamSetRichPresence(const char* key, const char* value) {
	std::lock_guard lock(g_mutex);
	if (g_ready) {
		g_friends->SetRichPresence(key, value ? value : "");
	}
}

bool SteamOverlay(const char* dialog) {
	std::lock_guard lock(g_mutex);
	if (!g_ready || !g_utils->IsOverlayEnabled()) {
		return false;
	}
	g_friends->ActivateGameOverlay(dialog);
	return true;
}

bool SteamOverlayToUser(const char* dialog, uint64_t id) {
	std::lock_guard lock(g_mutex);
	if (!g_ready || !id || !g_utils->IsOverlayEnabled()) {
		return false;
	}
	g_friends->ActivateGameOverlayToUser(dialog, Sid(id));
	return true;
}

bool SteamInviteDialog() {
	std::lock_guard lock(g_mutex);
	if (!g_ready || !g_lobby || !g_utils->IsOverlayEnabled()) {
		return false;
	}
	g_friends->ActivateGameOverlayInviteDialog(Sid(g_lobby));
	return true;
}

void SteamHostLobby(const LobbyInfo* info) {
	std::lock_guard lock(g_mutex);
	if (!g_ready) {
		return;
	}
	if (!info) {
		g_wantLobby = false;
		if (g_lobby) {
			LOG("steam: leaving lobby %llu", g_lobby);
			g_mm->LeaveLobby(Sid(g_lobby));
			g_lobby = 0;
			g_friends->SetRichPresence("connect", "");
		}
		return;
	}
	bool typeChanged = g_lobby && info->listed != g_lobbyWanted.listed;
	g_wantLobby = true;
	g_lobbyWanted = *info;
	g_lobbyWanted.data["lp2net"] = "1";
	if (g_lobby) {
		if (typeChanged) {
			g_mm->SetLobbyType(Sid(g_lobby), info->listed ? kLobbyPublic : kLobbyFriendsOnly);
		}
		PublishLobbyData();
	}
	else if (!g_creatingLobby) {
		g_creatingLobby = true;
		SteamAPICall_t c = g_mm->CreateLobby(info->listed ? kLobbyPublic : kLobbyFriendsOnly, info->maxMembers);
		if (c) {
			g_calls[c] = Call::CreateLobby;
		}
		else {
			g_creatingLobby = false;
			LOG("steam: CreateLobby refused");
		}
	}
}

uint64_t SteamHostedLobby() {
	return g_lobby;
}

void SteamRequestLobbies() {
	std::lock_guard lock(g_mutex);
	if (!g_ready || g_listPending) {
		return;
	}
	g_mm->AddRequestLobbyListStringFilter("lp2net", "1", kLobbyCmpEqual);
	g_mm->AddRequestLobbyListDistanceFilter(kLobbyDistanceWorldwide);
	g_mm->AddRequestLobbyListResultCountFilter(50);
	SteamAPICall_t c = g_mm->RequestLobbyList();
	if (c) {
		g_calls[c] = Call::LobbyList;
		g_listPending = true;
	}
}

bool SteamLobbyResults(ULONGLONG* stamp, std::vector<LobbyEntry>* out) {
	std::lock_guard lock(g_mutex);
	if (!g_listStamp || g_listStamp == *stamp) {
		return false;
	}
	*stamp = g_listStamp;
	*out = g_listResult;
	return true;
}

bool SteamPendingLobbyRequest() {
	return g_listPending;
}

std::vector<FriendInGame> SteamFriendsInGame() {
	std::lock_guard lock(g_mutex);
	std::vector<FriendInGame> out;
	if (!g_ready) {
		return out;
	}
	int n = g_friends->GetFriendCount(kFriendFlagImmediate);
	for (int i = 0; i < n; ++i) {
		CSteamID f = g_friends->GetFriendByIndex(i, kFriendFlagImmediate);
		FriendGameInfo_t g{};
		if (g_friends->GetFriendGamePlayed(f, &g) && (g.gameID & 0xFFFFFF) == kAppId) {
			const char* name = g_friends->GetFriendPersonaName(f);
			out.push_back({f.v, name ? name : "?", g.lobby.v});
		}
	}
	return out;
}

bool SteamRelaySend(uint64_t to, const void* data, uint32_t cb) {
	if (!g_ready || !to) {
		return false;
	}
	return g_net->SendP2PPacket(Sid(to), data, cb, cb <= kP2PUnreliableMax ? kP2PUnreliable : kP2PReliable, 0);
}

void SteamSetRelayHandler(RelayHandler fn) {
	g_relayHandler = fn;
}

void SteamSetLobbyHandler(LobbyHandler fn) {
	g_lobbyHandler = fn;
}

static void AskLobby(uint64_t lobby, bool join) {
	std::lock_guard lock(g_mutex);
	if (!g_ready || !lobby || lobby == g_lobby) {
		return;
	}
	bool& wanted = g_lobbyWaiting[lobby];
	wanted = wanted || join;
	if (!g_mm->RequestLobbyData(Sid(lobby))) {
		LOG("steam: lobby %llu: data request refused", lobby);
		g_lobbyWaiting.erase(lobby);
	}
}

void SteamJoinLobbyById(uint64_t lobby) {
	AskLobby(lobby, true);
}

void SteamQueryLobby(uint64_t lobby) {
	AskLobby(lobby, false);
}

void SteamVoiceRecord(bool on) {
	std::lock_guard lock(g_mutex);
	if (g_ready) {
		on ? g_user->StartVoiceRecording() : g_user->StopVoiceRecording();
	}
}

int SteamVoiceGet(void* buf, uint32_t cb, int* result) {
	std::lock_guard lock(g_mutex);
	uint32_t avail = 0, written = 0;
	*result = g_ready ? g_user->GetAvailableVoice(&avail, nullptr, 0) : 1;  // 1 = k_EVoiceResultNotInitialized
	if (*result != 0 || !avail) {
		return 0;
	}
	*result = g_user->GetVoice(true, buf, cb, &written, false, nullptr, 0, nullptr, 0);
	return *result == 0 ? (int)written : 0;
}

int SteamVoiceDecompress(const void* data, uint32_t cb, int16_t* pcm, uint32_t cbPcm, uint32_t sampleRate) {
	std::lock_guard lock(g_mutex);
	uint32_t written = 0;
	if (!g_ready || g_user->DecompressVoice(data, cb, pcm, cbPcm, &written, sampleRate) != 0) {
		return -1;
	}
	return (int)written;
}

uint32_t SteamVoiceSampleRate() {
	std::lock_guard lock(g_mutex);
	return g_ready ? g_user->GetVoiceOptimalSampleRate() : 0;
}

void SteamLaunchJoin() {
	static bool done = false;
	if (done || !g_ready) {
		return;
	}
	done = true;
	const char* cmd = GetCommandLineA();
	const char* at = cmd ? strstr(cmd, "+lp2net_lobby ") : nullptr;
	unsigned long long lobby = 0;
	if (at && sscanf_s(at, "+lp2net_lobby %llu", &lobby) == 1 && lobby) {
		LOG("steam: started by Join Game for lobby %llu", lobby);
		SteamJoinLobbyById(lobby);
	}
}

}  // namespace lp2net
