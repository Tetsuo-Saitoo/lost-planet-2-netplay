// XSession: local session bookkeeping. With System Link there is no Live server: the host creates the session
// (we mint its XNKID/XNKEY and put our XNADDR in it), the game tells peers about it over its own sockets, and
// joiners create a client-side copy from the XSESSION_INFO they received. The host's session is also advertised
// by the LP2Net core so the overlay can list it.
#include "gfwl.h"
#include "log.h"
#include "matchmaking.h"
#include "net.h"
#include "steam.h"
#include "util.h"

#include <algorithm>
#include <map>
#include <mutex>
#include <string>
#include <vector>

using namespace lp2net;

namespace lp2net {
namespace {
struct Session {
	DWORD flags = 0;
	DWORD userIndex = 0;
	DWORD maxPublic = 0, maxPrivate = 0;
	XSESSION_STATE state = XSESSION_STATE_LOBBY;
	ULONGLONG nonce = 0;
	XSESSION_INFO info{};
	std::vector<XSESSION_MEMBER> members;
	bool host = false;
};
std::recursive_mutex g_mutex;
std::map<HANDLE, Session> g_sessions;

Session* Find(HANDLE h) {
	auto it = g_sessions.find(h);
	return it == g_sessions.end() ? nullptr : &it->second;
}

// Only multiplayer sessions we host are advertised; single-player "sessions" (stats/presence only) are not.
void Advertise(const Session& s) {
	if (!s.host || !(s.flags & XSESSION_CREATE_USES_PEER_NETWORK) || s.state == XSESSION_STATE_DELETED) {
		return;
	}
	AdvertisedSession a;
	a.info = s.info;
	a.flags = s.flags;
	a.gameType = GetUserContext(X_CONTEXT_GAME_TYPE, 0);
	a.gameMode = GetUserContext(X_CONTEXT_GAME_MODE, 0);
	a.players = (int)s.members.size();
	a.maxPlayers = (int)(s.maxPublic + s.maxPrivate);
	for (const auto& m : s.members) {
		(m.dwFlags & XSESSION_MEMBER_FLAGS_PRIVATE_SLOT) ? ++a.filledPrivate : ++a.filledPublic;
	}
	a.maxPublic = (int)s.maxPublic;
	a.maxPrivate = (int)s.maxPrivate;
	a.inGame = s.state == XSESSION_STATE_INGAME;
	a.attrs = PackAttrs(HostAttributes());
	SetAdvertisedSession(&a);
}

// Steam shows players in the same session as a group in the friends list.
void SteamGroup(const Session* s) {
	if (s && (s->flags & XSESSION_CREATE_USES_PEER_NETWORK) && s->state != XSESSION_STATE_DELETED) {
		SteamSetRichPresence("steam_player_group", ToHex(s->info.sessionID.ab, sizeof(s->info.sessionID.ab)).c_str());
		SteamSetRichPresence("steam_player_group_size", std::to_string(std::max<size_t>(1, s->members.size())).c_str());
	}
	else {
		SteamSetRichPresence("steam_player_group", nullptr);
		SteamSetRichPresence("steam_player_group_size", nullptr);
	}
}

void StopAdvertising(const Session& s) {
	if (s.host) {
		SetAdvertisedSession(nullptr);
	}
}

std::string KidStr(const XNKID& k) {
	char b[20];
	snprintf(b, sizeof(b), "%02x%02x%02x%02x%02x%02x%02x%02x", k.ab[0], k.ab[1], k.ab[2], k.ab[3], k.ab[4],
	         k.ab[5], k.ab[6], k.ab[7]);
	return b;
}
}  // namespace

bool CloseSessionHandle(HANDLE h) {
	std::lock_guard lock(g_mutex);
	Session* s = Find(h);
	if (!s) {
		return false;
	}
	StopAdvertising(*s);
	g_sessions.erase(h);
	return true;
}
}  // namespace lp2net

