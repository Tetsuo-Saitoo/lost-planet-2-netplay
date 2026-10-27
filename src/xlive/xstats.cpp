// LIVE stats (leaderboards), kept on the player's PC. Only used when the player is signed in online ([Network]
// OnlineMode): the Online menu reads the player's rank before it opens and reports "Communication error" if that
// read fails, and the Ranking screens list the leaderboards.
//
// The game writes its stats through XSessionWriteStats after a match (NetworkRankingLive::moveUpdateRanking). We
// keep every row per (view, player) in <profile>\LP2Netplay\stats.dat, aggregated the way the game's SPA says (Last
// or Max), send our own rows to every LP2 Netplay peer, and keep the rows of the players we played with. Reads and
// the by-rank enumerator answer from those rows; ranks are worked out here by sorting on each view's rating column.
// Call order, structures and gotchas: MODLOG.md, "Stats, leaderboards and achievements".
#include "gfwl.h"
#include "log.h"
#include "net.h"
#include "spa.h"
#include "stats.h"
#include "util.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <vector>

namespace lp2net {
std::wstring ProfileDir();
}

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

struct XSESSION_VIEW_PROPERTIES {
	DWORD dwViewId;
	DWORD dwNumProperties;
	XUSER_PROPERTY* pProperties;
};
static_assert(sizeof(XSESSION_VIEW_PROPERTIES) == 12);

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

constexpr BYTE kTypeInt32 = 1;
constexpr BYTE kTypeInt64 = 2;
constexpr BYTE kTypeDouble = 3;
constexpr BYTE kTypeFloat = 5;
constexpr BYTE kTypeDateTime = 7;
constexpr BYTE kTypeNull = 0xFF;

// TrueSkill view: LIVE computed it from X_PROPERTY_RELATIVE_SCORE / SESSION_TEAM. We only count games and report
// LIVE's starting values (skill 1, mu 3, sigma 1).
constexpr DWORD kSkillView = 0xFFFF0000;
constexpr WORD kSkillColumnFirst = 61;  // X_STATS_COLUMN_SKILL_SKILL, then _GAMESPLAYED, _MU, _SIGMA
constexpr LONGLONG kSkillDefault = 1;

// ---- the store -------------------------------------------------------------------------------------------------
struct Value {
	BYTE type = kTypeNull;
	uint64_t bits = 0;  // the XUSER_DATA union's first 8 bytes
};

struct Row {
	std::string gamertag;
	bool own = false;
	uint64_t stamp = 0;   // FILETIME of the owner's last write: a newer copy from a peer replaces ours
	uint32_t writes = 0;  // stats writes to this view (games played, for the skill view)
	std::map<DWORD, Value> props;
};

std::mutex g_mutex;
bool g_loaded = false;
XUID g_self = 0;
std::map<DWORD, std::map<XUID, Row>> g_rows;  // view -> player -> row
std::set<XUID> g_met;                         // players we were in a session with
int g_readLogs = 0;

Value FromData(const XUSER_DATA& d) {
	Value v;
	switch (d.type) {
	case kTypeInt32:
		v.bits = (uint32_t)d.nData;
		break;
	case kTypeInt64:
	case kTypeDouble:
	case kTypeFloat:
	case kTypeDateTime:
		memcpy(&v.bits, &d.i64Data, sizeof(v.bits));
		break;
	default:
		return v;  // strings and blobs are not kept
	}
	v.type = d.type;
	return v;
}

XUSER_DATA ToData(const Value& v) {
	XUSER_DATA d{};
	d.type = v.type;
	memcpy(&d.i64Data, &v.bits, sizeof(v.bits));
	return d;
}

double AsDouble(const Value& v) {
	double d = 0;
	float f = 0;
	switch (v.type) {
	case kTypeInt32:
		return (int32_t)v.bits;
	case kTypeDouble:
		memcpy(&d, &v.bits, sizeof(d));
		return d;
	case kTypeFloat:
		memcpy(&f, &v.bits, sizeof(f));
		return f;
	default:
		return (double)(int64_t)v.bits;
	}
}

