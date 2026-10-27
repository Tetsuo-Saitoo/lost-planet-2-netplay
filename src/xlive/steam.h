// Steam integration, through the Steam client the player already runs (steamclient.dll, loaded the way
// steam_api.dll does it, so no Valve binary ships with the mod). Everything here is optional: without Steam the
// rest of LP2Net works as before.
#pragma once

#include <windows.h>

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace lp2net {

bool SteamStart();  // connect as the logged-in Steam user; false when Steam is not available
void SteamStop();
void SteamTick();  // callbacks, call results, relay packets; called from the net worker thread
bool SteamReady();

uint64_t SteamMyId();
std::string SteamMyName();               // persona name, UTF-8
std::string SteamUserName(uint64_t id);  // "" if unknown
bool SteamIsSteamXuid(uint64_t xuid);    // online XUIDs we make from Steam accounts
uint64_t SteamIdFromXuid(uint64_t xuid);
uint64_t SteamXuid();  // our online XUID (0x0009 << 48 | account id)

// rich presence (shown in the Steam friends list); value nullptr or "" removes the key
void SteamSetRichPresence(const char* key, const char* value);

// Steam overlay; false when the overlay is not available (game not started through Steam)
bool SteamOverlay(const char* dialog);  // "Friends", "Achievements", "Players", ...
bool SteamOverlayToUser(const char* dialog, uint64_t id);  // "steamid", "chat", ...
bool SteamInviteDialog();  // invite friends to the session we host

// The lobby that advertises the session we host: public for LIVE-style matchmaking sessions, friends-only otherwise.
struct LobbyInfo {
	std::map<std::string, std::string> data;
	int maxMembers = 16;
	bool listed = false;  // visible to lobby searches
};
void SteamHostLobby(const LobbyInfo* info);  // nullptr = leave
uint64_t SteamHostedLobby();

struct LobbyEntry {
	uint64_t id = 0;
	uint64_t owner = 0;
	std::map<std::string, std::string> data;
};
// Ask Steam for listed LP2Net lobbies; SteamLobbyResults returns the newest finished list (stamp increases).
void SteamRequestLobbies();
bool SteamLobbyResults(ULONGLONG* stamp, std::vector<LobbyEntry>* out);
bool SteamPendingLobbyRequest();

// Friends running Lost Planet 2 right now, with the lobby they are in (0 if none).
struct FriendInGame {
	uint64_t id = 0;
	std::string name;
	uint64_t lobby = 0;
};
std::vector<FriendInGame> SteamFriendsInGame();

// Relay: Steam P2P (with Valve's relay servers when a direct path fails). Frames are opaque to this module.
bool SteamRelaySend(uint64_t to, const void* data, uint32_t cb);
using RelayHandler = void (*)(uint64_t from, const BYTE* data, uint32_t cb);
void SteamSetRelayHandler(RelayHandler fn);

// Lobby data arriving for a lobby we asked about: join = the player accepted an invite or picked "Join Game"
// (SteamJoinLobbyById), otherwise it is a friend's lobby we looked up to find their game (SteamQueryLobby).
using LobbyHandler = void (*)(const LobbyEntry& lobby, bool join);
void SteamSetLobbyHandler(LobbyHandler fn);
void SteamJoinLobbyById(uint64_t lobby);  // e.g. from the "+lp2net_lobby <id>" launch option Steam passes
void SteamQueryLobby(uint64_t lobby);
// "Join Game" while the game was not running: Steam starts it with "+lp2net_lobby <id>". Called once the game
// reaches its menus (it ignores invites before that).
void SteamLaunchJoin();

}  // namespace lp2net
