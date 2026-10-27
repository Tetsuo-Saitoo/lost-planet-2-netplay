// LIVE matchmaking without the LIVE servers. See matchmaking.h.
//
// A search waits a moment for answers (LAN/peer session queries go out at once, a Steam lobby search takes up to a
// few seconds), then applies the game's query to every session we know of and writes the results the way
// XSessionSearch does: header, result array, then the returned properties/contexts and their strings.
#include "matchmaking.h"

#include "log.h"
#include "net.h"
#include "steam.h"
#include "util.h"

#include <algorithm>
#include <cmath>
#include <mutex>
#include <optional>

namespace lp2net {
std::vector<std::pair<DWORD, DWORD>> GetUserContexts();
std::vector<std::pair<DWORD, std::vector<BYTE>>> GetUserProperties();
XUID MyOnlineXuid();

namespace {
enum class Op { Eq, Ne, Lt, Le, Gt, Ge };
struct QueryFilter {
	DWORD attr;
	Op op;
	DWORD param;
};
struct Query {
	DWORD id;
	const char* name;
	std::vector<QueryFilter> filters;
	std::vector<DWORD> returns;
};
const std::vector<Query> kQueries = {
#include "lp2_queries.inc"
};

constexpr BYTE kTypeContext = 0, kTypeInt32 = 1, kTypeInt64 = 2, kTypeDouble = 3, kTypeUnicode = 4, kTypeFloat = 5,
               kTypeBinary = 6, kTypeDateTime = 7, kTypeNull = 0xFF;

BYTE TypeOfId(DWORD id) {
	return (BYTE)((id >> 28) & 0xF);
}

// Properties LIVE added to every hosted session itself (X_PROPERTY_GAMER_*): LP2's queries return the host's
// gamertag and PUID from them.
constexpr DWORD kPropGamerHostname = 0x40008109, kPropGamerPuid = 0x20008107;
constexpr DWORD kLiveProps[] = {0x10008101, 0x10008102, 0x10008103, 0x50008104, 0x30008105,
                                0x30008106, 0x20008108, 0x10008201, 0x10008202};

SessionAttr Numeric(DWORD id, BYTE type, double v) {
	SessionAttr a{id, type, {}};
	if (type == kTypeInt32 || type == kTypeContext) {
		LONG x = (LONG)v;
		a.value.assign((BYTE*)&x, (BYTE*)&x + 4);
	}
	else if (type == kTypeFloat) {
		float x = (float)v;
		a.value.assign((BYTE*)&x, (BYTE*)&x + 4);
	}
	else if (type == kTypeDouble) {
		a.value.assign((BYTE*)&v, (BYTE*)&v + 8);
	}
	else {
		LONGLONG x = (LONGLONG)v;
		a.value.assign((BYTE*)&x, (BYTE*)&x + 8);
	}
	return a;
}

std::optional<double> AsNumber(BYTE type, const BYTE* p, size_t cb) {
	switch (type) {
	case kTypeContext:
	case kTypeInt32:
		return cb >= 4 ? std::optional<double>(*(const LONG*)p) : std::nullopt;
	case kTypeFloat:
		return cb >= 4 ? std::optional<double>(*(const float*)p) : std::nullopt;
	case kTypeInt64:
	case kTypeDateTime:
		return cb >= 8 ? std::optional<double>((double)*(const LONGLONG*)p) : std::nullopt;
	case kTypeDouble:
		return cb >= 8 ? std::optional<double>(*(const double*)p) : std::nullopt;
	default:
		return std::nullopt;
	}
}

// the value bytes of a search parameter (an XUSER_PROPERTY of the searcher)
std::vector<BYTE> DataBytes(const XUSER_DATA& d) {
	switch (d.type) {
	case kTypeInt32:
	case kTypeFloat:
		return std::vector<BYTE>((const BYTE*)&d.nData, (const BYTE*)&d.nData + 4);
	case kTypeInt64:
	case kTypeDouble:
	case kTypeDateTime:
		return std::vector<BYTE>((const BYTE*)&d.i64Data, (const BYTE*)&d.i64Data + 8);
	case kTypeUnicode:
		return d.string.pwszData ? std::vector<BYTE>((const BYTE*)d.string.pwszData,
		                                             (const BYTE*)d.string.pwszData + d.string.cbData)
		                         : std::vector<BYTE>();
	case kTypeBinary:
		return d.binary.pbData ? std::vector<BYTE>(d.binary.pbData, d.binary.pbData + d.binary.cbData)
		                       : std::vector<BYTE>();
	default:
		return {};
	}
}

bool Compare(Op op, int c) {
	switch (op) {
	case Op::Eq: return c == 0;
	case Op::Ne: return c != 0;
	case Op::Lt: return c < 0;
	case Op::Le: return c <= 0;
	case Op::Gt: return c > 0;
	case Op::Ge: return c >= 0;
	}
	return false;
}

struct Param {
	DWORD id;
	BYTE type;
	std::vector<BYTE> value;
};

const SessionAttr* FindAttr(const SessionAttrs& attrs, DWORD id) {
	for (const auto& a : attrs) {
		if (a.id == id) {
			return &a;
		}
	}
	return nullptr;
}

std::string ValueText(BYTE type, const std::vector<BYTE>& v) {
	auto n = AsNumber(type, v.data(), v.size());
	if (n) {
		char b[32];
		snprintf(b, sizeof(b), "%.0f", *n);
		return b;
	}
	return ToHex(v.data(), std::min<size_t>(v.size(), 16));
}

// Filter: host attribute <op> searcher parameter. A parameter the searcher did not send is no filter.
bool Passes(const Query* q, const SessionAttrs& attrs, const std::vector<Param>& params, std::string* why) {
	if (!q) {
		return true;
	}
	for (const auto& f : q->filters) {
		auto pit = std::find_if(params.begin(), params.end(), [&](const Param& p) { return p.id == f.param; });
		if (pit == params.end()) {
			continue;
		}
		const SessionAttr* a = FindAttr(attrs, f.attr);
		if (!a) {
			char b[64];
			snprintf(b, sizeof(b), "host has no attribute %08lX", f.attr);
			*why = b;
			return false;
		}
		int c;
		auto x = AsNumber(a->type, a->value.data(), a->value.size());
		auto y = AsNumber(pit->type, pit->value.data(), pit->value.size());
		if (x && y) {
			c = *x < *y ? -1 : (*x > *y ? 1 : 0);
		}
		else {
			c = a->value == pit->value ? 0 : (a->value < pit->value ? -1 : 1);
		}
		if (!Compare(f.op, c)) {
			char b[128];
			snprintf(b, sizeof(b), "attribute %08lX: host %s, wanted op %d %s", f.attr,
			         ValueText(a->type, a->value).c_str(), (int)f.op, ValueText(pit->type, pit->value).c_str());
			*why = b;
			return false;
		}
	}
	return true;
}

struct Job {
	DWORD procedure;
	DWORD maxResults;
	std::vector<Param> params;
	std::optional<XNKID> byId;
	DWORD cbBuffer;
	XSESSION_SEARCHRESULT_HEADER* out;
	XOVERLAPPED* ov;
	ULONGLONG started;
	ULONGLONG stampAtStart;
};
std::mutex g_mutex;
std::vector<Job> g_jobs;

constexpr ULONGLONG kLanWaitMs = 1200, kSteamWaitMs = 4000;
constexpr DWORD kPerResultBytes = sizeof(XSESSION_SEARCHRESULT) + 32 * sizeof(XUSER_PROPERTY) +
                                  8 * sizeof(XUSER_CONTEXT) + 160;

// Lay out the matching sessions in the caller's buffer. Returns the bytes used.
DWORD WriteResults(const Job& job) {
	const Query* q = nullptr;
	for (const auto& k : kQueries) {
		if (k.id == job.procedure) {
			q = &k;
		}
	}
	auto sessions = ListSessions();
	// LAN first, then by ping (unknown last)
	std::stable_sort(sessions.begin(), sessions.end(), [](const SessionView& a, const SessionView& b) {
		if (a.lan != b.lan) {
			return a.lan;
		}
		int pa = a.pingMs < 0 ? 99999 : a.pingMs, pb = b.pingMs < 0 ? 99999 : b.pingMs;
		return pa < pb;
	});
	BYTE* base = (BYTE*)job.out;
	BYTE* end = base + job.cbBuffer;
	auto* results = (XSESSION_SEARCHRESULT*)(job.out + 1);
	DWORD maxFit = (DWORD)std::min<size_t>(job.maxResults, (job.cbBuffer - sizeof(*job.out)) / kPerResultBytes);
	BYTE* heap = (BYTE*)(results + maxFit);
	DWORD n = 0;
	for (const auto& s : sessions) {
		if (n >= maxFit) {
			break;
		}
		SessionAttrs attrs;
		UnpackAttrs(s.attrs.data(), s.attrs.size(), &attrs);
		if (job.byId) {
			if (memcmp(&s.info.sessionID, &*job.byId, sizeof(XNKID)) != 0) {
				continue;
			}
		}
		else {
			std::string why;
			if (!(s.flags & XSESSION_CREATE_USES_MATCHMAKING)) {
				why = "not an online (matchmaking) session";
			}
			else if (s.inGame) {
				why = "already playing";
			}
			else if (s.filledPublic >= s.maxPublic) {
				why = "full";
			}
			else {
				Passes(q, attrs, job.params, &why);
			}
			if (!why.empty()) {
				LOG("  skip %s: %s (%zu attributes)", s.host.c_str(), why.c_str(), attrs.size());
				continue;
			}
		}
		std::vector<DWORD> wanted;
		if (q && !job.byId) {
			wanted = q->returns;
		}
		else {
			for (const auto& a : attrs) {
				wanted.push_back(a.id);
			}
		}
		DWORD numCtx = 0, numProp = 0;
		for (DWORD id : wanted) {
			(TypeOfId(id) == kTypeContext ? numCtx : numProp)++;
		}
		auto* props = (XUSER_PROPERTY*)heap;
		auto* ctxs = (XUSER_CONTEXT*)(props + numProp);
		BYTE* strings = (BYTE*)(ctxs + numCtx);
		size_t stringBytes = 0;
		for (DWORD id : wanted) {
			const SessionAttr* a = FindAttr(attrs, id);
			if (a && (a->type == kTypeUnicode || a->type == kTypeBinary)) {
				stringBytes += (a->value.size() + 3) & ~3u;
			}
		}
		if (strings + stringBytes > end) {
			break;
		}
		XSESSION_SEARCHRESULT& r = results[n++];
		memset(&r, 0, sizeof(r));
		r.info = s.info;
		r.dwFilledPublicSlots = s.filledPublic;
		r.dwFilledPrivateSlots = s.filledPrivate;
		r.dwOpenPublicSlots = s.maxPublic > s.filledPublic ? s.maxPublic - s.filledPublic : 0;
		r.dwOpenPrivateSlots = s.maxPrivate > s.filledPrivate ? s.maxPrivate - s.filledPrivate : 0;
		r.cProperties = numProp;
		r.cContexts = numCtx;
		r.pProperties = numProp ? props : nullptr;
		r.pContexts = numCtx ? ctxs : nullptr;
		DWORD ip = 0, ic = 0;
		for (DWORD id : wanted) {
			const SessionAttr* a = FindAttr(attrs, id);
			if (TypeOfId(id) == kTypeContext) {
				ctxs[ic].dwContextId = id;
				ctxs[ic].dwValue = a && a->value.size() >= 4 ? *(const DWORD*)a->value.data() : 0;
				++ic;
				continue;
			}
			XUSER_PROPERTY& p = props[ip++];
			memset(&p, 0, sizeof(p));
			p.dwPropertyId = id;
			p.value.type = a ? a->type : kTypeNull;
			if (!a) {
				continue;
			}
			if (a->type == kTypeUnicode || a->type == kTypeBinary) {
				memcpy(strings, a->value.data(), a->value.size());
				p.value.string.cbData = (DWORD)a->value.size();
				p.value.string.pwszData = (LPWSTR)strings;
				strings += (a->value.size() + 3) & ~3u;
			}
			else {
				memcpy(&p.value.i64Data, a->value.data(), std::min<size_t>(a->value.size(), 8));
			}
		}
		heap = strings;
		LOG("  result: %s (%s), %d/%d players", s.host.c_str(), s.steamLobby && !s.lan ? "Steam" : "network",
		    s.filledPublic + s.filledPrivate, s.maxPublic + s.maxPrivate);
	}
	job.out->dwSearchResults = n;
	job.out->pResults = n ? results : nullptr;
	return (DWORD)(heap - base);
}
}  // namespace

// ---- attributes ------------------------------------------------------------------------------------------------
SessionAttrs HostAttributes() {
	SessionAttrs out;
	for (const auto& [id, v] : GetUserContexts()) {
		out.push_back(Numeric(id, kTypeContext, v));
	}
	for (const auto& [id, v] : GetUserProperties()) {
		out.push_back({id, TypeOfId(id), v});
	}
	auto have = [&](DWORD id) { return FindAttr(out, id) != nullptr; };
	if (!have(kPropGamerHostname)) {
		std::wstring w = Widen(Me().name);
		out.push_back({kPropGamerHostname, kTypeUnicode,
		               std::vector<BYTE>((const BYTE*)w.c_str(), (const BYTE*)(w.c_str() + w.size() + 1))});
	}
	if (!have(kPropGamerPuid)) {
		out.push_back(Numeric(kPropGamerPuid, kTypeInt64, 0));
		XUID x = MyOnlineXuid();
		memcpy(out.back().value.data(), &x, 8);
	}
	for (DWORD id : kLiveProps) {
		if (!have(id)) {
			out.push_back(Numeric(id, TypeOfId(id), 0));
		}
	}
	return out;
}

// [u16 count] then per attribute [u32 id][u8 type][u16 length][value]
std::vector<BYTE> PackAttrs(const SessionAttrs& attrs) {
	std::vector<BYTE> out;
	auto put = [&](const void* p, size_t n) { out.insert(out.end(), (const BYTE*)p, (const BYTE*)p + n); };
	uint16_t count = (uint16_t)attrs.size();
	put(&count, 2);
	for (const auto& a : attrs) {
		uint16_t len = (uint16_t)std::min<size_t>(a.value.size(), 512);
		put(&a.id, 4);
		put(&a.type, 1);
		put(&len, 2);
		put(a.value.data(), len);
	}
	return out;
}

bool UnpackAttrs(const BYTE* data, size_t cb, SessionAttrs* out) {
	out->clear();
	if (cb < 2) {
		return false;
	}
	uint16_t count;
	memcpy(&count, data, 2);
	size_t pos = 2;
	for (uint16_t i = 0; i < count; ++i) {
		if (pos + 7 > cb) {
			return false;
		}
		SessionAttr a;
		uint16_t len;
		memcpy(&a.id, data + pos, 4);
		a.type = data[pos + 4];
		memcpy(&len, data + pos + 5, 2);
		pos += 7;
		if (pos + len > cb) {
			return false;
		}
		a.value.assign(data + pos, data + pos + len);
		pos += len;
		out->push_back(std::move(a));
	}
	return true;
}

std::string ToHex(const void* data, size_t cb) {
	static const char* digits = "0123456789abcdef";
	std::string s(cb * 2, '0');
	for (size_t i = 0; i < cb; ++i) {
		BYTE b = ((const BYTE*)data)[i];
		s[2 * i] = digits[b >> 4];
		s[2 * i + 1] = digits[b & 15];
	}
	return s;
}

bool FromHex(const std::string& hex, std::vector<BYTE>* out) {
	out->clear();
	if (hex.size() % 2) {
		return false;
	}
	auto nib = [](char c) { return c >= '0' && c <= '9' ? c - '0' : (c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1); };
	for (size_t i = 0; i < hex.size(); i += 2) {
		int h = nib(hex[i]), l = nib(hex[i + 1]);
		if (h < 0 || l < 0) {
			return false;
		}
		out->push_back((BYTE)(h << 4 | l));
	}
	return true;
}

// ---- searches --------------------------------------------------------------------------------------------------
DWORD SessionSearch(DWORD procedure, DWORD maxResults, WORD numProps, const XUSER_PROPERTY* props, WORD numContexts,
                    const XUSER_CONTEXT* contexts, const XNKID* byId, DWORD* pcbResults,
                    XSESSION_SEARCHRESULT_HEADER* results, XOVERLAPPED* overlapped) {
	if (!pcbResults) {
		return ERROR_INVALID_PARAMETER;
	}
	maxResults = std::clamp<DWORD>(maxResults, 1, 50);
	DWORD need = sizeof(XSESSION_SEARCHRESULT_HEADER) + maxResults * kPerResultBytes;
	if (!results || *pcbResults < sizeof(XSESSION_SEARCHRESULT_HEADER) + kPerResultBytes) {
		*pcbResults = need;
		return ERROR_INSUFFICIENT_BUFFER;
	}
	Job job{procedure, maxResults, {}, std::nullopt, *pcbResults, results, overlapped, GetTickCount64(),
	        SessionsStamp()};
	for (WORD i = 0; props && i < numProps; ++i) {
		job.params.push_back({props[i].dwPropertyId, props[i].value.type, DataBytes(props[i].value)});
	}
	for (WORD i = 0; contexts && i < numContexts; ++i) {
		job.params.push_back({contexts[i].dwContextId, kTypeContext,
		                      std::vector<BYTE>((const BYTE*)&contexts[i].dwValue,
		                                        (const BYTE*)&contexts[i].dwValue + 4)});
	}
	if (byId) {
		job.byId = *byId;
	}
	for (const auto& p : job.params) {
		LOG("  parameter %08lX = %s", p.id, ValueText(p.type, p.value).c_str());
	}
	results->dwSearchResults = 0;
	results->pResults = nullptr;
	RefreshSessions();
	LOG("search started: query %lu, %u parameter(s)%s%s", procedure, (unsigned)job.params.size(),
	    byId ? ", by session id" : "", SteamReady() ? ", Steam lobbies too" : "");
	if (!overlapped) {
		// synchronous caller: wait here for the answers
		while (GetTickCount64() - job.started < (SteamReady() ? kSteamWaitMs : kLanWaitMs)) {
			if (GetTickCount64() - job.started >= kLanWaitMs && SessionsStamp() != job.stampAtStart) {
				break;
			}
			Sleep(50);
		}
		WriteResults(job);
		return ERROR_SUCCESS;
	}
	overlapped->InternalLow = ERROR_IO_PENDING;
	overlapped->InternalHigh = 0;
	if (overlapped->hEvent) {
		ResetEvent(overlapped->hEvent);
	}
	std::lock_guard lock(g_mutex);
	g_jobs.push_back(job);
	return ERROR_IO_PENDING;
}

void MatchmakingTick() {
	std::vector<Job> done;
	{
		std::lock_guard lock(g_mutex);
		ULONGLONG now = GetTickCount64();
		for (auto it = g_jobs.begin(); it != g_jobs.end();) {
			ULONGLONG age = now - it->started;
			bool steamAnswered = !SteamReady() || SessionsStamp() != it->stampAtStart;
			if (age >= kLanWaitMs && (steamAnswered || age >= kSteamWaitMs)) {
				done.push_back(*it);
				it = g_jobs.erase(it);
			}
			else {
				++it;
			}
		}
	}
	for (const auto& job : done) {
		DWORD used = WriteResults(job);
		LOG("search finished: query %lu, %lu session(s)", job.procedure, job.out->dwSearchResults);
		CompleteOverlapped(job.ov, ERROR_SUCCESS, used);
	}
}

}  // namespace lp2net