LONGLONG AsInt64(const Value& v) {
	return v.type == kTypeDouble || v.type == kTypeFloat ? std::llround(AsDouble(v)) : v.type == kTypeInt32
	                                                                                     ? (int32_t)v.bits
	                                                                                     : (LONGLONG)v.bits;
}

// The XUSER_DATA type a property holds: its id's top nibble.
BYTE PropertyType(DWORD propertyId) {
	return (BYTE)(propertyId >> 28 & 0xF);
}

bool IsMe(XUID x) {
	return x && (x == g_self || x == MySessionXuid() ||
	             (x & 0x0000FFFFFFFFFFFFull) == (Me().xuid & 0x0000FFFFFFFFFFFFull));
}

// ---- stats.dat ---------------------------------------------------------------------------------------------------
constexpr uint32_t kFileMagic = 0x5332504C;  // "LP2S"
constexpr uint32_t kFileVersion = 1;

std::wstring StatsPath() {
	return ProfileDir() + L"\\LP2Netplay\\stats.dat";
}

struct Out {
	std::vector<BYTE> b;
	template <typename T>
	void Put(const T& v) {
		b.insert(b.end(), (const BYTE*)&v, (const BYTE*)&v + sizeof(v));
	}
};

struct In {
	const BYTE* p;
	size_t size, pos = 0;
	bool ok = true;
	template <typename T>
	T Get() {
		T v{};
		if (!ok || pos + sizeof(T) > size) {
			ok = false;
			return v;
		}
		memcpy(&v, p + pos, sizeof(T));
		pos += sizeof(T);
		return v;
	}
};

void PutTag(Out& o, const std::string& tag) {
	char t[XUSER_NAME_SIZE]{};
	strncpy_s(t, tag.c_str(), _TRUNCATE);
	o.Put(t);
}

std::string GetTag(In& in) {
	auto t = in.Get<std::array<char, XUSER_NAME_SIZE>>();
	return std::string(t.data(), strnlen(t.data(), t.size()));
}

void PutProps(Out& o, const Row& r) {
	o.Put((uint8_t)std::min<size_t>(r.props.size(), 255));
	size_t n = 0;
	for (auto& [id, v] : r.props) {
		if (n++ == 255) {
			break;
		}
		o.Put((uint32_t)id);
		o.Put(v.type);
		o.Put(v.bits);
	}
}

void GetProps(In& in, Row* r) {
	uint8_t n = in.Get<uint8_t>();
	for (uint8_t i = 0; i < n && in.ok; ++i) {
		uint32_t id = in.Get<uint32_t>();
		Value v;
		v.type = in.Get<BYTE>();
		v.bits = in.Get<uint64_t>();
		r->props[id] = v;
	}
}

// Called with g_mutex held.
void Save() {
	Out o;
	o.Put(kFileMagic);
	o.Put(kFileVersion);
	o.Put((uint32_t)g_met.size());
	for (XUID x : g_met) {
		o.Put(x);
	}
	std::vector<std::pair<std::pair<DWORD, XUID>, const Row*>> keep;
	for (auto& [view, players] : g_rows) {
		for (auto& [xuid, row] : players) {
			if (row.own || g_met.count(xuid)) {
				keep.push_back({{view, xuid}, &row});
			}
		}
	}
	if (keep.empty() && GetFileAttributesW(StatsPath().c_str()) == INVALID_FILE_ATTRIBUTES) {
		return;  // nothing to keep yet: no file (programs like nettest join sessions too)
	}
	o.Put((uint32_t)keep.size());
	for (auto& [key, row] : keep) {
		o.Put((uint32_t)key.first);
		o.Put(key.second);
		o.Put((uint8_t)row->own);
		PutTag(o, row->gamertag);
		o.Put(row->stamp);
		o.Put(row->writes);
		PutProps(o, *row);
	}
	if (!WriteFileAtomic(StatsPath(), o.b.data(), o.b.size())) {
		LOG("stats: could not save %ls (%lu)", StatsPath().c_str(), GetLastError());
	}
}

