#include "setup.h"

#include "lp2net_version.h"
#include "resource.h"

#include <bcrypt.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cwchar>
#include <cwctype>

namespace lp2setup {
namespace {
// Capcom's 2025 "xlive-dummy" (Steam build 17599994). An xlive.dll that is neither this nor ours is never touched.
const wchar_t kCapcomSha[] = L"867bfe461a0562aee0a43cd832a52b228ac6da4e4216bdfbb94ef14bece85366";
const wchar_t kVersion[] = L"" LP2NET_VERSION;
const wchar_t kBackupDir[] = L"LP2Netplay-backup";
const wchar_t kVerifyHint[] =
    L"In Steam, right-click Lost Planet 2 > Properties > Installed Files > \"Verify integrity of game files\", "
    L"then run Setup again.";
const wchar_t kNotGame[] = L"Lost Planet 2 is not in this folder (LP2DX9.exe is missing).";
const wchar_t kNotGameHint[] =
    L"Click Browse and pick the game folder. Steam shows it under: right-click Lost Planet 2 > Manage > "
    L"Browse local files.";
const wchar_t kRunning[] = L"Lost Planet 2 is running. Quit the game, then try again.";
const wchar_t kNotOurs[] =
    L"The game's xlive.dll is not Capcom's 2025 file and not LP2 Netplay, so Setup does not touch it.";
const wchar_t kUnreadable[] =
    L"Setup could not read the game's xlive.dll. If the game or an antivirus scan is using it, wait a moment and "
    L"try again.";

std::wstring Join(const std::wstring& dir, const std::wstring& name) {
	return dir + L"\\" + name;
}

std::wstring Lower(std::wstring s) {
	CharLowerBuffW(s.data(), (DWORD)s.size());
	return s;
}

std::wstring Trim(const std::wstring& s) {
	size_t a = s.find_first_not_of(L" \t");
	size_t b = s.find_last_not_of(L" \t");
	return a == std::wstring::npos ? std::wstring() : s.substr(a, b - a + 1);
}

class Sha256 {
public:
	Sha256() {
		if (BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg_, BCRYPT_SHA256_ALGORITHM, nullptr, 0))) {
			BCryptCreateHash(alg_, &hash_, nullptr, 0, nullptr, 0, 0);
		}
	}
	~Sha256() {
		if (hash_) {
			BCryptDestroyHash(hash_);
		}
		if (alg_) {
			BCryptCloseAlgorithmProvider(alg_, 0);
		}
	}
	void Add(const void* data, size_t size) {
		if (hash_) {
			BCryptHashData(hash_, (PUCHAR)data, (ULONG)size, 0);
		}
	}
	std::wstring Hex() {
		BYTE d[32];
		if (!hash_ || !BCRYPT_SUCCESS(BCryptFinishHash(hash_, d, sizeof(d), 0))) {
			return {};
		}
		std::wstring s;
		for (BYTE b : d) {
			wchar_t h[3];
			swprintf_s(h, L"%02x", b);
			s += h;
		}
		return s;
	}

private:
	BCRYPT_ALG_HANDLE alg_ = nullptr;
	BCRYPT_HASH_HANDLE hash_ = nullptr;
};

HANDLE OpenRead(const std::wstring& path) {
	return CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
	                   OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
}

std::wstring HashFile(const std::wstring& path) {
	HANDLE f = OpenRead(path);
	if (f == INVALID_HANDLE_VALUE) {
		return {};
	}
	Sha256 sha;
	std::vector<BYTE> buf(1 << 20);
	DWORD n = 0;
	bool ok;
	while ((ok = ReadFile(f, buf.data(), (DWORD)buf.size(), &n, nullptr) != FALSE) && n) {
		sha.Add(buf.data(), n);
	}
	CloseHandle(f);
	return ok ? sha.Hex() : std::wstring();
}