extern "C" DWORD WINAPI XSessionCreate(DWORD dwFlags, DWORD dwUserIndex, DWORD dwMaxPublicSlots,
                                       DWORD dwMaxPrivateSlots, ULONGLONG* pqwSessionNonce,
                                       XSESSION_INFO* pSessionInfo, XOVERLAPPED* pOverlapped, HANDLE* ph) {
	if (!pSessionInfo || !ph) {
		return ERROR_INVALID_PARAMETER;
	}
	NetStartup();
	Session s;
	s.flags = dwFlags;
	s.userIndex = dwUserIndex;
	s.maxPublic = dwMaxPublicSlots;
	s.maxPrivate = dwMaxPrivateSlots;
	s.host = (dwFlags & XSESSION_CREATE_HOST) != 0;
	s.nonce = RandomU64();
	if (s.host) {
		RandomBytes(s.info.sessionID.ab, sizeof(s.info.sessionID.ab));
		BYTE kind = (dwFlags & XSESSION_CREATE_USES_MATCHMAKING) ? XNET_XNKID_ONLINE_PEER : XNET_XNKID_SYSTEM_LINK;
		s.info.sessionID.ab[0] = (s.info.sessionID.ab[0] & ~XNET_XNKID_MASK) | kind;
		RandomBytes(s.info.keyExchangeKey.ab, sizeof(s.info.keyExchangeKey.ab));
		s.info.hostAddress = LocalXnAddr();
		*pSessionInfo = s.info;
	}
	else {
		s.info = *pSessionInfo;
	}
	if (pqwSessionNonce) {
		*pqwSessionNonce = s.nonce;
	}
	RegisterKey(&s.info.sessionID, &s.info.keyExchangeKey);
	HANDLE h = NewFakeHandle();
	*ph = h;
	{
		std::lock_guard lock(g_mutex);
		g_sessions[h] = s;
		Advertise(s);
		SteamGroup(&s);
	}
	LOG("XSessionCreate(flags=0x%lX user=%lu pub=%lu priv=%lu) -> %p %s kid=%s host=%s", dwFlags, dwUserIndex,
	    dwMaxPublicSlots, dwMaxPrivateSlots, h, s.host ? "HOST" : "client", KidStr(s.info.sessionID).c_str(),
	    XnAddrToString(s.info.hostAddress).c_str());
	return CompleteOverlapped(pOverlapped, ERROR_SUCCESS, 0);
}

extern "C" DWORD WINAPI XSessionDelete(HANDLE hSession, XOVERLAPPED* pOverlapped) {
	LOG("XSessionDelete(%p)", hSession);
	std::lock_guard lock(g_mutex);
	if (Session* s = Find(hSession)) {
		s->state = XSESSION_STATE_DELETED;
		SteamGroup(nullptr);
		UnregisterKey(&s->info.sessionID);
		StopAdvertising(*s);
	}
	return CompleteOverlapped(pOverlapped, ERROR_SUCCESS, 0);
}

extern "C" DWORD WINAPI XSessionStart(HANDLE hSession, DWORD dwFlags, XOVERLAPPED* pOverlapped) {
	LOG("XSessionStart(%p, 0x%lX)", hSession, dwFlags);
	std::lock_guard lock(g_mutex);
	if (Session* s = Find(hSession)) {
		s->state = XSESSION_STATE_INGAME;
		Advertise(*s);
	}
	return CompleteOverlapped(pOverlapped, ERROR_SUCCESS, 0);
}

extern "C" DWORD WINAPI XSessionEnd(HANDLE hSession, XOVERLAPPED* pOverlapped) {
	LOG("XSessionEnd(%p)", hSession);
	std::lock_guard lock(g_mutex);
	if (Session* s = Find(hSession)) {
		s->state = XSESSION_STATE_LOBBY;
		Advertise(*s);
	}
	return CompleteOverlapped(pOverlapped, ERROR_SUCCESS, 0);
}

extern "C" DWORD WINAPI XSessionModify(HANDLE hSession, DWORD dwFlags, DWORD dwMaxPublicSlots,
                                       DWORD dwMaxPrivateSlots, XOVERLAPPED* pOverlapped) {
	LOG("XSessionModify(%p, flags=0x%lX pub=%lu priv=%lu)", hSession, dwFlags, dwMaxPublicSlots, dwMaxPrivateSlots);
	std::lock_guard lock(g_mutex);
	if (Session* s = Find(hSession)) {
		s->flags = (s->flags & ~XSESSION_CREATE_MODIFIERS_MASK) | (dwFlags & XSESSION_CREATE_MODIFIERS_MASK);
		s->maxPublic = dwMaxPublicSlots;
		s->maxPrivate = dwMaxPrivateSlots;
		Advertise(*s);
	}
	return CompleteOverlapped(pOverlapped, ERROR_SUCCESS, 0);
}