// Called with g_mutex held.
void EnsureLoaded() {
	if (g_loaded) {
		return;
	}
	g_loaded = true;
	g_self = MySessionXuid();
	std::vector<BYTE> data;
	if (!ReadWholeFile(StatsPath(), &data)) {
		LOG("stats: no saved stats yet (%ls)", StatsPath().c_str());
		return;
	}
	In in{data.data(), data.size()};
	if (in.Get<uint32_t>() != kFileMagic || in.Get<uint32_t>() != kFileVersion) {
		LOG("stats: %ls is not a stats file this version reads, ignored", StatsPath().c_str());
		return;
	}
	uint32_t met = in.Get<uint32_t>();
	for (uint32_t i = 0; i < met && in.ok; ++i) {
		g_met.insert(in.Get<XUID>());
	}
	uint32_t rows = in.Get<uint32_t>();
	size_t loaded = 0;
	for (uint32_t i = 0; i < rows && in.ok; ++i) {
		DWORD view = in.Get<uint32_t>();
		XUID xuid = in.Get<XUID>();
		Row r;
		r.own = in.Get<uint8_t>() != 0;
		r.gamertag = GetTag(in);
		r.stamp = in.Get<uint64_t>();
		r.writes = in.Get<uint32_t>();
		GetProps(in, &r);
		if (in.ok) {
			// our own rows follow the player: the XUID changes with or without Steam
			g_rows[view][r.own ? g_self : xuid] = std::move(r);
			++loaded;
		}
	}
	LOG("stats: loaded %zu rows, %zu players met%s", loaded, g_met.size(), in.ok ? "" : " (file truncated)");
}

// ---- peer packets ------------------------------------------------------------------------------------------------
#pragma pack(push, 1)
struct PacketHead {
	uint8_t version;
	uint8_t rows;
	XUID xuid;
	char gamertag[XUSER_NAME_SIZE];
	// rows follow: u32 view, u64 stamp, u32 writes, u8 count, count x {u32 property, u8 type, u64 value}
};
#pragma pack(pop)
constexpr uint8_t kPacketVersion = 1;
constexpr size_t kPacketBudget = 1200;

// Called with g_mutex held.
std::vector<std::vector<BYTE>> OwnPackets() {
	std::vector<std::vector<BYTE>> packets;
	Out cur;
	auto flush = [&] {
		if (cur.b.size() > sizeof(PacketHead)) {
			packets.push_back(std::move(cur.b));
		}
		cur.b.clear();
	};
	for (auto& [view, players] : g_rows) {
		auto it = players.find(g_self);
		if (it == players.end() || !it->second.own) {
			continue;
		}
		Out row;
		row.Put((uint32_t)view);
		row.Put(it->second.stamp);
		row.Put(it->second.writes);
		PutProps(row, it->second);
		if (!cur.b.empty() && (cur.b.size() + row.b.size() > kPacketBudget || cur.b[1] == 255)) {
			flush();
		}
		if (cur.b.empty()) {
			PacketHead h{kPacketVersion, 0, g_self, {}};
			strncpy_s(h.gamertag, Me().name.c_str(), _TRUNCATE);
			cur.Put(h);
		}
		cur.b.insert(cur.b.end(), row.b.begin(), row.b.end());
		++cur.b[1];
	}
	flush();
	return packets;
}

// ---- reads -------------------------------------------------------------------------------------------------------
struct Ranked {
	XUID xuid;
	const Row* row;
	LONGLONG rating;
	DWORD rank;
};

