// Stats sharing and achievements through xlive.dll, without the game: two processes (slots 0 and 1) play the part
// of two players the way LP2 drives the API.
//   statstest a <LP2DX9.exe>   (start first) writes stats and unlocks an achievement, then waits for b
//   statstest b <LP2DX9.exe>   plays with a, reads a's shared stats and the leaderboard -> prints PASS or FAIL
// The SPA (stats views, achievements) comes from the game exe. Each run keeps its files in a new folder under
// %TEMP%\lp2net-statstest (LP2NET_PROFILE_DIR), never in the player's Documents.
#include "../src/xlive/gfwl.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {
HMODULE g_dll;
template <typename T>
T Ord(WORD n) {
	auto p = (T)GetProcAddress(g_dll, MAKEINTRESOURCEA(n));
	if (!p) {
		printf("missing ordinal %u\n", n);
		ExitProcess(2);
	}
	return p;
}

struct XUSER_STATS_SPEC {
	DWORD dwViewId;
	DWORD dwNumColumnIds;
	WORD rgwColumnIds[64];
};
struct XUSER_STATS_COLUMN {
	WORD wColumnId;
	XUSER_DATA Value;
};
struct XUSER_STATS_ROW {
	XUID xuid;
	DWORD dwRank;
	LONGLONG i64Rating;
	CHAR szGamertag[XUSER_NAME_SIZE];
	DWORD dwNumColumns;
	XUSER_STATS_COLUMN* pColumns;
};
struct XUSER_STATS_VIEW {
	DWORD dwViewId;
	DWORD dwTotalViewRows;
	DWORD dwNumRows;
	XUSER_STATS_ROW* pRows;
};
struct XUSER_STATS_READ_RESULTS {
	DWORD dwNumViews;
	XUSER_STATS_VIEW* pViews;
};
struct XSESSION_VIEW_PROPERTIES {
	DWORD dwViewId;
	DWORD dwNumProperties;
	XUSER_PROPERTY* pProperties;
};
#pragma pack(push, 4)
struct XUSER_RANK_REQUEST {
	DWORD dwViewId;
	LONGLONG i64Rating;
};
#pragma pack(pop)
struct XUSER_ESTIMATE_RANK_RESULTS {
	DWORD dwNumRankings;
	DWORD* pdwRankings;
};
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

struct Api {
	DWORD(WINAPI* XUserGetXUID)(DWORD, XUID*);
	DWORD(WINAPI* XSessionCreate)(DWORD, DWORD, DWORD, DWORD, ULONGLONG*, XSESSION_INFO*, XOVERLAPPED*, HANDLE*);
	DWORD(WINAPI* XSessionJoinRemote)(HANDLE, DWORD, const XUID*, const BOOL*, XOVERLAPPED*);
	DWORD(WINAPI* XSessionWriteStats)(HANDLE, XUID, DWORD, const XSESSION_VIEW_PROPERTIES*, XOVERLAPPED*);
	DWORD(WINAPI* XUserReadStats)(DWORD, DWORD, const XUID*, DWORD, const XUSER_STATS_SPEC*, DWORD*,
	                              XUSER_STATS_READ_RESULTS*, XOVERLAPPED*);
	DWORD(WINAPI* XUserCreateStatsEnumeratorByRank)(DWORD, DWORD, DWORD, DWORD, const XUSER_STATS_SPEC*, DWORD*,
	                                                HANDLE*);
	DWORD(WINAPI* XUserEstimateRankForRating)(DWORD, const XUSER_RANK_REQUEST*, DWORD, XUSER_ESTIMATE_RANK_RESULTS*,
	                                          XOVERLAPPED*);
	DWORD(WINAPI* XUserWriteAchievements)(DWORD, const XUSER_ACHIEVEMENT*, XOVERLAPPED*);
	DWORD(WINAPI* XUserCreateAchievementEnumerator)(DWORD, DWORD, XUID, DWORD, DWORD, DWORD, DWORD*, HANDLE*);
	DWORD(WINAPI* XEnumerate)(HANDLE, void*, DWORD, DWORD*, XOVERLAPPED*);
	BOOL(WINAPI* XCloseHandle)(HANDLE);
	DWORD(WINAPI* XGetOverlappedResult)(XOVERLAPPED*, DWORD*, BOOL);
} x;