extern "C" DWORD WINAPI XSessionMigrateHost(HANDLE hSession, DWORD dwUserIndex, XSESSION_INFO* pSessionInfo,
                                            XOVERLAPPED* pOverlapped) {
	std::lock_guard lock(g_mutex);
	Session* s = Find(hSession);
	if (!s || !pSessionInfo) {
		return CompleteOverlapped(pOverlapped, ERROR_INVALID_PARAMETER, 0);
	}
	if (dwUserIndex != XUSER_INDEX_NONE) {
		// we are the new host: same session id and key, our address
		s->host = true;
		s->info.hostAddress = LocalXnAddr();
		*pSessionInfo = s->info;
		Advertise(*s);
	}
	else {
		StopAdvertising(*s);
		s->host = false;
		s->info = *pSessionInfo;
	}
	LOG("XSessionMigrateHost(%p, user=%lu) -> host=%s", hSession, dwUserIndex,
	    XnAddrToString(s->info.hostAddress).c_str());
	return CompleteOverlapped(pOverlapped, ERROR_SUCCESS, 0);
}

static DWORD JoinMembers(HANDLE hSession, DWORD count, const XUID* xuids, const DWORD* userIndexes,
                         const BOOL* pfPrivate, XOVERLAPPED* pOverlapped) {
	std::lock_guard lock(g_mutex);
	Session* s = Find(hSession);
	if (!s) {
		return CompleteOverlapped(pOverlapped, ERROR_INVALID_HANDLE, 0);
	}
	for (DWORD i = 0; i < count; ++i) {
		XSESSION_MEMBER m{};
		m.xuidOnline = xuids ? xuids[i] : MySessionXuid();
		m.dwUserIndex = userIndexes ? userIndexes[i] : XUSER_INDEX_NONE;
		m.dwFlags = (pfPrivate && pfPrivate[i]) ? XSESSION_MEMBER_FLAGS_PRIVATE_SLOT : 0;
		// LP2 joins remote players by their *online* XUID, which is 0 for locally signed-in profiles, so zero XUIDs
		// are separate players and never merged.
		auto same = [&](const XSESSION_MEMBER& o) { return m.xuidOnline && o.xuidOnline == m.xuidOnline; };
		if (std::find_if(s->members.begin(), s->members.end(), same) == s->members.end()) {
			s->members.push_back(m);
		}
		LOG("  join %016llX (user %lu)%s", m.xuidOnline, m.dwUserIndex, m.dwFlags ? " private" : "");
	}
	SteamGroup(s);
	Advertise(*s);
	return CompleteOverlapped(pOverlapped, ERROR_SUCCESS, 0);
}

static DWORD LeaveMembers(HANDLE hSession, DWORD count, const XUID* xuids, XOVERLAPPED* pOverlapped) {
	std::lock_guard lock(g_mutex);
	Session* s = Find(hSession);
	if (!s) {
		return CompleteOverlapped(pOverlapped, ERROR_INVALID_HANDLE, 0);
	}
	for (DWORD i = 0; i < count; ++i) {
		XUID x = xuids ? xuids[i] : MySessionXuid();
		// remove one matching entry (several remote players can share XUID 0)
		auto it = std::find_if(s->members.begin(), s->members.end(),
		                       [&](const XSESSION_MEMBER& m) { return m.xuidOnline == x; });
		if (it != s->members.end()) {
			s->members.erase(it);
		}
		LOG("  leave %016llX", x);
	}
	SteamGroup(s);
	Advertise(*s);
	return CompleteOverlapped(pOverlapped, ERROR_SUCCESS, 0);
}

extern "C" DWORD WINAPI XSessionJoinLocal(HANDLE hSession, DWORD dwUserCount, const DWORD* pdwUserIndexes,
                                          const BOOL* pfPrivateSlots, XOVERLAPPED* pOverlapped) {
	LOG("XSessionJoinLocal(%p, %lu)", hSession, dwUserCount);
	std::vector<XUID> xuids(dwUserCount, MySessionXuid());
	return JoinMembers(hSession, dwUserCount, xuids.data(), pdwUserIndexes, pfPrivateSlots, pOverlapped);
}