// Called with g_mutex held. The visible rows of a view (ours and the players we met), best first, with
// competition ranks (equal ratings share a rank).
std::vector<Ranked> RankView(DWORD viewId) {
	std::vector<Ranked> out;
	auto v = g_rows.find(viewId);
	if (v == g_rows.end()) {
		return out;
	}
	const SpaView* sv = GameSpa().View(viewId);
	const SpaField* rf = sv ? sv->Rating() : nullptr;
	for (auto& [xuid, row] : v->second) {
		if (!row.own && !g_met.count(xuid)) {
			continue;
		}
		LONGLONG rating = viewId == kSkillView ? kSkillDefault : 0;
		if (rf) {
			auto p = row.props.find(rf->propertyId);
			rating = p != row.props.end() ? AsInt64(p->second) : 0;
		}
		out.push_back({xuid, &row, rating, 0});
	}
	std::stable_sort(out.begin(), out.end(), [](const Ranked& a, const Ranked& b) {
		return a.rating != b.rating ? a.rating > b.rating : a.row->stamp < b.row->stamp;
	});
	for (size_t i = 0; i < out.size(); ++i) {
		out[i].rank = i && out[i].rating == out[i - 1].rating ? out[i - 1].rank : (DWORD)i + 1;
	}
	return out;
}

// What the game gets back, built under the lock and written into its buffer later.
struct OutRow {
	XUID xuid = 0;
	DWORD rank = 0;
	LONGLONG rating = 0;
	std::string gamertag;
	std::vector<XUSER_DATA> columns;
};
struct OutView {
	DWORD viewId = 0, total = 0;
	std::vector<WORD> columnIds;
	std::vector<OutRow> rows;
};

XUSER_DATA Column(DWORD viewId, const SpaView* sv, WORD column, size_t index, const Ranked* r) {
	XUSER_DATA d{};
	if (viewId == kSkillView) {
		size_t which = column >= kSkillColumnFirst && column < kSkillColumnFirst + 4 ? column - kSkillColumnFirst
		                                                                             : index;
		switch (which) {
		case 0:
			d.type = kTypeInt64;
			d.i64Data = r ? r->rating : 0;
			break;
		case 1:
			d.type = kTypeInt64;
			d.i64Data = r ? r->row->writes : 0;
			break;
		default:
			d.type = kTypeDouble;
			d.dblData = which == 2 ? 3.0 : 1.0;
			break;
		}
		return d;
	}
	if (column == kColumnRank) {
		d.type = kTypeInt32;
		d.nData = r ? (LONG)r->rank : 0;
		return d;
	}
	if (column == kColumnRating) {
		d.type = kTypeInt64;
		d.i64Data = r ? r->rating : 0;
		return d;
	}
	const SpaField* f = sv ? sv->ByColumn(column) : nullptr;
	if (!f || PropertyType(f->propertyId) == 4 || PropertyType(f->propertyId) == 6) {
		d.type = f ? kTypeNull : kTypeInt64;  // strings (the gamertag column) are not kept
		return d;
	}
	d.type = PropertyType(f->propertyId);
	if (r) {
		auto p = r->row->props.find(f->propertyId);
		if (p != r->row->props.end()) {
			d = ToData(p->second);
		}
	}
	return d;
}

OutRow MakeRow(const XUSER_STATS_SPEC& spec, const SpaView* sv, XUID xuid, const Ranked* r) {
	OutRow o;
	o.xuid = xuid;
	if (r) {
		o.rank = r->rank;
		o.rating = r->rating;
		o.gamertag = r->row->own ? Me().name : r->row->gamertag;
	}
	else if (IsMe(xuid)) {
		o.gamertag = Me().name;
	}
	DWORD n = std::min<DWORD>(spec.dwNumColumnIds, _countof(spec.rgwColumnIds));
	for (DWORD c = 0; c < n; ++c) {
		o.columns.push_back(Column(spec.dwViewId, sv, spec.rgwColumnIds[c], c, r));
	}
	return o;
}

size_t ResultsSize(const std::vector<OutView>& views) {
	size_t n = sizeof(XUSER_STATS_READ_RESULTS) + views.size() * sizeof(XUSER_STATS_VIEW);
	for (const OutView& v : views) {
		for (const OutRow& r : v.rows) {
			n += sizeof(XUSER_STATS_ROW) + r.columns.size() * sizeof(XUSER_STATS_COLUMN);
		}
	}
	return n;
}

