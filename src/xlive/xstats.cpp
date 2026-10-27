// LIVE stats (leaderboards), gone with GFWL. Only used when the player is signed in online ([Network] OnlineMode):
// the Online menu reads the player's rank before it opens and reports "Communication error" if that read fails.
// For now every requested player gets an empty row (rank 0, all columns 0).
#include "gfwl.h"
#include "log.h"
#include "net.h"
#include "util.h"

#include <vector>

using namespace lp2net;

namespace {
// natural alignment, like every GFWL structure (see gfwl.h)
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
static_assert(sizeof(XUSER_STATS_SPEC) == 136 && sizeof(XUSER_STATS_COLUMN) == 24 && sizeof(XUSER_STATS_ROW) == 48);

// The one exception to natural alignment: LP2 builds these as 12-byte records (NetworkRankingLive::moveUpdateRanking).
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
static_assert(sizeof(XUSER_RANK_REQUEST) == 12);

constexpr BYTE kDataTypeInt64 = 2;

size_t ResultsSize(DWORD numXuids, DWORD numSpecs, const XUSER_STATS_SPEC* specs) {
	size_t n = sizeof(XUSER_STATS_READ_RESULTS) + numSpecs * sizeof(XUSER_STATS_VIEW);
	for (DWORD s = 0; s < numSpecs; ++s) {
		n += numXuids * (sizeof(XUSER_STATS_ROW) + specs[s].dwNumColumnIds * sizeof(XUSER_STATS_COLUMN));
	}
	return n;
}
}  // namespace

extern "C" DWORD WINAPI XUserReadStats(DWORD dwTitleId, DWORD dwNumXuids, const XUID* pXuids, DWORD dwNumStatsSpecs,
                                       const XUSER_STATS_SPEC* pSpecs, DWORD* pcbResultsBuffer,
                                       XUSER_STATS_READ_RESULTS* pResults, XOVERLAPPED* pOverlapped) {
	if (!pcbResultsBuffer || (dwNumXuids && !pXuids) || (dwNumStatsSpecs && !pSpecs)) {
		return ERROR_INVALID_PARAMETER;
	}
	size_t need = ResultsSize(dwNumXuids, dwNumStatsSpecs, pSpecs);
	if (!pResults || *pcbResultsBuffer < need) {
		*pcbResultsBuffer = (DWORD)need;
		return ERROR_INSUFFICIENT_BUFFER;
	}
	LOG("XUserReadStats(%lu players, %lu views): empty rows", dwNumXuids, dwNumStatsSpecs);
	memset(pResults, 0, need);
	auto* views = (XUSER_STATS_VIEW*)(pResults + 1);
	auto* cursor = (BYTE*)(views + dwNumStatsSpecs);
	pResults->dwNumViews = dwNumStatsSpecs;
	pResults->pViews = views;
	for (DWORD s = 0; s < dwNumStatsSpecs; ++s) {
		XUSER_STATS_VIEW& v = views[s];
		v.dwViewId = pSpecs[s].dwViewId;
		v.dwTotalViewRows = dwNumXuids;
		v.dwNumRows = dwNumXuids;
		v.pRows = (XUSER_STATS_ROW*)cursor;
		cursor += dwNumXuids * sizeof(XUSER_STATS_ROW);
		for (DWORD x = 0; x < dwNumXuids; ++x) {
			XUSER_STATS_ROW& r = v.pRows[x];
			r.xuid = pXuids[x];
			if ((pXuids[x] & 0x0000FFFFFFFFFFFFull) == (Me().xuid & 0x0000FFFFFFFFFFFFull)) {
				strncpy_s(r.szGamertag, Me().name.c_str(), _TRUNCATE);
			}
			r.dwNumColumns = pSpecs[s].dwNumColumnIds;
			r.pColumns = (XUSER_STATS_COLUMN*)cursor;
			cursor += r.dwNumColumns * sizeof(XUSER_STATS_COLUMN);
			for (DWORD c = 0; c < r.dwNumColumns; ++c) {
				r.pColumns[c].wColumnId = pSpecs[s].rgwColumnIds[c];
				r.pColumns[c].Value.type = kDataTypeInt64;
			}
		}
	}
	return CompleteOverlapped(pOverlapped, ERROR_SUCCESS, (DWORD)need);
}

// After a Ranked Match the game asks where the new rating would place the player. Capcom's stub answered 0 without
// completing the request, which the game logs as a failed ranking update. Nobody else is on our leaderboards yet,
// so every rating ranks first.
extern "C" DWORD WINAPI XUserEstimateRankForRating(DWORD dwNumRatingLookups, const XUSER_RANK_REQUEST* pRatingLookups,
                                                   DWORD cbResults, XUSER_ESTIMATE_RANK_RESULTS* pResults,
                                                   XOVERLAPPED* pOverlapped) {
	size_t need = sizeof(XUSER_ESTIMATE_RANK_RESULTS) + dwNumRatingLookups * sizeof(DWORD);
	if ((dwNumRatingLookups && !pRatingLookups) || !pResults) {
		return ERROR_INVALID_PARAMETER;
	}
	if (cbResults < need) {
		return ERROR_INSUFFICIENT_BUFFER;
	}
	LOG("XUserEstimateRankForRating(%lu ratings, view 0x%lX rating %lld): rank 1", dwNumRatingLookups,
	    dwNumRatingLookups ? pRatingLookups[0].dwViewId : 0, dwNumRatingLookups ? pRatingLookups[0].i64Rating : 0);
	pResults->dwNumRankings = dwNumRatingLookups;
	pResults->pdwRankings = (DWORD*)(pResults + 1);
	for (DWORD i = 0; i < dwNumRatingLookups; ++i) {
		pResults->pdwRankings[i] = 1;
	}
	return CompleteOverlapped(pOverlapped, ERROR_SUCCESS, (DWORD)need);
}