std::wstring HashBytes(const void* data, size_t size) {
	Sha256 sha;
	sha.Add(data, size);
	return sha.Hex();
}

std::vector<BYTE> ReadBytes(const std::wstring& path, LONGLONG limit) {
	std::vector<BYTE> data;
	HANDLE f = OpenRead(path);
	if (f == INVALID_HANDLE_VALUE) {
		return data;
	}
	LARGE_INTEGER size;
	if (GetFileSizeEx(f, &size) && size.QuadPart <= limit) {
		data.resize((size_t)size.QuadPart);
		DWORD n = 0;
		if (!ReadFile(f, data.data(), (DWORD)data.size(), &n, nullptr) || n != data.size()) {
			data.clear();
		}
	}
	CloseHandle(f);
	return data;
}

bool ContainsWide(const std::vector<BYTE>& data, const wchar_t* text) {
	const BYTE* p = (const BYTE*)text;
	return std::search(data.begin(), data.end(), p, p + wcslen(text) * sizeof(wchar_t)) != data.end();
}

// ProductName and ProductVersion from the version resource (the en-US Unicode block versioninfo.rc writes).
bool ReadVersion(const std::wstring& path, std::wstring& product, std::wstring& version) {
	DWORD unused = 0;
	DWORD size = GetFileVersionInfoSizeW(path.c_str(), &unused);
	if (!size) {
		return false;
	}
	std::vector<BYTE> info(size);
	if (!GetFileVersionInfoW(path.c_str(), 0, size, info.data())) {
		return false;
	}
	auto query = [&](const wchar_t* key) {
		wchar_t* p = nullptr;
		UINT len = 0;
		std::wstring sub = std::wstring(L"\\StringFileInfo\\040904b0\\") + key;
		return VerQueryValueW(info.data(), sub.c_str(), (void**)&p, &len) && p && len ? std::wstring(p, wcsnlen(p, len))
		                                                                               : std::wstring();
	};
	product = query(L"ProductName");
	version = query(L"ProductVersion");
	return true;
}

Dll Classify(const std::wstring& path, std::wstring* version, bool* unreadable) {
	if (!FileExists(path)) {
		return Dll::Missing;
	}
	std::vector<BYTE> data = ReadBytes(path, 64 << 20);
	if (data.empty()) {
		if (unreadable) {
			*unreadable = true;  // locked (an antivirus scan, say) or empty: never taken for ours or Capcom's
		}
		return Dll::Other;
	}
	if (HashBytes(data.data(), data.size()) == kCapcomSha) {
		return Dll::Capcom;
	}
	std::wstring product, ver;
	if (ReadVersion(path, product, ver) && product == L"LP2 Netplay") {
		if (version) {
			*version = ver;
		}
		return Dll::Ours;
	}
	// Test builds before 0.2.0 have no version resource; they are the only DLLs that load xlive_capcom.dll and
	// read lp2net.ini.
	if (ContainsWide(data, L"xlive_capcom.dll") && ContainsWide(data, L"lp2net.ini")) {
		return Dll::Ours;
	}
	return Dll::Other;
}

bool GameRunning(const std::wstring& dir) {
	HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (snap == INVALID_HANDLE_VALUE) {
		return false;
	}
	std::wstring prefix = Lower(dir) + L"\\";
	bool running = false;
	PROCESSENTRY32W pe = {sizeof(pe)};
	for (BOOL ok = Process32FirstW(snap, &pe); ok && !running; ok = Process32NextW(snap, &pe)) {
		std::wstring exe = Lower(pe.szExeFile);
		bool game = exe == L"lp2dx9.exe" || exe == L"lp2dx11.exe";
		if (!game && exe != L"launcher.exe") {
			continue;
		}
		HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
		wchar_t path[MAX_PATH * 2];
		DWORD n = ARRAYSIZE(path);
		if (p && QueryFullProcessImageNameW(p, 0, path, &n)) {
			running = Lower(path).starts_with(prefix);
		} else {
			running = game;  // can't see where it runs from: assume it is this game
		}
		if (p) {
			CloseHandle(p);
		}
	}
	CloseHandle(snap);
	return running;
}