// XUSER_STATS_READ_RESULTS, then the views, then each view's rows and their columns.
void WriteResults(const std::vector<OutView>& views, void* buffer) {
	auto* res = (XUSER_STATS_READ_RESULTS*)buffer;
	memset(res, 0, ResultsSize(views));
	auto* pv = (XUSER_STATS_VIEW*)(res + 1);
	auto* cursor = (BYTE*)(pv + views.size());
	res->dwNumViews = (DWORD)views.size();
	res->pViews = pv;
	for (size_t s = 0; s < views.size(); ++s) {
		const OutView& v = views[s];
		pv[s].dwViewId = v.viewId;
		pv[s].dwTotalViewRows = v.total;
		pv[s].dwNumRows = (DWORD)v.rows.size();
		pv[s].pRows = (XUSER_STATS_ROW*)cursor;
		cursor += v.rows.size() * sizeof(XUSER_STATS_ROW);
		for (size_t i = 0; i < v.rows.size(); ++i) {
			const OutRow& o = v.rows[i];
			XUSER_STATS_ROW& r = pv[s].pRows[i];
			r.xuid = o.xuid;
			r.dwRank = o.rank;
			r.i64Rating = o.rating;
			strncpy_s(r.szGamertag, o.gamertag.c_str(), _TRUNCATE);
			r.dwNumColumns = (DWORD)o.columns.size();
			r.pColumns = (XUSER_STATS_COLUMN*)cursor;
			cursor += o.columns.size() * sizeof(XUSER_STATS_COLUMN);
			for (size_t c = 0; c < o.columns.size(); ++c) {
				r.pColumns[c].wColumnId = v.columnIds[c];
				r.pColumns[c].Value = o.columns[c];
			}
		}
	}
}

OutView StartView(const XUSER_STATS_SPEC& spec, size_t total) {
	OutView v;
	v.viewId = spec.dwViewId;
	v.total = (DWORD)total;
	DWORD n = std::min<DWORD>(spec.dwNumColumnIds, _countof(spec.rgwColumnIds));
	v.columnIds.assign(spec.rgwColumnIds, spec.rgwColumnIds + n);
	return v;
}

std::string SpecText(DWORD n, const XUSER_STATS_SPEC* specs) {
	std::string s;
	for (DWORD i = 0; i < n; ++i) {
		char b[32];
		snprintf(b, sizeof(b), "%sview 0x%lX [", i ? "; " : "", specs[i].dwViewId);
		s += b;
		for (DWORD c = 0; c < specs[i].dwNumColumnIds && c < 64; ++c) {
			snprintf(b, sizeof(b), "%s%X", c ? " " : "", specs[i].rgwColumnIds[c]);
			s += b;
		}
		s += "]";
	}
	return s;
}

// Rows `first`.. of the first spec's ranking, at most `count`; the same players in the other specs. The game's
// "Overall" boards come here (XUserCreateStatsEnumeratorByRank).
std::vector<OutView> Window(DWORD numSpecs, const XUSER_STATS_SPEC* specs, size_t first, size_t count) {
	std::vector<OutView> out;
	std::vector<XUID> players;
	for (DWORD s = 0; s < numSpecs; ++s) {
		auto ranked = RankView(specs[s].dwViewId);
		if (s == 0) {
			for (size_t i = first; i < ranked.size() && players.size() < count; ++i) {
				players.push_back(ranked[i].xuid);
			}
		}
		const SpaView* sv = GameSpa().View(specs[s].dwViewId);
		OutView v = StartView(specs[s], ranked.size());
		for (XUID x : players) {
			auto it = std::find_if(ranked.begin(), ranked.end(), [&](const Ranked& r) { return r.xuid == x; });
			v.rows.push_back(MakeRow(specs[s], sv, x, it != ranked.end() ? &*it : nullptr));
		}
		out.push_back(std::move(v));
	}
	return out;
}