// LP2's battle view (RANK_BATTLE_00) and a training trial view (best score kept)
constexpr DWORD kBattleView = 1, kTrialView = 13;
constexpr DWORD kRating = 0x2000001D, kBattleColumn3 = 0x2000000B, kTrialColumn3 = 0x2000001E;
constexpr WORD kColRank = 0xFFFF, kColRating = 0xFFFE;

struct Shared {
	XUID a;
	LONG done;
};
Shared* g_shared;

bool g_pass = true;
void Check(bool ok, const char* what) {
	printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
	g_pass &= ok;
}

XUSER_PROPERTY Int64(DWORD id, LONGLONG v) {
	XUSER_PROPERTY p{};
	p.dwPropertyId = id;
	p.value.type = 2;
	p.value.i64Data = v;
	return p;
}

// XSessionWriteStats the way LP2 calls it: async, then XGetOverlappedResult
bool Write(XUID me, DWORD view, std::vector<XUSER_PROPERTY> props) {
	XSESSION_VIEW_PROPERTIES v{view, (DWORD)props.size(), props.data()};
	XOVERLAPPED ov{};
	DWORD r = x.XSessionWriteStats(nullptr, me, 1, &v, &ov);
	return r == ERROR_IO_PENDING && x.XGetOverlappedResult(&ov, nullptr, TRUE) == ERROR_SUCCESS;
}

XUSER_STATS_SPEC Spec(DWORD view, std::vector<WORD> columns) {
	XUSER_STATS_SPEC s{view, (DWORD)columns.size(), {}};
	memcpy(s.rgwColumnIds, columns.data(), columns.size() * sizeof(WORD));
	return s;
}

// XUserReadStats the way LP2 calls it: ask the size, then read asynchronously
std::vector<BYTE> Read(const std::vector<XUID>& who, const XUSER_STATS_SPEC& spec) {
	DWORD cb = 0;
	if (x.XUserReadStats(0, (DWORD)who.size(), who.data(), 1, &spec, &cb, nullptr, nullptr) !=
	    ERROR_INSUFFICIENT_BUFFER) {
		return {};
	}
	std::vector<BYTE> buf(cb);
	XOVERLAPPED ov{};
	if (x.XUserReadStats(0, (DWORD)who.size(), who.data(), 1, &spec, &cb, (XUSER_STATS_READ_RESULTS*)buf.data(),
	                     &ov) != ERROR_IO_PENDING ||
	    x.XGetOverlappedResult(&ov, nullptr, TRUE) != ERROR_SUCCESS) {
		return {};
	}
	return buf;
}

const XUSER_STATS_ROW* Row(const std::vector<BYTE>& buf, size_t i) {
	if (buf.empty()) {
		return nullptr;
	}
	auto* r = (const XUSER_STATS_READ_RESULTS*)buf.data();
	return r->dwNumViews && r->pViews[0].dwNumRows > i ? &r->pViews[0].pRows[i] : nullptr;
}

