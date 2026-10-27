#include "config.h"

#include "util.h"

#include <algorithm>
#include <cctype>
#include <cstdio>

namespace lp2net {
namespace {
Config g_cfg;
std::wstring g_dir;
std::wstring g_ini;

std::string ReadStr(const wchar_t* sec, const wchar_t* key, const std::string& def) {
	wchar_t buf[1024];
	GetPrivateProfileStringW(sec, key, Widen(def).c_str(), buf, ARRAYSIZE(buf), g_ini.c_str());
	return Narrow(buf);
}

bool ReadBool(const wchar_t* sec, const wchar_t* key, bool def) {
	return GetPrivateProfileIntW(sec, key, def ? 1 : 0, g_ini.c_str()) != 0;
}

void WriteStr(const wchar_t* sec, const wchar_t* key, const std::string& v) {
	WritePrivateProfileStringW(sec, key, Widen(v).c_str(), g_ini.c_str());
}

std::vector<std::string> SplitList(const std::string& s) {
	std::vector<std::string> out;
	size_t start = 0;
	while (start <= s.size()) {
		size_t end = s.find_first_of(",; ", start);
		if (end == std::string::npos) {
			end = s.size();
		}
		if (end > start) {
			out.push_back(s.substr(start, end - start));
		}
		start = end + 1;
	}
	return out;
}
}  // namespace

Config& Cfg() {
	return g_cfg;
}

// The game names the save folder after the gamertag, so keep it to characters a folder name can hold.
std::string SanitizeGamertag(std::string name) {
	std::erase_if(name, [](char ch) { return !(isalnum((unsigned char)ch) || ch == ' ' || ch == '_' || ch == '-'); });
	while (!name.empty() && name.back() == ' ') {
		name.pop_back();
	}
	while (!name.empty() && name.front() == ' ') {
		name.erase(name.begin());
	}
	if (name.size() > 15) {
		name.resize(15);
	}
	return name;
}

const std::wstring& GameDir() {
	if (g_dir.empty()) {
		wchar_t path[MAX_PATH];
		GetModuleFileNameW(nullptr, path, MAX_PATH);
		g_dir = path;
		g_dir.resize(g_dir.find_last_of(L"\\/") + 1);
	}
	return g_dir;
}

void LoadConfig() {
	g_ini = GameDir() + L"lp2net.ini";
	Config& c = g_cfg;
	c.name = ReadStr(L"Player", L"Name", "");
	if (c.name.empty()) {
		char user[64] = "Player";
		DWORD n = sizeof(user);
		GetUserNameA(user, &n);
		c.name = user;
	}
	c.name = SanitizeGamertag(c.name);
	if (c.name.empty()) {
		c.name = "Player";
	}
	c.profileFolder = ReadStr(L"Player", L"ProfileFolder", "");
	std::string xuid = ReadStr(L"Player", L"XUID", "");
	c.xuid = xuid.empty() ? 0 : _strtoui64(xuid.c_str(), nullptr, 16);
	if (c.xuid == 0) {
		// offline-style XUID (0xE000...) so it can never collide with a real Live account
		c.xuid = 0xE000000000000000ull | (RandomU64() & 0x0000FFFFFFFFFFFFull);
	}

	c.lanAddress = ReadStr(L"Network", L"LanAddress", "");
	c.publicAddress = ReadStr(L"Network", L"PublicAddress", "");
	c.upnp = ReadBool(L"Network", L"UPnP", true);
	c.peers = SplitList(ReadStr(L"Network", L"Peers", ""));
	c.lobbyServer = ReadStr(L"Network", L"LobbyServer", "");
	c.onlineMode = ReadBool(L"Network", L"OnlineMode", true);
	c.freezeGuard = ReadBool(L"Network", L"FreezeGuard", true);

	c.steam = ReadBool(L"Steam", L"Enabled", true);
	c.steamName = ReadBool(L"Steam", L"UseSteamName", true);
	c.steamRelay = ReadBool(L"Steam", L"Relay", true);

	c.overlayKey = GetPrivateProfileIntW(L"Overlay", L"Hotkey", VK_HOME, g_ini.c_str());

	c.voice = ReadBool(L"Voice", L"Enabled", true);
	c.pushToTalk = GetPrivateProfileIntW(L"Voice", L"PushToTalk", 0, g_ini.c_str());
	c.voiceVolume = std::clamp((int)GetPrivateProfileIntW(L"Voice", L"Volume", 100, g_ini.c_str()), 0, 200);
	c.voiceSteamMic = ReadBool(L"Voice", L"SteamMicrophone", true);
	c.muted.clear();
	for (const auto& x : SplitList(ReadStr(L"Voice", L"Muted", ""))) {
		if (ULONGLONG v = _strtoui64(x.c_str(), nullptr, 16)) {
			c.muted.push_back(v);
		}
	}

	c.log = ReadBool(L"Debug", L"Log", true);
	c.logPackets = ReadBool(L"Debug", L"LogPackets", false);
	c.gameLog = ReadBool(L"Debug", L"GameLog", true);
	c.allowMultipleCopies = ReadBool(L"Debug", L"AllowMultipleCopies", false);
	c.voiceFeedWav = ReadStr(L"Debug", L"VoiceFeedWav", "");
	c.voiceFeedCopy = GetPrivateProfileIntW(L"Debug", L"VoiceFeedCopy", 0, g_ini.c_str());
	c.voiceDumpWav = ReadStr(L"Debug", L"VoiceDumpWav", "");
	c.voiceProbe = ReadBool(L"Debug", L"VoiceProbe", false);
	SaveConfig();  // writes defaults and the generated XUID back so the file documents itself
}

void SaveConfig() {
	const Config& c = g_cfg;
	char xuid[32];
	snprintf(xuid, sizeof(xuid), "%016llX", c.xuid);
	std::string peers;
	for (const auto& p : c.peers) {
		peers += (peers.empty() ? "" : ",") + p;
	}
	WriteStr(L"Player", L"Name", c.name);
	WriteStr(L"Player", L"XUID", xuid);
	WriteStr(L"Player", L"ProfileFolder", c.profileFolder);
	WriteStr(L"Network", L"LanAddress", c.lanAddress);
	WriteStr(L"Network", L"PublicAddress", c.publicAddress);
	WriteStr(L"Network", L"UPnP", c.upnp ? "1" : "0");
	WriteStr(L"Network", L"Peers", peers);
	WriteStr(L"Network", L"LobbyServer", c.lobbyServer);
	WriteStr(L"Network", L"OnlineMode", c.onlineMode ? "1" : "0");
	WriteStr(L"Network", L"FreezeGuard", c.freezeGuard ? "1" : "0");
	WriteStr(L"Steam", L"Enabled", c.steam ? "1" : "0");
	WriteStr(L"Steam", L"UseSteamName", c.steamName ? "1" : "0");
	WriteStr(L"Steam", L"Relay", c.steamRelay ? "1" : "0");
	WriteStr(L"Overlay", L"Hotkey", std::to_string(c.overlayKey));
	std::string muted;
	for (ULONGLONG x : c.muted) {
		char buf[24];
		snprintf(buf, sizeof(buf), "%s%016llX", muted.empty() ? "" : ",", x);
		muted += buf;
	}
	WriteStr(L"Voice", L"Enabled", c.voice ? "1" : "0");
	WriteStr(L"Voice", L"PushToTalk", std::to_string(c.pushToTalk));
	WriteStr(L"Voice", L"Volume", std::to_string(c.voiceVolume));
	WriteStr(L"Voice", L"SteamMicrophone", c.voiceSteamMic ? "1" : "0");
	WriteStr(L"Voice", L"Muted", muted);
	WriteStr(L"Debug", L"Log", c.log ? "1" : "0");
	WriteStr(L"Debug", L"LogPackets", c.logPackets ? "1" : "0");
	WriteStr(L"Debug", L"GameLog", c.gameLog ? "1" : "0");
	WriteStr(L"Debug", L"AllowMultipleCopies", c.allowMultipleCopies ? "1" : "0");
	WriteStr(L"Debug", L"VoiceFeedWav", c.voiceFeedWav);
	WriteStr(L"Debug", L"VoiceFeedCopy", std::to_string(c.voiceFeedCopy));
	WriteStr(L"Debug", L"VoiceDumpWav", c.voiceDumpWav);
	WriteStr(L"Debug", L"VoiceProbe", c.voiceProbe ? "1" : "0");
}

}  // namespace lp2net