// The enumerator the game reads a Window from: cb == 0 tells it the board is empty.
DWORD WindowEnumerator(std::vector<OutView> views, DWORD* pcbBuffer, HANDLE* ph) {
	bool empty = views.empty() || views[0].rows.empty();
	DWORD need = empty ? 0 : (DWORD)ResultsSize(views);
	*pcbBuffer = need;
	if (empty) {
		*ph = CreateEmptyEnumerator();
		return ERROR_SUCCESS;
	}
	auto shared = std::make_shared<std::vector<OutView>>(std::move(views));
	auto done = std::make_shared<bool>(false);
	*ph = CreateFillEnumerator([shared, done, need](void* buffer, DWORD cb, DWORD* items) -> DWORD {
		*items = 0;
		if (*done) {
			return ERROR_NO_MORE_FILES;
		}
		if (!buffer || cb < need) {
			return ERROR_INSUFFICIENT_BUFFER;
		}
		WriteResults(*shared, buffer);
		*done = true;
		*items = 1;
		return ERROR_SUCCESS;
	});
	return ERROR_SUCCESS;
}
}  // namespace

namespace lp2net {

void StatsWrite(XUID xuid, DWORD numViews, const void* views) {
	if (!views && numViews) {
		return;
	}
	std::vector<std::vector<BYTE>> packets;
	{
		std::lock_guard lock(g_mutex);
		EnsureLoaded();
		if (!IsMe(xuid)) {
			LOG("XSessionWriteStats(%016llX, %lu views): another player's stats, not kept", xuid, numViews);
			return;
		}
		FILETIME ft;
		GetSystemTimeAsFileTime(&ft);
		uint64_t now = (uint64_t)ft.dwHighDateTime << 32 | ft.dwLowDateTime;
		const Spa& spa = GameSpa();
		auto* vp = (const XSESSION_VIEW_PROPERTIES*)views;
		for (DWORD i = 0; i < numViews; ++i) {
			const SpaView* sv = spa.View(vp[i].dwViewId);
			Row& row = g_rows[vp[i].dwViewId][g_self];
			row.own = true;
			row.gamertag = Me().name;
			row.stamp = std::max(now, row.stamp + 1);
			++row.writes;
			std::string text;
			for (DWORD p = 0; p < vp[i].dwNumProperties && vp[i].pProperties; ++p) {
				const XUSER_PROPERTY& prop = vp[i].pProperties[p];
				Value v = FromData(prop.value);
				char b[64];
				if (v.type == kTypeDouble || v.type == kTypeFloat) {
					snprintf(b, sizeof(b), " %lX=%g", prop.dwPropertyId, AsDouble(v));
				}
				else {
					snprintf(b, sizeof(b), " %lX=%lld", prop.dwPropertyId, AsInt64(v));
				}
				text += b;
				if (v.type == kTypeNull) {
					continue;
				}
				const SpaField* f = sv ? sv->ByProperty(prop.dwPropertyId) : nullptr;
				auto old = row.props.find(prop.dwPropertyId);
				if (f && f->aggregation == kAggregateMax && old != row.props.end() &&
				    AsDouble(old->second) >= AsDouble(v)) {
					continue;  // a best score: keep the better one
				}
				row.props[prop.dwPropertyId] = v;
			}
			LOG("XSessionWriteStats: view 0x%lX%s%s", vp[i].dwViewId, sv ? "" : " (not in the SPA)", text.c_str());
		}
		Save();
		packets = OwnPackets();
	}
	SendStatsToPeers(packets);
}

void StatsMetPlayers(const XUID* xuids, DWORD count) {
	std::lock_guard lock(g_mutex);
	EnsureLoaded();
	bool changed = false;
	for (DWORD i = 0; xuids && i < count; ++i) {
		if (xuids[i] && !IsMe(xuids[i]) && g_met.insert(xuids[i]).second) {
			changed = true;
		}
	}
	if (changed) {
		Save();
	}
}

std::vector<std::vector<BYTE>> StatsPackets() {
	std::lock_guard lock(g_mutex);
	EnsureLoaded();
	return OwnPackets();
}

void StatsReceive(uint32_t peerId, const BYTE* body, int cb) {
	In in{body, (size_t)std::max(cb, 0)};
	auto h = in.Get<PacketHead>();
	if (!in.ok || h.version != kPacketVersion || !h.xuid) {
		return;
	}
	std::string tag(h.gamertag, strnlen(h.gamertag, sizeof(h.gamertag)));
	std::lock_guard lock(g_mutex);
	EnsureLoaded();
	if (IsMe(h.xuid)) {
		return;  // never let a copy replace our own rows
	}
	const Spa& spa = GameSpa();
	int took = 0;
	for (uint8_t i = 0; i < h.rows && in.ok; ++i) {
		DWORD view = in.Get<uint32_t>();
		Row r;
		r.gamertag = tag;
		r.stamp = in.Get<uint64_t>();
		r.writes = in.Get<uint32_t>();
		GetProps(in, &r);
		if (!in.ok || (spa.loaded && !spa.View(view))) {
			continue;
		}
		Row& cur = g_rows[view][h.xuid];
		if (cur.stamp < r.stamp || cur.gamertag != tag) {
			cur = std::move(r);
			++took;
		}
	}
	if (took) {
		bool met = g_met.count(h.xuid) != 0;
		LOG("stats: %d rows from %s (peer %08X)%s", took, tag.c_str(), peerId, met ? "" : ", not played with yet");
		if (met) {
			Save();
		}
	}
}

}  // namespace lp2net