// Games for Windows LIVE files the game no longer uses: the GFWL installer (Steam's install script names a
// "gwfwlsetup.exe" that does not exist, so it never runs) and the per-exe catalog and LIVE config files that
// Microsoft's xlive.dll read. Neither Capcom's stub nor ours reads them.
bool IsLeftover(const std::wstring& name) {
	std::wstring n = Lower(name);
	return n == L"gfwlivesetup.exe" || n == L"xliveredist.msi" || n == L"wllogin_32.msi" || n == L"wllogin_64.msi" ||
	       n.ends_with(L".exe.cat") || n.ends_with(L".exe.cfg");
}

std::vector<GameFile> ListFiles(const std::wstring& dir, bool onlyLeftovers) {
	std::vector<GameFile> files;
	WIN32_FIND_DATAW fd;
	HANDLE h = FindFirstFileW(Join(dir, L"*").c_str(), &fd);
	if (h == INVALID_HANDLE_VALUE) {
		return files;
	}
	do {
		if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && (!onlyLeftovers || IsLeftover(fd.cFileName))) {
			files.push_back({fd.cFileName, ((ULONGLONG)fd.nFileSizeHigh << 32) | fd.nFileSizeLow});
		}
	} while (FindNextFileW(h, &fd));
	FindClose(h);
	return files;
}

std::wstring ErrorText(DWORD err) {
	switch (err) {
	case ERROR_ACCESS_DENIED:
		return L"Windows did not let Setup change files in the game folder. Close Setup, right-click "
		       L"LP2NetSetup.exe, choose \"Run as administrator\" and try again.";
	case ERROR_SHARING_VIOLATION:
	case ERROR_LOCK_VIOLATION:
	case ERROR_USER_MAPPED_FILE:
		return L"A file in the game folder is in use. Quit Lost Planet 2, wait a few seconds and try again.";
	case ERROR_DISK_FULL:
	case ERROR_HANDLE_DISK_FULL:
		return L"The disk is full. Free some space and try again.";
	case ERROR_CRC:
		return L"The file in the game folder did not come out the same as LP2 Netplay's file (an antivirus program "
		       L"may have changed or removed it).";
	}
	wchar_t* msg = nullptr;
	FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
	               err, 0, (LPWSTR)&msg, 0, nullptr);
	std::wstring s = msg ? msg : L"";
	LocalFree(msg);
	while (!s.empty() && (s.back() == L'\n' || s.back() == L'\r' || s.back() == L' ')) {
		s.pop_back();
	}
	return s + L" (Windows error " + std::to_wstring(err) + L")";
}

// Writes next to the target, checks the bytes, then swaps it in, so a failure leaves the old file in place.
DWORD WriteVerified(const std::wstring& path, const void* data, DWORD size) {
	std::wstring tmp = path + L".lp2new";
	std::wstring want = HashBytes(data, size);
	HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (f == INVALID_HANDLE_VALUE) {
		return GetLastError();
	}
	DWORD written = 0;
	DWORD err = 0;
	if (!WriteFile(f, data, size, &written, nullptr) || written != size || !FlushFileBuffers(f)) {
		err = GetLastError();
	}
	CloseHandle(f);
	if (!err && HashFile(tmp) != want) {
		err = ERROR_CRC;
	}
	if (!err) {
		SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);
		if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
			err = GetLastError();
		}
	}
	if (err) {
		DeleteFileW(tmp.c_str());
		return err;
	}
	return HashFile(path) == want ? 0 : ERROR_CRC;
}