extern "C" DWORD WINAPI XSessionJoinRemote(HANDLE hSession, DWORD dwXuidCount, const XUID* pXuids,
                                           const BOOL* pfPrivateSlots, XOVERLAPPED* pOverlapped) {
	LOG("XSessionJoinRemote(%p, %lu)", hSession, dwXuidCount);
	return JoinMembers(hSession, dwXuidCount, pXuids, nullptr, pfPrivateSlots, pOverlapped);
}

extern "C" DWORD WINAPI XSessionLeaveLocal(HANDLE hSession, DWORD dwUserCount, const DWORD* pdwUserIndexes,
                                           XOVERLAPPED* pOverlapped) {
	LOG("XSessionLeaveLocal(%p, %lu)", hSession, dwUserCount);
	return LeaveMembers(hSession, dwUserCount, nullptr, pOverlapped);
}

extern "C" DWORD WINAPI XSessionLeaveRemote(HANDLE hSession, DWORD dwXuidCount, const XUID* pXuids,
                                            XOVERLAPPED* pOverlapped) {
	LOG("XSessionLeaveRemote(%p, %lu)", hSession, dwXuidCount);
	return LeaveMembers(hSession, dwXuidCount, pXuids, pOverlapped);
}

extern "C" DWORD WINAPI XSessionGetDetails(HANDLE hSession, DWORD* pcbResultsBuffer,
                                           XSESSION_LOCAL_DETAILS* pSessionDetails, XOVERLAPPED* pOverlapped) {
	if (!pcbResultsBuffer) {
		return ERROR_INVALID_PARAMETER;
	}
	std::lock_guard lock(g_mutex);
	Session* s = Find(hSession);
	if (!s) {
		return CompleteOverlapped(pOverlapped, ERROR_INVALID_HANDLE, 0);
	}
	DWORD need = sizeof(XSESSION_LOCAL_DETAILS) + (DWORD)(s->members.size() * sizeof(XSESSION_MEMBER));
	if (!pSessionDetails || *pcbResultsBuffer < need) {
		*pcbResultsBuffer = need;
		return ERROR_INSUFFICIENT_BUFFER;
	}
	XSESSION_LOCAL_DETAILS& d = *pSessionDetails;
	DWORD usedPublic = 0, usedPrivate = 0;
	for (const auto& m : s->members) {
		(m.dwFlags & XSESSION_MEMBER_FLAGS_PRIVATE_SLOT) ? ++usedPrivate : ++usedPublic;
	}
	d.dwUserIndexHost = s->host ? s->userIndex : XUSER_INDEX_NONE;
	d.dwGameType = GetUserContext(X_CONTEXT_GAME_TYPE, 0);
	d.dwGameMode = GetUserContext(X_CONTEXT_GAME_MODE, 0);
	d.dwFlags = s->flags;
	d.dwMaxPublicSlots = s->maxPublic;
	d.dwMaxPrivateSlots = s->maxPrivate;
	d.dwAvailablePublicSlots = s->maxPublic > usedPublic ? s->maxPublic - usedPublic : 0;
	d.dwAvailablePrivateSlots = s->maxPrivate > usedPrivate ? s->maxPrivate - usedPrivate : 0;
	d.dwActualMemberCount = (DWORD)s->members.size();
	d.dwReturnedMemberCount = (DWORD)s->members.size();
	d.eState = s->state;
	d.qwNonce = s->nonce;
	d.sessionInfo = s->info;
	memset(&d.xnkidArbitration, 0, sizeof(d.xnkidArbitration));
	d.pSessionMembers = (XSESSION_MEMBER*)(&d + 1);
	std::copy(s->members.begin(), s->members.end(), d.pSessionMembers);
	return CompleteOverlapped(pOverlapped, ERROR_SUCCESS, 0);
}