extern "C" DWORD WINAPI XUserReadStats(DWORD dwTitleId, DWORD dwNumXuids, const XUID* pXuids, DWORD dwNumStatsSpecs,
                                       const XUSER_STATS_SPEC* pSpecs, DWORD* pcbResultsBuffer,
                                       XUSER_STATS_READ_RESULTS* pResults, XOVERLAPPED* pOverlapped) {
	if (!pcbResultsBuffer || (dwNumXuids && !pXuids) || (dwNumStatsSpecs && !pSpecs)) {
		return ERROR_INVALID_PARAMETER;
	}
	// Every requested player gets a row (rank 0 when we have nothing for them), so the size is known up front.
	size_t need = sizeof(XUSER_STATS_READ_RESULTS) + dwNumStatsSpecs * sizeof(XUSER_STATS_VIEW);
	for (DWORD s = 0; s < dwNumStatsSpecs; ++s) {
		need += dwNumXuids * (sizeof(XUSER_STATS_ROW) +
		                      std::min<DWORD>(pSpecs[s].dwNumColumnIds, 64) * sizeof(XUSER_STATS_COLUMN));
	}
	if (!pResults || *pcbResultsBuffer < need) {
		*pcbResultsBuffer = (DWORD)need;
		return ERROR_INSUFFICIENT_BUFFER;
	}
	std::vector<OutView> views;
	int found = 0;
	{
		std::lock_guard lock(g_mutex);
		EnsureLoaded();
		for (DWORD s = 0; s < dwNumStatsSpecs; ++s) {
			auto ranked = RankView(pSpecs[s].dwViewId);
			const SpaView* sv = GameSpa().View(pSpecs[s].dwViewId);
			OutView v = StartView(pSpecs[s], ranked.size());
			for (DWORD x = 0; x < dwNumXuids; ++x) {
				auto it = std::find_if(ranked.begin(), ranked.end(), [&](const Ranked& r) {
					return r.xuid == pXuids[x] || (r.row->own && IsMe(pXuids[x]));
				});
				found += it != ranked.end();
				v.rows.push_back(MakeRow(pSpecs[s], sv, pXuids[x], it != ranked.end() ? &*it : nullptr));
			}
			views.push_back(std::move(v));
		}
		if (g_readLogs < 40) {
			++g_readLogs;
			LOG("XUserReadStats(%lu players, %s): %d rows", dwNumXuids, SpecText(dwNumStatsSpecs, pSpecs).c_str(),
			    found);
		}
	}
	WriteResults(views, pResults);
	return CompleteOverlapped(pOverlapped, ERROR_SUCCESS, (DWORD)need);
}