bool LoadPayload(const void** data, DWORD* size) {
	HRSRC res = FindResourceW(nullptr, MAKEINTRESOURCEW(IDR_XLIVE), RT_RCDATA);
	HGLOBAL h = res ? LoadResource(nullptr, res) : nullptr;
	*data = h ? LockResource(h) : nullptr;
	*size = res ? SizeofResource(nullptr, res) : 0;
	return *data && *size;
}

std::wstring IniRead(const std::wstring& ini, const wchar_t* section, const wchar_t* key) {
	wchar_t buf[512];
	GetPrivateProfileStringW(section, key, L"", buf, ARRAYSIZE(buf), ini.c_str());
	return buf;
}

bool WriteSettings(const std::wstring& dir, const Settings& s) {
	std::wstring ini = Join(dir, L"lp2net.ini");
	bool ok = true;
	if (s.name) {
		std::wstring name = SanitizeGamertag(*s.name);
		if (!WritePrivateProfileStringW(L"Player", L"Name", name.empty() ? L"Player" : name.c_str(), ini.c_str())) {
			ok = false;
		}
	}
	if (s.peers && !WritePrivateProfileStringW(L"Network", L"Peers", Trim(*s.peers).c_str(), ini.c_str())) {
		ok = false;
	}
	if (!WritePrivateProfileStringW(L"Debug", L"AllowMultipleCopies", L"0", ini.c_str())) {
		ok = false;
	}
	return ok;
}

// Moves the GFWL leftovers into <game>\LP2Netplay-backup (same drive, so nothing is copied or deleted).
void BackUpLeftovers(const std::wstring& dir, const std::vector<GameFile>& files, Result& r) {
	if (files.empty()) {
		r.details.push_back(L"There were no old Games for Windows LIVE files to move.");
		return;
	}
	std::wstring backup = Join(dir, kBackupDir);
	CreateDirectoryW(backup.c_str(), nullptr);
	int moved = 0;
	for (const GameFile& f : files) {
		std::wstring src = Join(dir, f.name);
		std::wstring dst = Join(backup, f.name);
		if (FileExists(dst)) {
			// moved before and put back by Steam since: the backup already holds it
			if (HashFile(dst) == HashFile(src) && DeleteFileW(src.c_str())) {
				++moved;
			} else {
				r.details.push_back(L"Left " + f.name + L" in place: the backup folder already has a different copy.");
			}
			continue;
		}
		if (MoveFileExW(src.c_str(), dst.c_str(), MOVEFILE_WRITE_THROUGH)) {
			++moved;
		} else {
			r.details.push_back(L"Could not move " + f.name + L": " + ErrorText(GetLastError()));
		}
	}
	if (moved) {
		r.details.push_back(L"Moved " + std::to_wstring(moved) + L" old Games for Windows LIVE file(s) to the " +
		                    kBackupDir + L" folder in the game folder. Uninstall puts them back.");
	}
}

bool RestoreLeftovers(const std::wstring& dir, Result& r) {
	std::wstring backup = Join(dir, kBackupDir);
	std::vector<GameFile> files = ListFiles(backup, false);
	int restored = 0;
	bool ok = true;
	for (const GameFile& f : files) {
		std::wstring src = Join(backup, f.name);
		std::wstring dst = Join(dir, f.name);
		if (FileExists(dst)) {
			// Steam put it back already
			if (HashFile(dst) == HashFile(src) && DeleteFileW(src.c_str())) {
				++restored;
			} else {
				r.details.push_back(L"Left " + f.name + L" in the " + kBackupDir +
				                    L" folder: the game folder has a different copy.");
				ok = false;
			}
			continue;
		}
		if (MoveFileExW(src.c_str(), dst.c_str(), MOVEFILE_WRITE_THROUGH)) {
			++restored;
		} else {
			r.details.push_back(L"Could not put back " + f.name + L": " + ErrorText(GetLastError()));
			ok = false;
		}
	}
	if (restored) {
		r.details.push_back(L"Put back " + std::to_wstring(restored) +
		                    L" old Games for Windows LIVE file(s) from the backup folder.");
	}
	RemoveDirectoryW(backup.c_str());  // only when empty
	return ok;
}