// LIVE matchmaking searches: answered from every session we know of (LAN, peers, Steam lobbies); matchmaking.cpp.
extern "C" DWORD WINAPI XSessionSearch(DWORD dwProcedureIndex, DWORD dwUserIndex, DWORD dwNumResults,
                                       WORD wNumProperties, WORD wNumContexts, XUSER_PROPERTY* pSearchProperties,
                                       XUSER_CONTEXT* pSearchContexts, DWORD* pcbResultsBuffer,
                                       XSESSION_SEARCHRESULT_HEADER* pSearchResults, XOVERLAPPED* pOverlapped) {
	LOG("XSessionSearch(proc=%lu, n=%lu)", dwProcedureIndex, dwNumResults);
	return SessionSearch(dwProcedureIndex, dwNumResults, wNumProperties, pSearchProperties, wNumContexts,
	                     pSearchContexts, nullptr, pcbResultsBuffer, pSearchResults, pOverlapped);
}

extern "C" DWORD WINAPI XSessionSearchEx(DWORD dwProcedureIndex, DWORD dwUserIndex, DWORD dwNumResults,
                                         DWORD dwNumUsers, WORD wNumProperties, WORD wNumContexts,
                                         XUSER_PROPERTY* pSearchProperties, XUSER_CONTEXT* pSearchContexts,
                                         DWORD* pcbResultsBuffer, XSESSION_SEARCHRESULT_HEADER* pSearchResults,
                                         XOVERLAPPED* pOverlapped) {
	LOG("XSessionSearchEx(proc=%lu, n=%lu, users=%lu)", dwProcedureIndex, dwNumResults, dwNumUsers);
	return SessionSearch(dwProcedureIndex, dwNumResults, wNumProperties, pSearchProperties, wNumContexts,
	                     pSearchContexts, nullptr, pcbResultsBuffer, pSearchResults, pOverlapped);
}

extern "C" DWORD WINAPI XSessionSearchByID(XNKID sessionID, DWORD dwUserIndex, DWORD* pcbResultsBuffer,
                                           XSESSION_SEARCHRESULT_HEADER* pSearchResults, XOVERLAPPED* pOverlapped) {
	LOG("XSessionSearchByID(%s)", KidStr(sessionID).c_str());
	return SessionSearch(0, 1, 0, nullptr, 0, nullptr, &sessionID, pcbResultsBuffer, pSearchResults, pOverlapped);
}

struct XSESSION_REGISTRANT {
	ULONGLONG qwMachineID;
	DWORD bTrustworthiness;
	DWORD bNumUsers;
	XUID* rgUsers;
};
struct XSESSION_REGISTRATION_RESULTS {
	DWORD wNumRegistrants;
	XSESSION_REGISTRANT* rgRegistrants;
};

extern "C" DWORD WINAPI XSessionArbitrationRegister(HANDLE hSession, DWORD dwFlags, ULONGLONG qwSessionNonce,
                                                    DWORD* pcbResultsBuffer,
                                                    XSESSION_REGISTRATION_RESULTS* pResults,
                                                    XOVERLAPPED* pOverlapped) {
	if (!pcbResultsBuffer) {
		return ERROR_INVALID_PARAMETER;
	}
	std::lock_guard lock(g_mutex);
	Session* s = Find(hSession);
	size_t n = s ? s->members.size() : 0;
	DWORD need = (DWORD)(sizeof(XSESSION_REGISTRATION_RESULTS) + n * (sizeof(XSESSION_REGISTRANT) + sizeof(XUID)));
	if (!pResults || *pcbResultsBuffer < need) {
		*pcbResultsBuffer = need;
		return ERROR_INSUFFICIENT_BUFFER;
	}
	auto* regs = (XSESSION_REGISTRANT*)(pResults + 1);
	auto* users = (XUID*)(regs + n);
	pResults->wNumRegistrants = (DWORD)n;
	pResults->rgRegistrants = regs;
	for (size_t i = 0; i < n; ++i) {
		users[i] = s->members[i].xuidOnline;
		regs[i] = {users[i], 1, 1, &users[i]};
	}
	return CompleteOverlapped(pOverlapped, ERROR_SUCCESS, 0);
}

extern "C" DWORD WINAPI XSessionWriteStats(HANDLE hSession, XUID xuid, DWORD dwNumViews, const void* pViews,
                                           XOVERLAPPED* pOverlapped) {
	return CompleteOverlapped(pOverlapped, ERROR_SUCCESS, 0);
}
