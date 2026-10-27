// LP2 reports what the player is doing through XUserSetContext/XUserSetProperty (GFWL rich presence). The values
// come from the game's SPA resource (XLAST source, title 43430808):
//   context 0          RICH_MODE  0 single, 1 co-op, 2 ranked, 3 player match, 4 influence, 5 campaign, 6 online,
//                                 7 training, 8 my page, 9/10 basic/advanced training, 11 bot battle, 12 System Link
//   context 1          RICH_SCENE 0 prepare, 1 title, 2 menu, 3 options, ... 12 PC settings
//   context 0x8001     presence   0 scene, 1 menu, 2 battle, 3 campaign, 4 training, 5 preparing (lobby)
//   property 0x1000000A / 0x10000009  RICH_EPISODE / RICH_CHAPTER
#include "presence.h"

#include "log.h"
#include "net.h"
#include "overlay.h"
#include "steam.h"

#include <mutex>
#include <set>

namespace lp2net {
namespace {
constexpr DWORD kCtxMode = 0, kCtxScene = 1, kCtxPresence = 0x8001;
constexpr DWORD kPropEpisode = 0x1000000A, kPropChapter = 0x10000009;
constexpr DWORD kModeCampaign = 5, kModeSystemLink = 12;
constexpr DWORD kPresenceMenu = 1;
constexpr DWORD kSceneMenu = 2;

std::mutex g_mutex;
PresenceView g_view;
std::set<DWORD> g_prompted;  // modes the System Link prompt was shown for in this run

const char* ModeName(DWORD m) {
	static const char* names[] = {"Single player", "Co-op", "Ranked Match", "Player Match", "Influence", "Campaign",
	                              "Online", "Training", "My Page", "Basic Training", "Advanced Training",
	                              "Bot Battle", "System Link"};
	return m < _countof(names) ? names[m] : "?";
}

const char* PresenceName(DWORD p) {
	static const char* names[] = {"in a scene", "in the menu", "in a battle", "playing", "training", "in the lobby"};
	return p < _countof(names) ? names[p] : "?";
}

// Shown once per mode per run, when the player reaches the Campaign or System Link menu: the game's own text still
// talks about Xbox LIVE, so say what System Link does now and where the extra options are.
void SystemLinkPrompt(DWORD mode) {
	auto peers = ListPeers();
	std::string who;
	int alive = 0;
	for (const auto& p : peers) {
		if (p.alive && !p.name.empty()) {
			who += (alive++ ? ", " : "") + p.name;
		}
	}
	std::string body = mode == kModeCampaign
	                       ? "Pick Connection Type: System Link to play co-op on your network or over the internet."
	                       : "System Link games now work on your network and over the internet.";
	body += alive ? "\nPlayers online: " + who : "\nNo other players found yet.";
	body += "\nHome key (or hold Back on a controller): game list, Join by IP, settings.";
	OverlayPrompt("LP2 Netplay", body);
}
}  // namespace

void PresenceOnContext(DWORD id, DWORD value) {
	DWORD promptMode = 0;
	{
		std::lock_guard lock(g_mutex);
		if (id == kCtxMode) {
			g_view.mode = value;
		}
		else if (id == kCtxScene) {
			g_view.scene = value;
		}
		else if (id == kCtxPresence) {
			g_view.presence = value;
		}
		else {
			return;
		}
		bool linkMenu = g_view.mode == kModeCampaign || g_view.mode == kModeSystemLink;
		if (linkMenu && g_view.presence == kPresenceMenu && g_prompted.insert(g_view.mode).second) {
			promptMode = g_view.mode;
		}
	}
	if (id != kCtxScene) {
		std::string text = PresenceText();
		LOG("presence: %s", text.c_str());
		SteamSetRichPresence("status", text.c_str());  // shown in the Steam friends list
	}
	if (promptMode) {
		SystemLinkPrompt(promptMode);
	}
	if (id == kCtxScene && value == kSceneMenu) {
		SteamLaunchJoin();
	}
}

void PresenceOnProperty(DWORD id, const void* value, DWORD cb) {
	if ((id != kPropEpisode && id != kPropChapter) || !value || cb < sizeof(DWORD)) {
		return;
	}
	DWORD v = *(const DWORD*)value;
	{
		std::lock_guard lock(g_mutex);
		DWORD& slot = id == kPropEpisode ? g_view.episode : g_view.chapter;
		if (slot == v) {
			return;
		}
		slot = v;
	}
	SteamSetRichPresence("status", PresenceText().c_str());
}

PresenceView CurrentPresence() {
	std::lock_guard lock(g_mutex);
	return g_view;
}

std::string PresenceText() {
	PresenceView v = CurrentPresence();
	char buf[96];
	if (v.presence == 3 && v.episode) {
		snprintf(buf, sizeof(buf), "%s: playing episode %lu, chapter %lu", ModeName(v.mode), v.episode, v.chapter);
	}
	else {
		snprintf(buf, sizeof(buf), "%s: %s", ModeName(v.mode), PresenceName(v.presence));
	}
	return buf;
}
}  // namespace lp2net
