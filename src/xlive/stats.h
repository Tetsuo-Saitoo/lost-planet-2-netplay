// Stats, leaderboards and achievements, kept on the player's PC now that LIVE is gone (xstats.cpp,
// achievements.cpp). The game writes its stats through XSessionWriteStats; we keep the rows in the profile folder,
// send our own to every LP2 Netplay peer, and answer the game's leaderboard reads from what we have: the player and
// the people they played with.
#pragma once

#include "gfwl.h"

#include <cstdint>
#include <string>
#include <vector>

namespace lp2net {

// ---- stats (xstats.cpp) ----------------------------------------------------------------------------------------
// XSessionWriteStats: the game's XSESSION_VIEW_PROPERTIES array for one player.
void StatsWrite(XUID xuid, DWORD numViews, const void* views);
// Session members (XSessionJoinRemote): players we played with. Their shared rows are kept and shown.
void StatsMetPlayers(const XUID* xuids, DWORD count);
// Our own rows as PacketType::Stats bodies, for a peer we just met (net.cpp).
std::vector<std::vector<BYTE>> StatsPackets();
// A PacketType::Stats body from a peer.
void StatsReceive(uint32_t peerId, const BYTE* body, int cb);
// Send Stats bodies to every live peer (net.cpp).
void SendStatsToPeers(const std::vector<std::vector<BYTE>>& packets);

// ---- achievements (achievements.cpp) ---------------------------------------------------------------------------
struct AchievementView {
	uint32_t id = 0;
	std::string name, description;  // UTF-8, from the game's SPA
	uint16_t gamerscore = 0;
	bool secret = false;  // locked, and the title hides it until then (most of LP2's are)
	bool unlocked = false;
	FILETIME when{};
};
std::vector<AchievementView> ListAchievements();  // empty when the game's SPA could not be read

}  // namespace lp2net