extern "C" DWORD WINAPI XUserCreateStatsEnumeratorByRank(DWORD dwTitleId, DWORD dwRankStart, DWORD dwNumRows,
                                                         DWORD dwNumStatsSpecs, const XUSER_STATS_SPEC* pSpecs,
                                                         DWORD* pcbBuffer, HANDLE* ph) {
	if (!pcbBuffer || !ph || !dwNumStatsSpecs || !pSpecs) {
		return ERROR_INVALID_PARAMETER;
	}
	std::vector<OutView> views;
	{
		std::lock_guard lock(g_mutex);
		EnsureLoaded();
		views = Window(dwNumStatsSpecs, pSpecs, dwRankStart ? dwRankStart - 1 : 0, dwNumRows);
		LOG("XUserCreateStatsEnumeratorByRank(from %lu, %lu rows, %s): %zu of %lu", dwRankStart, dwNumRows,
		    SpecText(dwNumStatsSpecs, pSpecs).c_str(), views[0].rows.size(), views[0].total);
	}
	return WindowEnumerator(std::move(views), pcbBuffer, ph);
}

// Rows around a player (LP2 imports this but was not seen calling it).
extern "C" DWORD WINAPI XUserCreateStatsEnumeratorByXuid(DWORD dwTitleId, XUID xuidPivot, DWORD dwNumRows,
                                                         DWORD dwNumStatsSpecs, const XUSER_STATS_SPEC* pSpecs,
                                                         DWORD* pcbBuffer, HANDLE* ph) {
	if (!pcbBuffer || !ph || !dwNumStatsSpecs || !pSpecs) {
		return ERROR_INVALID_PARAMETER;
	}
	std::vector<OutView> views;
	{
		std::lock_guard lock(g_mutex);
		EnsureLoaded();
		auto ranked = RankView(pSpecs[0].dwViewId);
		auto it = std::find_if(ranked.begin(), ranked.end(), [&](const Ranked& r) {
			return r.xuid == xuidPivot || (r.row->own && IsMe(xuidPivot));
		});
		if (it != ranked.end()) {
			size_t at = it - ranked.begin();
			views = Window(dwNumStatsSpecs, pSpecs, at > dwNumRows / 2 ? at - dwNumRows / 2 : 0, dwNumRows);
		}
		LOG("XUserCreateStatsEnumeratorByXuid(%016llX, %lu rows, %s): %zu rows", xuidPivot, dwNumRows,
		    SpecText(dwNumStatsSpecs, pSpecs).c_str(), views.empty() ? 0 : views[0].rows.size());
	}
	return WindowEnumerator(std::move(views), pcbBuffer, ph);
}

// After a Ranked Match the game asks where the new rating would place the player, before it writes the stats.
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
	pResults->dwNumRankings = dwNumRatingLookups;
	pResults->pdwRankings = (DWORD*)(pResults + 1);
	{
		std::lock_guard lock(g_mutex);
		EnsureLoaded();
		for (DWORD i = 0; i < dwNumRatingLookups; ++i) {
			DWORD rank = 1;  // one past everyone else with a better rating
			for (const Ranked& r : RankView(pRatingLookups[i].dwViewId)) {
				rank += !r.row->own && r.rating > pRatingLookups[i].i64Rating;
			}
			pResults->pdwRankings[i] = rank;
			LOG("XUserEstimateRankForRating(view 0x%lX, rating %lld): rank %lu", pRatingLookups[i].dwViewId,
			    pRatingLookups[i].i64Rating, rank);
		}
	}
	return CompleteOverlapped(pOverlapped, ERROR_SUCCESS, (DWORD)need);
}