Result Fail(Result r, const std::wstring& summary, const std::wstring& hint = {}) {
	r.ok = false;
	r.summary = summary;
	if (!hint.empty()) {
		r.details.push_back(hint);
	}
	return r;
}

std::wstring RegString(HKEY root, const wchar_t* key, const wchar_t* value, REGSAM view) {
	HKEY h;
	if (RegOpenKeyExW(root, key, 0, KEY_QUERY_VALUE | view, &h) != ERROR_SUCCESS) {
		return {};
	}
	wchar_t buf[MAX_PATH * 2] = {};
	DWORD size = sizeof(buf) - sizeof(wchar_t);
	DWORD type = 0;
	LSTATUS st = RegQueryValueExW(h, value, nullptr, &type, (BYTE*)buf, &size);
	RegCloseKey(h);
	return st == ERROR_SUCCESS && (type == REG_SZ || type == REG_EXPAND_SZ) ? buf : L"";
}

// "path" values from Steam's libraryfolders.vdf (UTF-8, backslashes escaped).
std::vector<std::wstring> SteamLibraries(const std::wstring& steam) {
	std::vector<std::wstring> libs{steam};
	std::vector<BYTE> vdf = ReadBytes(Join(steam, L"steamapps\\libraryfolders.vdf"), 1 << 20);
	std::string text(vdf.begin(), vdf.end());
	std::vector<std::string> tokens;
	for (size_t i = 0; i < text.size(); ++i) {
		if (text[i] != '"') {
			continue;
		}
		std::string tok;
		for (++i; i < text.size() && text[i] != '"'; ++i) {
			if (text[i] == '\\' && i + 1 < text.size()) {
				++i;
			}
			tok += text[i];
		}
		tokens.push_back(tok);
	}
	for (size_t i = 0; i + 1 < tokens.size(); ++i) {
		if (_stricmp(tokens[i].c_str(), "path") == 0) {
			std::string& p = tokens[i + 1];
			int n = MultiByteToWideChar(CP_UTF8, 0, p.data(), (int)p.size(), nullptr, 0);
			std::wstring w(n, L'\0');
			MultiByteToWideChar(CP_UTF8, 0, p.data(), (int)p.size(), w.data(), n);
			libs.push_back(w);
		}
	}
	return libs;
}
}  // namespace

