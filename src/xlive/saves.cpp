// Keep the player's save when the gamertag changes.
//
// LP2 stores its save in Documents\CAPCOM\LOST PLANET 2\<signed-in user name>\ (the name comes from
// XUserGetSigninInfo). Capcom's stub always reported "Player1", so existing saves live in "Player1". Before the game
// first reads its save we copy the previous profile folder to the current gamertag's folder if that one doesn't
// exist yet. Copy, never move: the old folder stays as a backup.
#include "config.h"
#include "log.h"
#include "net.h"
#include "util.h"

#include <shlobj.h>

#include <string>

namespace lp2net {
namespace {
bool DirExists(const std::wstring& p) {
	DWORD a = GetFileAttributesW(p.c_str());
	return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

bool CopyDir(const std::wstring& from, const std::wstring& to) {
	if (!CreateDirectoryW(to.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
		return false;
	}
	WIN32_FIND_DATAW fd;
	HANDLE h = FindFirstFileW((from + L"\\*").c_str(), &fd);
	if (h == INVALID_HANDLE_VALUE) {
		return true;
	}
	bool ok = true;
	do {
		std::wstring name = fd.cFileName;
		if (name == L"." || name == L"..") {
			continue;
		}
		if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
			ok &= CopyDir(from + L"\\" + name, to + L"\\" + name);
		}
		else {
			ok &= CopyFileW((from + L"\\" + name).c_str(), (to + L"\\" + name).c_str(), TRUE) != FALSE;
		}
	} while (FindNextFileW(h, &fd));
	FindClose(h);
	return ok;
}
}  // namespace

std::wstring SavesRoot() {
	wchar_t docs[MAX_PATH];
	if (FAILED(SHGetFolderPathW(nullptr, CSIDL_PERSONAL, nullptr, 0, docs))) {
		return L"";
	}
	return std::wstring(docs) + L"\\CAPCOM\\LOST PLANET 2\\";
}

std::wstring ProfileDir() {
	wchar_t test[MAX_PATH];
	if (GetEnvironmentVariableW(L"LP2NET_PROFILE_DIR", test, MAX_PATH)) {
		return test;  // tests (statstest): keep their files out of the player's Documents
	}
	std::wstring base = SavesRoot();
	return base.empty() ? GameDir() + L"LP2Netplay-profile" : base + Widen(Me().name);
}

void CarrySaveToGamertag() {
	std::wstring base = SavesRoot();
	if (base.empty()) {
		return;
	}
	std::wstring target = base + Widen(Me().name);
	if (DirExists(target)) {
		LOG("saves: using %ls", target.c_str());
	}
	else {
		for (const std::string& src : {Cfg().profileFolder, std::string("Player1")}) {
			if (src.empty() || src == Me().name || !DirExists(base + Widen(src))) {
				continue;
			}
			bool ok = CopyDir(base + Widen(src), target);
			LOG("saves: copied profile '%s' -> '%s' %s", src.c_str(), Me().name.c_str(), ok ? "ok" : "FAILED");
			break;
		}
	}
	if (Me().slot == 0 && Cfg().profileFolder != Me().name) {
		Cfg().profileFolder = Me().name;
		SaveConfig();
	}
}

}  // namespace lp2net
