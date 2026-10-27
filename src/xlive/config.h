// lp2net.ini next to the game exe. Edited by hand, by the launcher, or from the in-game overlay.
#pragma once

#include <windows.h>

#include <string>
#include <vector>

namespace lp2net {

struct Config {
	// [Player]
	std::string name = "Player";
	ULONGLONG xuid = 0;  // generated and saved on first run
	std::string profileFolder;  // gamertag used last time; its save folder is carried over on a rename

	// [Network]
	std::string lanAddress;     // empty = best interface
	std::string publicAddress;  // empty = learn from peers
	bool upnp = true;
	std::vector<std::string> peers;  // "host[:port]" entries for join-by-IP
	std::string lobbyServer;          // optional "host[:port]" rendezvous/listing server
	bool onlineMode = true;  // signed in "online": the game's own Online menus (Ranked/Player Match) work
	bool freezeGuard = true;  // the game's frozen-game check never takes a player out of a match (watchdog.cpp)

	// [Steam]
	bool steam = true;         // use the Steam client when the game runs under Steam
	bool steamName = true;     // gamertag = Steam name (falls back to [Player] Name)
	bool steamRelay = true;    // reach players through Steam when a direct connection fails

	// [Overlay]
	UINT overlayKey = VK_HOME;

	// [Debug]
	bool log = true;
	bool logPackets = false;
	bool gameLog = true;  // copy the game's own network debug messages into lp2net.log
	bool allowMultipleCopies = false;  // run several copies on one PC (testing)
};

Config& Cfg();
std::string SanitizeGamertag(std::string name);  // "" when nothing usable is left
const std::wstring& GameDir();
void LoadConfig();
void SaveConfig();

}  // namespace lp2net
