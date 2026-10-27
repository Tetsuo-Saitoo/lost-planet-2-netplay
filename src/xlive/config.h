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

	// [Steam]
	bool steam = true;         // use the Steam client when the game runs under Steam
	bool steamName = true;     // gamertag = Steam name (falls back to [Player] Name)
	bool steamRelay = true;    // reach players through Steam when a direct connection fails

	// [Overlay]
	UINT overlayKey = VK_HOME;

	// [Voice]
	bool voice = true;
	UINT pushToTalk = 0;       // virtual-key code held to talk; 0 = open microphone
	int voiceVolume = 100;     // playback, percent (0..200)
	bool voiceSteamMic = true;  // Steam records and compresses the microphone when Steam runs
	std::vector<ULONGLONG> muted;  // XUIDs on the mute list (XUserMuteListQuery)

	// [Debug]
	bool log = true;
	bool logPackets = false;
	bool gameLog = true;  // copy the game's own network debug messages into lp2net.log
	bool allowMultipleCopies = false;  // run several copies on one PC (testing)
	std::string voiceFeedWav;  // play this WAV file into voice chat instead of the microphone (loops)
	int voiceFeedCopy = 0;     // only copy N on this PC uses the feed (0 = every copy)
	std::string voiceDumpWav;  // write what voice chat plays to this WAV file (copy N > 1 adds "-copyN")
	bool voiceProbe = false;   // log players, soldiers and cameras found in memory (proximity voice research)
};

Config& Cfg();
std::string SanitizeGamertag(std::string name);  // "" when nothing usable is left
const std::wstring& GameDir();
void LoadConfig();
void SaveConfig();

}  // namespace lp2net
