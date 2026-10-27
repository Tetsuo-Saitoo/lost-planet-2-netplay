// LP2NetSetup logic: finding the game, telling Capcom's stub from ours, install / repair / uninstall, the opt-in
// backup of Games for Windows LIVE leftovers and the two player settings in lp2net.ini. No UI here: main.cpp shows
// the results in the window or on the command line.
#pragma once

#include <windows.h>

#include <optional>
#include <string>
#include <vector>

namespace lp2setup {

enum class Dll { Missing, Capcom, Ours, Other };

struct GameFile {
	std::wstring name;
	ULONGLONG size = 0;
};

struct GameState {
	std::wstring dir;
	bool isGame = false;   // LP2DX9.exe is there
	bool running = false;  // the game (or its launcher) runs from this folder
	Dll xlive = Dll::Missing;
	std::wstring xliveVersion;  // when xlive is ours; empty for test builds before 0.2.0
	bool xliveUnreadable = false;  // xlive.dll is there but could not be read (counted as Other)
	Dll capcom = Dll::Missing;  // xlive_capcom.dll: Missing, Capcom or Other
	std::vector<GameFile> leftovers;  // GFWL files still in the game folder
	std::vector<GameFile> backedUp;   // GFWL files in our backup folder
};

struct Settings {
	std::optional<std::wstring> name;   // fallback gamertag; nullopt leaves lp2net.ini as it is
	std::optional<std::wstring> peers;  // friend's address(es) for join by IP
};

struct Result {
	bool ok = false;
	std::wstring summary;  // one sentence for the player
	std::vector<std::wstring> details;
};

bool FileExists(const std::wstring& path);
std::wstring NormalizeDir(const std::wstring& dir);
bool IsGameDir(const std::wstring& dir);
std::wstring FindGameDir();  // registry, then every Steam library; empty when not found

GameState Inspect(const std::wstring& dir);
Settings ReadSettings(const std::wstring& dir);
std::wstring SanitizeGamertag(const std::wstring& name);
bool ValidPeers(const std::wstring& peers);

Result Install(const std::wstring& dir, const Settings& settings, bool cleanup);  // also repair and update
Result Uninstall(const std::wstring& dir);

// Player-facing text
std::wstring Describe(const GameState& st);
std::wstring InstallLabel(const GameState& st);  // "Install", "Update" or "Repair"
bool CanInstall(const GameState& st);
bool CanUninstall(const GameState& st);
std::wstring DllText(Dll kind, const std::wstring& version);
std::wstring FileList(const std::vector<GameFile>& files);

}  // namespace lp2setup