int A() {
	XUID me = 0;
	x.XUserGetXUID(0, &me);
	printf("a: %016llX\n", me);
	Check(Write(me, kBattleView, {Int64(kRating, 500), Int64(0x2000001C, 0x7F), Int64(kBattleColumn3, 42)}),
	      "battle stats written");
	Check(Write(me, kTrialView, {Int64(kRating, 100), Int64(kTrialColumn3, 7)}), "trial score 100 written");
	Check(Write(me, kTrialView, {Int64(kRating, 50), Int64(kTrialColumn3, 7)}), "trial score 50 written");

	auto own = Read({me}, Spec(kBattleView, {kColRank, kColRating, 3}));
	const XUSER_STATS_ROW* r = Row(own, 0);
	Check(r && r->dwRank == 1 && r->i64Rating == 500 && r->dwNumColumns == 3 && r->pColumns[0].Value.nData == 1 &&
	          r->pColumns[2].wColumnId == 3 && r->pColumns[2].Value.i64Data == 42,
	      "own battle row: rank 1, rating 500, column 3 = 42");

	XUSER_ACHIEVEMENT ach{0, 1};
	XOVERLAPPED ov{};
	DWORD res = 1;
	Check(x.XUserWriteAchievements(1, &ach, &ov) == ERROR_IO_PENDING && x.XGetOverlappedResult(&ov, &res, TRUE) == 0,
	      "achievement 1 written (pending, then success)");
	DWORD cb = 0;
	HANDLE h = nullptr;
	x.XUserCreateAchievementEnumerator(0, 0, me, 0xFFFFFFFF, 0, 50, &cb, &h);
	std::vector<BYTE> buf(cb);
	DWORD items = 0;
	x.XEnumerate(h, buf.data(), cb, &items, nullptr);
	x.XCloseHandle(h);
	auto* d = (const XACHIEVEMENT_DETAILS*)buf.data();
	bool found = false;
	for (DWORD i = 0; i < items; ++i) {
		found |= d[i].dwId == 1 && (d[i].dwFlags & 0x00020000) && d[i].pwszLabel && d[i].pwszLabel[0];
	}
	Check(items == 50 && found, "achievement list: 50 entries, #1 unlocked and named");

	g_shared->a = me;
	printf("a: waiting for b\n");
	for (int i = 0; i < 600 && !g_shared->done; ++i) {
		Sleep(100);
	}
	Check(g_shared->done != 0, "b finished");
	printf("a: %s\n", g_pass ? "PASS" : "FAIL");
	return g_pass ? 0 : 1;
}

int B() {
	XUID me = 0;
	x.XUserGetXUID(0, &me);
	for (int i = 0; i < 100 && !g_shared->a; ++i) {
		Sleep(100);
	}
	XUID a = g_shared->a;
	printf("b: %016llX, a is %016llX\n", me, a);
	if (!a) {
		printf("FAIL start statstest a first\n");
		return 1;
	}
	// in a session with a: from now on a's rows are kept and shown
	ULONGLONG nonce = 0;
	XSESSION_INFO info{};
	HANDLE hs = nullptr;
	x.XSessionCreate(XSESSION_CREATE_HOST | XSESSION_CREATE_USES_PEER_NETWORK, 0, 4, 0, &nonce, &info, nullptr, &hs);
	x.XSessionJoinRemote(hs, 1, &a, nullptr, nullptr);

	XUSER_STATS_SPEC battle = Spec(kBattleView, {kColRank, kColRating, 3});
	std::vector<BYTE> got;
	DWORD start = GetTickCount();
	while (GetTickCount() - start < 20000) {
		got = Read({a, me}, battle);
		if (Row(got, 0) && Row(got, 0)->dwRank) {
			break;
		}
		Sleep(200);
	}
	printf("b: a's stats after %lu ms\n", GetTickCount() - start);
	const XUSER_STATS_ROW* r = Row(got, 0);
	Check(r && r->dwRank == 1 && r->i64Rating == 500 && r->szGamertag[0] && r->pColumns[2].Value.i64Data == 42,
	      "a's battle row arrived: rank 1, rating 500, column 3 = 42, gamertag set");
	const XUSER_STATS_ROW* mine = Row(got, 1);
	Check(mine && mine->xuid == me && mine->dwRank == 0, "own row before any write: rank 0");

	auto trial = Read({a}, Spec(kTrialView, {kColRating, 3}));
	r = Row(trial, 0);
	Check(r && r->i64Rating == 100 && r->pColumns[0].Value.i64Data == 100 && r->pColumns[1].Value.i64Data == 7,
	      "a's trial row: best score 100 kept (Max), column 3 = 7");

	Check(Write(me, kBattleView, {Int64(kRating, 700), Int64(kBattleColumn3, 1)}), "own battle stats written");
	DWORD cb = 0;
	HANDLE h = nullptr;
	x.XUserCreateStatsEnumeratorByRank(0, 1, 10, 1, &battle, &cb, &h);
	std::vector<BYTE> board(cb ? cb : 1);
	XOVERLAPPED ov{};
	DWORD e = x.XEnumerate(h, board.data(), cb, nullptr, &ov);
	DWORD er = x.XGetOverlappedResult(&ov, nullptr, TRUE);
	x.XCloseHandle(h);
	auto* res = (const XUSER_STATS_READ_RESULTS*)board.data();
	bool ok = cb && e == ERROR_IO_PENDING && er == 0 && res->dwNumViews == 1 && res->pViews[0].dwNumRows == 2 &&
	          res->pViews[0].dwTotalViewRows == 2;
	if (ok) {
		const XUSER_STATS_ROW* rows = res->pViews[0].pRows;
		printf("  board: #%lu %s %lld, #%lu %s %lld\n", rows[0].dwRank, rows[0].szGamertag, rows[0].i64Rating,
		       rows[1].dwRank, rows[1].szGamertag, rows[1].i64Rating);
		ok = rows[0].xuid == me && rows[0].dwRank == 1 && rows[0].i64Rating == 700 && rows[1].xuid == a &&
		     rows[1].dwRank == 2 && rows[1].i64Rating == 500;
	}
	Check(ok, "leaderboard by rank: b 700 first, a 500 second");

	XUSER_RANK_REQUEST req[2] = {{kBattleView, 600}, {kBattleView, 400}};
	BYTE est[sizeof(XUSER_ESTIMATE_RANK_RESULTS) + 2 * sizeof(DWORD)];
	auto* er2 = (XUSER_ESTIMATE_RANK_RESULTS*)est;
	x.XUserEstimateRankForRating(2, req, sizeof(est), er2, nullptr);
	Check(er2->pdwRankings[0] == 1 && er2->pdwRankings[1] == 2, "estimated rank: 600 -> 1, 400 -> 2");

	InterlockedExchange(&g_shared->done, 1);
	printf("%s\n", g_pass ? "PASS" : "FAIL");
	return g_pass ? 0 : 1;
}
}  // namespace

