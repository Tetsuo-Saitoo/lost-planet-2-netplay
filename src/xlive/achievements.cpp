// Achievements, kept on the player's PC. LP2 on Steam has none, and Capcom's stub answered the game's unlocks with
// a plain 0 the game logs as a failure, so they were lost. We catch XUserWriteAchievements, keep the unlocks in
// <profile>\LP2Netplay\achievements.txt and list them in the overlay with the names and descriptions from the game's
// own SPA (spa.h). The game queues an unlock (NetworkXboxMediator::writeAchievements) only while a player is signed
// in, and expects ERROR_IO_PENDING plus a clean XGetOverlappedResult.
#include "gfwl.h"
#include "log.h"
#include "net.h"
#include "overlay.h"
#include "spa.h"
#include "stats.h"
#include "util.h"

#include <algorithm>
#include <cstdio>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>

namespace lp2net {
std::wstring ProfileDir();

const Spa& GameSpa() {
	static Spa spa;
	static std::once_flag once;
	std::call_once(once, [] {
		std::string error;
		HMODULE exe = nullptr;
		char test[MAX_PATH];
		if (GetEnvironmentVariableA("LP2NET_SPA_EXE", test, MAX_PATH)) {
			// tests (statstest): the game's SPA without the game
			exe = LoadLibraryExA(test, nullptr, LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
		}
		if (LoadSpaResource(exe, &spa, &error)) {
			LOG("spa: %zu achievements, %zu stats views", spa.achievements.size(), spa.views.size());
		}
		else {
			LOG("spa: %s; achievements show without names, stats without ranking columns", error.c_str());
		}
	});
	return spa;
}
}  // namespace lp2net

using namespace lp2net;

namespace {
struct XUSER_ACHIEVEMENT {
	DWORD dwUserIndex;
	DWORD dwAchievementId;
};

struct XACHIEVEMENT_DETAILS {
	DWORD dwId;
	LPWSTR pwszLabel;
	LPWSTR pwszDescription;
	LPWSTR pwszUnachieved;
	DWORD dwImageId;
	DWORD dwCred;
	FILETIME ftAchieved;
	DWORD dwFlags;
};
static_assert(sizeof(XUSER_ACHIEVEMENT) == 8 && sizeof(XACHIEVEMENT_DETAILS) == 36);

constexpr DWORD kLabelChars = 33, kDescChars = 100, kUnachievedChars = 100;  // XACHIEVEMENT_MAX_*_LENGTH
constexpr DWORD kDetailsSize = sizeof(XACHIEVEMENT_DETAILS) + (kLabelChars + kDescChars + kUnachievedChars) * 2;
constexpr DWORD kAchievedOnline = 0x00010000;  // XACHIEVEMENT_DETAILS_ACHIEVED_ONLINE
constexpr DWORD kAchieved = 0x00020000;        // XACHIEVEMENT_DETAILS_ACHIEVED
constexpr DWORD kShowUnachieved = 0x8;         // SPA flag: listed with its description while locked

std::mutex g_mutex;
bool g_loaded = false;
std::map<uint32_t, uint64_t> g_unlocked;  // id -> FILETIME

std::wstring AchievementsPath() {
	return ProfileDir() + L"\\LP2Netplay\\achievements.txt";
}

// Called with g_mutex held.
void EnsureLoaded() {
	if (g_loaded) {
		return;
	}
	g_loaded = true;
	std::vector<BYTE> data;
	if (!ReadWholeFile(AchievementsPath(), &data)) {
		return;
	}
	std::istringstream in(std::string(data.begin(), data.end()));
	std::string line;
	while (std::getline(in, line)) {
		unsigned id = 0;
		unsigned long long when = 0;
		if (!line.empty() && line[0] != '#' && sscanf_s(line.c_str(), "%u %llu", &id, &when) == 2) {
			g_unlocked[id] = when;
		}
	}
	LOG("achievements: %zu unlocked so far", g_unlocked.size());
}

// Called with g_mutex held.
void Save() {
	std::string out = "# LP2 Netplay achievements: id, unlock time (Windows FILETIME, UTC)\r\n";
	for (auto& [id, when] : g_unlocked) {
		char line[64];
		snprintf(line, sizeof(line), "%u %llu\r\n", id, when);
		out += line;
	}
	if (!WriteFileAtomic(AchievementsPath(), out.data(), out.size())) {
		LOG("achievements: could not save %ls (%lu)", AchievementsPath().c_str(), GetLastError());
	}
}

// Writes a UTF-8 string as a NUL-terminated UTF-16 one of at most `chars` characters.
LPWSTR PutString(BYTE*& cursor, const std::string& s, DWORD chars) {
	std::wstring w = Widen(s);
	if (w.size() >= chars) {
		w.resize(chars - 1);
	}
	auto* out = (LPWSTR)cursor;
	memcpy(out, w.c_str(), (w.size() + 1) * sizeof(wchar_t));
	cursor += (w.size() + 1) * sizeof(wchar_t);
	return out;
}
}  // namespace

namespace lp2net {
std::vector<AchievementView> ListAchievements() {
	const Spa& spa = GameSpa();
	std::lock_guard lock(g_mutex);
	EnsureLoaded();
	std::vector<AchievementView> out;
	for (const SpaAchievement& a : spa.achievements) {
		AchievementView v;
		v.id = a.id;
		v.gamerscore = a.gamerscore;
		auto it = g_unlocked.find(a.id);
		v.unlocked = it != g_unlocked.end();
		if (v.unlocked) {
			v.when.dwLowDateTime = (DWORD)it->second;
			v.when.dwHighDateTime = (DWORD)(it->second >> 32);
		}
		v.secret = !v.unlocked && !(a.flags & kShowUnachieved);
		v.name = a.name;
		v.description = a.description;
		out.push_back(std::move(v));
	}
	return out;
}
}  // namespace lp2net

extern "C" DWORD WINAPI XUserWriteAchievements(DWORD dwNumAchievements, const XUSER_ACHIEVEMENT* pAchievements,
                                               XOVERLAPPED* pOverlapped) {
	if (dwNumAchievements && !pAchievements) {
		return ERROR_INVALID_PARAMETER;
	}
	const Spa& spa = GameSpa();
	std::vector<const SpaAchievement*> fresh;
	std::vector<uint32_t> freshIds;
	{
		std::lock_guard lock(g_mutex);
		EnsureLoaded();
		FILETIME ft;
		GetSystemTimeAsFileTime(&ft);
		for (DWORD i = 0; i < dwNumAchievements; ++i) {
			uint32_t id = pAchievements[i].dwAchievementId;
			bool isNew = g_unlocked.emplace(id, (uint64_t)ft.dwHighDateTime << 32 | ft.dwLowDateTime).second;
			const SpaAchievement* a = spa.Achievement(id);
			LOG("XUserWriteAchievements: user %lu, achievement %u '%s'%s", pAchievements[i].dwUserIndex, id,
			    a ? a->name.c_str() : "?", isNew ? ": unlocked" : " (already unlocked)");
			if (isNew) {
				fresh.push_back(a);
				freshIds.push_back(id);
			}
		}
		if (!fresh.empty()) {
			Save();
		}
	}
	for (size_t i = 0; i < fresh.size(); ++i) {
		const SpaAchievement* a = fresh[i];
		OverlayPrompt("Achievement unlocked", a ? a->name + " (" + std::to_string(a->gamerscore) + "G)\n" +
		                                              a->description
		                                        : "Achievement " + std::to_string(freshIds[i]));
	}
	return CompleteOverlapped(pOverlapped, ERROR_SUCCESS, 0);
}

// The Guide's achievement list, for titles that read it (LP2 imports this but was not seen calling it). Only the
// local player's unlocks are known; anyone else's list shows everything locked.
extern "C" DWORD WINAPI XUserCreateAchievementEnumerator(DWORD dwTitleId, DWORD dwUserIndex, XUID xuid,
                                                         DWORD dwDetailFlags, DWORD dwStartingIndex, DWORD cItem,
                                                         DWORD* pcbBuffer, HANDLE* ph) {
	if (!pcbBuffer || !ph || !cItem) {
		return ERROR_INVALID_PARAMETER;
	}
	bool mine = !xuid || xuid == MySessionXuid() || xuid == Me().xuid;
	std::vector<AchievementView> list = ListAchievements();
	std::vector<std::pair<XACHIEVEMENT_DETAILS, const SpaAchievement*>> items;
	for (size_t i = dwStartingIndex; i < GameSpa().achievements.size() && items.size() < cItem; ++i) {
		const SpaAchievement& a = GameSpa().achievements[i];
		XACHIEVEMENT_DETAILS d{};
		d.dwId = a.id;
		d.dwImageId = a.imageId;
		d.dwCred = a.gamerscore;
		d.dwFlags = a.flags;
		if (mine && list[i].unlocked) {
			d.ftAchieved = list[i].when;
			d.dwFlags |= kAchieved | kAchievedOnline;
		}
		items.push_back({d, &a});
	}
	LOG("XUserCreateAchievementEnumerator(user %lu, %016llX, from %lu, %lu items): %zu", dwUserIndex, xuid,
	    dwStartingIndex, cItem, items.size());
	*pcbBuffer = cItem * kDetailsSize;
	auto next = std::make_shared<size_t>(0);
	*ph = CreateFillEnumerator([items, next](void* buffer, DWORD cb, DWORD* count) -> DWORD {
		*count = 0;
		if (*next >= items.size()) {
			return ERROR_NO_MORE_FILES;
		}
		DWORD n = std::min<DWORD>((DWORD)(items.size() - *next), cb / kDetailsSize);
		if (!buffer || !n) {
			return ERROR_INSUFFICIENT_BUFFER;
		}
		// the records first, then their strings
		auto* out = (XACHIEVEMENT_DETAILS*)buffer;
		auto* cursor = (BYTE*)(out + n);
		for (DWORD i = 0; i < n; ++i) {
			auto& [d, a] = items[*next + i];
			out[i] = d;
			out[i].pwszLabel = PutString(cursor, a->name, kLabelChars);
			out[i].pwszDescription = PutString(cursor, a->description, kDescChars);
			out[i].pwszUnachieved = PutString(cursor, a->unachieved, kUnachievedChars);
		}
		*next += n;
		*count = n;
		return ERROR_SUCCESS;
	});
	return ERROR_SUCCESS;
}