bool FileExists(const std::wstring& path) {
	DWORD a = GetFileAttributesW(path.c_str());
	return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring NormalizeDir(const std::wstring& dir) {
	if (dir.empty()) {
		return {};
	}
	wchar_t full[MAX_PATH * 2];
	DWORD n = GetFullPathNameW(dir.c_str(), ARRAYSIZE(full), full, nullptr);
	std::wstring s = n && n < ARRAYSIZE(full) ? full : dir;
	std::replace(s.begin(), s.end(), L'/', L'\\');
	while (s.size() > 3 && s.back() == L'\\') {
		s.pop_back();
	}
	return s;
}

bool IsGameDir(const std::wstring& dir) {
	return !dir.empty() && FileExists(Join(dir, L"LP2DX9.exe"));
}

std::wstring FindGameDir() {
	std::wstring dir =
	    NormalizeDir(RegString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Capcom\\LostPlanet 2", L"installdir", KEY_WOW64_32KEY));
	if (IsGameDir(dir)) {
		return dir;
	}
	std::wstring steam = RegString(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath", 0);
	if (steam.empty()) {
		return {};
	}
	for (const std::wstring& lib : SteamLibraries(NormalizeDir(steam))) {
		dir = NormalizeDir(Join(lib, L"steamapps\\common\\Lost Planet 2"));
		if (IsGameDir(dir)) {
			return dir;
		}
	}
	return {};
}

GameState Inspect(const std::wstring& dir) {
	GameState st;
	st.dir = dir;
	if (!IsGameDir(dir)) {
		return st;
	}
	st.isGame = true;
	st.running = GameRunning(dir);
	st.xlive = Classify(Join(dir, L"xlive.dll"), &st.xliveVersion, &st.xliveUnreadable);
	st.capcom = Classify(Join(dir, L"xlive_capcom.dll"), nullptr, nullptr);
	if (st.capcom == Dll::Ours) {
		st.capcom = Dll::Other;  // ours under Capcom's name would forward to itself
	}
	st.leftovers = ListFiles(dir, true);
	st.backedUp = ListFiles(Join(dir, kBackupDir), false);
	return st;
}

std::wstring SanitizeGamertag(const std::wstring& name) {
	// same rule as the DLL (config.cpp): the game names the save folder after the gamertag
	std::wstring s;
	for (wchar_t c : name) {
		if ((c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9') || c == L' ' ||
		    c == L'_' || c == L'-') {
			s += c;
		}
	}
	s = Trim(s);
	if (s.size() > 15) {
		s.resize(15);
	}
	return s;
}

bool ValidPeers(const std::wstring& peers) {
	return std::all_of(peers.begin(), peers.end(),
	                   [](wchar_t c) { return c < 128 && (iswalnum(c) || wcschr(L" .:,;_-[]", c)); });
}

Settings ReadSettings(const std::wstring& dir) {
	Settings s;
	std::wstring ini = Join(dir, L"lp2net.ini");
	s.name = dir.empty() ? L"" : IniRead(ini, L"Player", L"Name");
	if (s.name->empty()) {
		wchar_t user[256];
		DWORD n = ARRAYSIZE(user);
		s.name = GetUserNameW(user, &n) ? SanitizeGamertag(user) : L"";
	}
	if (s.name->empty()) {
		s.name = L"Player";
	}
	s.peers = dir.empty() ? L"" : IniRead(ini, L"Network", L"Peers");
	return s;
}

Result Install(const std::wstring& dir, const Settings& settings, bool cleanup) {
	Result r;
	GameState st = Inspect(dir);
	if (!st.isGame) {
		return Fail(r, kNotGame, kNotGameHint);
	}
	if (st.running) {
		return Fail(r, kRunning);
	}
	if (settings.peers && !ValidPeers(*settings.peers)) {
		return Fail(r, L"Nothing was changed: the friend's address can only have letters, digits, dots, colons and "
		               L"commas (for example 203.0.113.7 or 203.0.113.7:3074).");
	}
	if (st.xliveUnreadable) {
		return Fail(r, L"Nothing was changed. " + std::wstring(kUnreadable));
	}
	if (st.xlive == Dll::Other) {
		return Fail(r, L"Nothing was changed. " + std::wstring(kNotOurs), kVerifyHint);
	}
	if (st.capcom == Dll::Other) {
		return Fail(r, L"Nothing was changed: xlive_capcom.dll in the game folder is not Capcom's 2025 file.",
		            L"Rename or remove xlive_capcom.dll, let Steam check the game files, then run Setup again. " +
		                std::wstring(kVerifyHint));
	}
	if (st.capcom == Dll::Missing && st.xlive != Dll::Capcom) {
		return Fail(r, L"Nothing was changed: Capcom's original xlive.dll is missing.", kVerifyHint);
	}
	const void* payload;
	DWORD payloadSize;
	if (!LoadPayload(&payload, &payloadSize)) {
		return Fail(r, L"This copy of LP2NetSetup.exe is damaged (LP2 Netplay's file inside it is missing).",
		            L"Download LP2NetSetup.exe again.");
	}

	const std::wstring xlive = Join(dir, L"xlive.dll");
	const std::wstring capcom = Join(dir, L"xlive_capcom.dll");
	bool movedStub = false;
	if (st.xlive == Dll::Capcom && st.capcom == Dll::Missing) {
		if (!MoveFileExW(xlive.c_str(), capcom.c_str(), MOVEFILE_WRITE_THROUGH)) {
			return Fail(r, L"Nothing was changed: Setup could not keep Capcom's xlive.dll as xlive_capcom.dll.",
			            ErrorText(GetLastError()));
		}
		movedStub = true;
		r.details.push_back(L"Kept Capcom's original xlive.dll as xlive_capcom.dll.");
	}
	if (DWORD err = WriteVerified(xlive, payload, payloadSize)) {
		if (movedStub && MoveFileExW(capcom.c_str(), xlive.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
			r.details.clear();  // the game is back as it was
		}
		return Fail(r, L"LP2 Netplay could not be installed.", ErrorText(err));
	}
	if (st.xlive == Dll::Ours && st.xliveVersion == kVersion) {
		r.details.push_back(L"Repaired LP2 Netplay " LP2NET_VERSION " (xlive.dll written again).");
	} else if (st.xlive == Dll::Ours) {
		r.details.push_back(L"Updated LP2 Netplay from " +
		                    (st.xliveVersion.empty() ? std::wstring(L"a test build") : st.xliveVersion) +
		                    L" to " LP2NET_VERSION ".");
	} else {
		r.details.push_back(L"Installed LP2 Netplay " LP2NET_VERSION " as xlive.dll.");
	}
	if (st.capcom == Dll::Capcom && st.xlive == Dll::Capcom) {
		r.details.push_back(L"(Steam had put Capcom's file back, probably during a game update.)");
	}

	if (WriteSettings(dir, settings)) {
		r.details.push_back(L"Saved your settings in lp2net.ini.");
	} else {
		r.details.push_back(L"Could not save the settings in lp2net.ini: " + ErrorText(GetLastError()));
	}
	if (cleanup) {
		BackUpLeftovers(dir, st.leftovers, r);
	}
	r.ok = true;
	r.summary = L"LP2 Netplay " LP2NET_VERSION " is installed. Start Lost Planet 2 from Steam as usual.";
	return r;
}

Result Uninstall(const std::wstring& dir) {
	Result r;
	GameState st = Inspect(dir);
	if (!st.isGame) {
		return Fail(r, kNotGame, kNotGameHint);
	}
	if (st.running) {
		return Fail(r, kRunning);
	}
	bool restored = RestoreLeftovers(dir, r);

	const std::wstring xlive = Join(dir, L"xlive.dll");
	const std::wstring capcom = Join(dir, L"xlive_capcom.dll");
	if ((st.xlive == Dll::Ours || st.xlive == Dll::Missing) && st.capcom != Dll::Missing) {
		SetFileAttributesW(xlive.c_str(), FILE_ATTRIBUTE_NORMAL);
		if (!MoveFileExW(capcom.c_str(), xlive.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
			return Fail(r, L"LP2 Netplay could not be removed.", ErrorText(GetLastError()));
		}
		r.details.push_back(L"Put Capcom's original xlive.dll back.");
		r.summary = L"LP2 Netplay is removed. The game is back to Capcom's version.";
	} else if (st.xlive == Dll::Ours || st.xlive == Dll::Missing) {
		return Fail(r, L"Capcom's original xlive.dll is missing, so Setup cannot put it back.",
		            L"In Steam, right-click Lost Planet 2 > Properties > Installed Files > \"Verify integrity of game "
		            L"files\". That brings Capcom's file back.");
	} else if (st.xlive == Dll::Capcom) {
		if (st.capcom == Dll::Capcom && DeleteFileW(capcom.c_str())) {
			r.details.push_back(L"Removed the extra copy of Capcom's file (xlive_capcom.dll).");
		} else if (st.capcom == Dll::Other) {
			r.details.push_back(L"xlive_capcom.dll is not Capcom's file; Setup left it alone.");
		}
		r.summary = L"LP2 Netplay is not installed: the game already has Capcom's original xlive.dll.";
	} else if (st.xliveUnreadable) {
		return Fail(r, L"Nothing was changed. " + std::wstring(kUnreadable));
	} else {
		r.summary = L"LP2 Netplay is not installed. " + std::wstring(kNotOurs);
	}
	r.details.push_back(L"Your settings (lp2net.ini) and the log (lp2net.log) stay in the game folder.");
	r.ok = restored;
	if (!restored) {
		r.summary += L" Some old Games for Windows LIVE files could not be put back (see below).";
	}
	return r;
}

std::wstring DllText(Dll kind, const std::wstring& version) {
	switch (kind) {
	case Dll::Missing:
		return L"missing";
	case Dll::Capcom:
		return L"Capcom's 2025 file";
	case Dll::Ours:
		return version.empty() ? L"LP2 Netplay (test build)" : L"LP2 Netplay " + version;
	default:
		return L"an unknown file (Setup does not touch it)";
	}
}

std::wstring FileList(const std::vector<GameFile>& files) {
	std::wstring s;
	for (const GameFile& f : files) {
		ULONGLONG kb = (f.size + 1023) / 1024;
		s += (s.empty() ? L"" : L", ") + f.name + L" (" +  // no-break spaces: the window wraps the list
		     (kb >= 1024 ? std::to_wstring((kb + 512) / 1024) + L" MB" : std::to_wstring(kb) + L" KB") + L")";
	}
	return s;
}

std::wstring Describe(const GameState& st) {
	if (!st.isGame) {
		return st.dir.empty() ? L"Lost Planet 2 was not found on this PC. If it is installed, click Browse and pick "
		                        L"its folder (the one with LP2DX9.exe)."
		                      : std::wstring(kNotGame) + L" " + kNotGameHint;
	}
	std::wstring s = st.running ? L"Lost Planet 2 is running: quit the game before you install or uninstall. " : L"";
	switch (st.xlive) {
	case Dll::Ours:
		s += DllText(st.xlive, st.xliveVersion) + L" is installed.";
		if (st.xliveVersion != kVersion) {
			s += L" Click Update to install " LP2NET_VERSION ".";
		}
		if (st.capcom == Dll::Missing) {
			s += L" Capcom's original file (xlive_capcom.dll) is missing: let Steam verify the game files.";
		}
		break;
	case Dll::Capcom:
		s += st.capcom == Dll::Missing ? L"LP2 Netplay is not installed yet. Click Install."
		                               : L"LP2 Netplay is not active: Steam put Capcom's file back (usually after a "
		                                 L"game update). Click Install.";
		break;
	case Dll::Missing:
		s += st.capcom == Dll::Missing ? L"The game's xlive.dll is missing. Let Steam verify the game files."
		                               : L"The game's xlive.dll is missing. Click Repair.";
		break;
	default:
		s += st.xliveUnreadable ? kUnreadable : std::wstring(kNotOurs) + L" Let Steam verify the game files first.";
		break;
	}
	if (st.capcom == Dll::Other) {
		s += L" (xlive_capcom.dll is not Capcom's file.)";
	}
	return s;
}

std::wstring InstallLabel(const GameState& st) {
	if (st.xlive == Dll::Ours) {
		return st.xliveVersion == kVersion ? L"Repair" : L"Update";
	}
	return st.xlive == Dll::Missing && st.capcom != Dll::Missing ? L"Repair" : L"Install";
}

bool CanInstall(const GameState& st) {
	return st.isGame && !st.running && st.xlive != Dll::Other && st.capcom != Dll::Other &&
	       (st.capcom != Dll::Missing || st.xlive == Dll::Capcom);
}

bool CanUninstall(const GameState& st) {
	return st.isGame && !st.running && (st.capcom != Dll::Missing || !st.backedUp.empty());
}

}  // namespace lp2setup