int main(int argc, char** argv) {
	if (argc < 3 || (strcmp(argv[1], "a") && strcmp(argv[1], "b"))) {
		printf("usage: statstest a|b <LP2DX9.exe>\n");
		return 2;
	}
	char temp[MAX_PATH];
	GetTempPathA(MAX_PATH, temp);
	std::string dir = std::string(temp) + "lp2net-statstest\\" + std::to_string(GetTickCount64()) + "-" + argv[1];
	SetEnvironmentVariableA("LP2NET_PROFILE_DIR", dir.c_str());
	SetEnvironmentVariableA("LP2NET_SPA_EXE", argv[2]);
	printf("%s: files in %s\n", argv[1], dir.c_str());

	HANDLE map = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(Shared),
	                                L"Local\\LP2NetStatsTest");
	g_shared = (Shared*)MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Shared));
	if (!strcmp(argv[1], "a")) {
		*g_shared = {};
	}

	g_dll = LoadLibraryW(L"xlive.dll");
	if (!g_dll) {
		printf("cannot load xlive.dll (%lu)\n", GetLastError());
		return 2;
	}
	x.XUserGetXUID = Ord<decltype(x.XUserGetXUID)>(5261);
	x.XSessionCreate = Ord<decltype(x.XSessionCreate)>(5300);
	x.XSessionJoinRemote = Ord<decltype(x.XSessionJoinRemote)>(5326);
	x.XSessionWriteStats = Ord<decltype(x.XSessionWriteStats)>(5317);
	x.XUserReadStats = Ord<decltype(x.XUserReadStats)>(5281);
	x.XUserCreateStatsEnumeratorByRank = Ord<decltype(x.XUserCreateStatsEnumeratorByRank)>(5284);
	x.XUserEstimateRankForRating = Ord<decltype(x.XUserEstimateRankForRating)>(5346);
	x.XUserWriteAchievements = Ord<decltype(x.XUserWriteAchievements)>(5278);
	x.XUserCreateAchievementEnumerator = Ord<decltype(x.XUserCreateAchievementEnumerator)>(5280);
	x.XEnumerate = Ord<decltype(x.XEnumerate)>(5256);
	x.XCloseHandle = Ord<decltype(x.XCloseHandle)>(5251);
	x.XGetOverlappedResult = Ord<decltype(x.XGetOverlappedResult)>(1083);
	XLIVE_INITIALIZE_INFO init{sizeof(init)};
	Ord<HRESULT(WINAPI*)(XLIVE_INITIALIZE_INFO*, DWORD)>(5297)(&init, 0x31006300);
	Ord<INT(WINAPI*)(const void*)>(51)(nullptr);
	return !strcmp(argv[1], "a") ? A() : B();
}
