#pragma once

#include <windows.h>

#include <string>

namespace lp2net {
// Where the player is in the game, from LP2's rich-presence contexts and properties (defined in the game's SPA
// resource, title 43430808). Drives the System Link prompt.
struct PresenceView {
	DWORD mode = 0;      // RICH_MODE (context 0): 5 Campaign, 12 System Link, 6 Online, ...
	DWORD scene = 0;     // RICH_SCENE (context 1): 1 title, 2 menu, ...
	DWORD presence = 0;  // X_CONTEXT_PRESENCE: 1 menu, 2 battle, 3 campaign, 4 training, 5 lobby
	DWORD episode = 0, chapter = 0;
};

void PresenceOnContext(DWORD id, DWORD value);
void PresenceOnProperty(DWORD id, const void* value, DWORD cb);
PresenceView CurrentPresence();
std::string PresenceText();  // "Campaign: in the lobby", for the log and the overlay
}  // namespace lp2net
