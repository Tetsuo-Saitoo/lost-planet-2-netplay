// XUser: one local player (index 0), signed in locally, with every privilege granted.
// Contexts and properties are kept so sessions can advertise game type/mode to peers.
#include "config.h"
#include "gfwl.h"
#include "log.h"
#include "net.h"
#include "overlay.h"
#include "presence.h"
#include "steam.h"
#include "util.h"
#include "xnotify.h"

#include <map>
#include <mutex>
#include <vector>

using namespace lp2net;

namespace lp2net {
namespace {
std::mutex g_mutex;
std::map<DWORD, DWORD> g_contexts;
std::map<DWORD, std::vector<BYTE>> g_properties;
}  // namespace

DWORD GetUserContext(DWORD id, DWORD def) {
	std::lock_guard lock(g_mutex);
	auto it = g_contexts.find(id);
	return it == g_contexts.end() ? def : it->second;
}

std::vector<std::pair<DWORD, DWORD>> GetUserContexts() {
	std::lock_guard lock(g_mutex);
	return {g_contexts.begin(), g_contexts.end()};
}

std::vector<std::pair<DWORD, std::vector<BYTE>>> GetUserProperties() {
	std::lock_guard lock(g_mutex);
	return {g_properties.begin(), g_properties.end()};
}

// Online XUIDs carry 0x0009 in the top 16 bits. With Steam it is made from the Steam account (so a gamer card can
// open the right Steam profile); a second copy on the same PC gets a different one.
XUID MyOnlineXuid() {
	XUID x = SteamXuid();
	if (!x) {
		x = 0x0009000000000000ull | (Me().xuid & 0x0000FFFFFFFFFFFFull);
	}
	return Me().slot ? x ^ ((XUID)Me().slot << 40) : x;
}

// What the session code records for the local player: online XUID when signed in online.
XUID MySessionXuid() {
	return Cfg().onlineMode ? MyOnlineXuid() : Me().xuid;
}
}  // namespace lp2net

namespace {
// [Network] OnlineMode: the game then offers its Xbox LIVE menus (Ranked/Player Match, Xbox LIVE connection type).
bool Online() {
	return Cfg().onlineMode;
}

XUID OnlineXuid() {
	return MyOnlineXuid();
}

XUSER_SIGNIN_STATE MySigninState() {
	return Online() ? eXUserSigninState_SignedInToLive : eXUserSigninState_SignedInLocally;
}

// A Steam invite / "Join Game" for a LIVE-style session, waiting for the game to ask (XInviteGetAcceptedInfo).
struct XINVITE_INFO {
	XUID xuidInvitee;
	XUID xuidInviter;
	DWORD dwTitleID;
	XSESSION_INFO hostInfo;
	BOOL fFromGameInvite;
};
std::mutex g_inviteMutex;
bool g_haveInvite = false;
XINVITE_INFO g_invite{};
constexpr DWORD kTitleId = 0x43430808;
}  // namespace

namespace lp2net {
void JoinSessionFromSteam(const XSESSION_INFO& info, DWORD flags, uint64_t hostSteamId, const std::string& hostName) {
	bool live = (flags & XSESSION_CREATE_USES_MATCHMAKING) != 0;
	if (live && Online()) {
		{
			std::lock_guard lock(g_inviteMutex);
			g_invite = {};
			g_invite.xuidInvitee = MyOnlineXuid();
			g_invite.xuidInviter = hostSteamId ? 0x0009535400000000ull | (hostSteamId & 0xFFFFFFFFull) : 0;
			g_invite.dwTitleID = kTitleId;
			g_invite.hostInfo = info;
			g_invite.fFromGameInvite = TRUE;
			g_haveInvite = true;
		}
		LOG("invite: joining %s's online session", hostName.c_str());
		OverlayToast("Joining " + hostName + "'s game...");
		PostNotification(XN_LIVE_INVITE_ACCEPTED, 0);
	}
	else {
		LOG("invite: %s hosts a System Link game (flags 0x%lX)", hostName.c_str(), flags);
		OverlayPrompt("LP2 Netplay", "Joining " + hostName + ": open System Link (or Campaign) and pick Quick Match.\n" +
		                                 hostName + "'s game will be in the list.");
	}
}
}  // namespace lp2net

extern "C" XUSER_SIGNIN_STATE WINAPI XUserGetSigninState(DWORD dwUserIndex) {
	return dwUserIndex == 0 ? MySigninState() : eXUserSigninState_NotSignedIn;
}

extern "C" DWORD WINAPI XUserGetSigninInfo(DWORD dwUserIndex, DWORD dwFlags, XUSER_SIGNIN_INFO* pInfo) {
	if (!pInfo || dwUserIndex >= XUSER_MAX_COUNT) {
		return ERROR_INVALID_PARAMETER;
	}
	memset(pInfo, 0, sizeof(*pInfo));
	pInfo->dwSponsorUserIndex = XUSER_INDEX_NONE;
	if (dwUserIndex != 0) {
		return ERROR_NO_SUCH_USER;
	}
	const auto& me = Me();
	pInfo->UserSigninState = MySigninState();
	if (Online()) {
		pInfo->dwInfoFlags = XUSER_INFO_FLAG_LIVE_ENABLED;
		pInfo->xuid = (dwFlags & XUSER_GET_SIGNIN_INFO_OFFLINE_XUID_ONLY) ? me.xuid : OnlineXuid();
	}
	else {
		// a locally signed-in profile has no online XUID
		pInfo->xuid = (dwFlags & XUSER_GET_SIGNIN_INFO_ONLINE_XUID_ONLY) ? 0 : me.xuid;
	}
	strncpy_s(pInfo->szUserName, me.name.c_str(), _TRUNCATE);
	return ERROR_SUCCESS;
}

extern "C" DWORD WINAPI XUserGetXUID(DWORD dwUserIndex, XUID* pXuid) {
	if (!pXuid) {
		return ERROR_INVALID_PARAMETER;
	}
	if (dwUserIndex != 0) {
		return ERROR_NO_SUCH_USER;
	}
	*pXuid = Online() ? OnlineXuid() : Me().xuid;
	return ERROR_SUCCESS;
}

extern "C" DWORD WINAPI XUserGetName(DWORD dwUserIndex, LPSTR szUserName, DWORD cchUserName) {
	if (!szUserName || !cchUserName) {
		return ERROR_INVALID_PARAMETER;
	}
	if (dwUserIndex != 0) {
		return ERROR_NO_SUCH_USER;
	}
	strncpy_s(szUserName, cchUserName, Me().name.c_str(), _TRUNCATE);
	return ERROR_SUCCESS;
}

extern "C" DWORD WINAPI XUserCheckPrivilege(DWORD dwUserIndex, DWORD privilegeType, BOOL* pfResult) {
	if (!pfResult) {
		return ERROR_INVALID_PARAMETER;
	}
	*pfResult = dwUserIndex == 0;
	return dwUserIndex == 0 ? ERROR_SUCCESS : ERROR_NOT_LOGGED_ON;
}

extern "C" DWORD WINAPI XUserAreUsersFriends(DWORD dwUserIndex, XUID* pXuids, DWORD dwXuidCount, BOOL* pfResult,
                                             XOVERLAPPED* pOverlapped) {
	if (pfResult) {
		*pfResult = FALSE;
	}
	return CompleteOverlapped(pOverlapped, ERROR_SUCCESS, 0);
}

extern "C" void WINAPI XUserSetContext(DWORD dwUserIndex, DWORD dwContextId, DWORD dwContextValue) {
	LOG("XUserSetContext(user=%lu, id=0x%08lX, value=%lu)", dwUserIndex, dwContextId, dwContextValue);
	{
		std::lock_guard lock(g_mutex);
		g_contexts[dwContextId] = dwContextValue;
	}
	PresenceOnContext(dwContextId, dwContextValue);
}

extern "C" DWORD WINAPI XUserGetContext(DWORD dwUserIndex, XUSER_CONTEXT* pContext, XOVERLAPPED* pOverlapped) {
	if (!pContext) {
		return ERROR_INVALID_PARAMETER;
	}
	pContext->dwValue = GetUserContext(pContext->dwContextId, 0);
	return CompleteOverlapped(pOverlapped, ERROR_SUCCESS, 0);
}

static void SetProperty(DWORD dwUserIndex, DWORD dwPropertyId, DWORD cbValue, const void* pvValue) {
	LOG("XUserSetProperty(user=%lu, id=0x%08lX, cb=%lu)", dwUserIndex, dwPropertyId, cbValue);
	{
		std::lock_guard lock(g_mutex);
		auto& v = g_properties[dwPropertyId];
		v.assign((const BYTE*)pvValue, (const BYTE*)pvValue + (pvValue ? cbValue : 0));
	}
	PresenceOnProperty(dwPropertyId, pvValue, cbValue);
}

extern "C" void WINAPI XUserSetProperty(DWORD dwUserIndex, DWORD dwPropertyId, DWORD cbValue, const void* pvValue) {
	SetProperty(dwUserIndex, dwPropertyId, cbValue, pvValue);
}

extern "C" DWORD WINAPI XUserSetPropertyEx(DWORD dwUserIndex, DWORD dwPropertyId, DWORD cbValue, const void* pvValue,
                                           XOVERLAPPED* pOverlapped) {
	SetProperty(dwUserIndex, dwPropertyId, cbValue, pvValue);
	return CompleteOverlapped(pOverlapped, ERROR_SUCCESS, 0);
}

extern "C" DWORD WINAPI XShowSigninUI(DWORD cPanes, DWORD dwFlags) {
	// already signed in; the game only needs the UI open/close pair
	LOG("XShowSigninUI(panes=%lu, flags=0x%lX)", cPanes, dwFlags);
	return ERROR_SUCCESS;
}

extern "C" DWORD WINAPI XShowGuideUI(DWORD dwUserIndex) {
	OverlayShow(true);
	return ERROR_SUCCESS;
}

extern "C" DWORD WINAPI XFriendsCreateEnumerator(DWORD dwUserIndex, DWORD dwStartingIndex, DWORD dwFriendsToReturn,
                                                 DWORD* pcbBuffer, HANDLE* ph) {
	if (pcbBuffer) {
		*pcbBuffer = 1;
	}
	if (ph) {
		*ph = CreateEmptyEnumerator();
	}
	return ERROR_SUCCESS;
}

extern "C" DWORD WINAPI XInviteGetAcceptedInfo(DWORD dwUserIndex, void* pInfo) {
	std::lock_guard lock(g_inviteMutex);
	if (dwUserIndex != 0 || !pInfo || !g_haveInvite) {
		return ERROR_NOT_FOUND;
	}
	memcpy(pInfo, &g_invite, sizeof(g_invite));
	g_haveInvite = false;
	LOG("XInviteGetAcceptedInfo: session hosted by %016llX", g_invite.xuidInviter);
	return ERROR_SUCCESS;
}

extern "C" DWORD WINAPI XOnlineGetNatType() {
	return 1;  // XONLINE_NAT_OPEN
}

extern "C" DWORD WINAPI XOnlineStartup() {
	WSADATA wsa;
	return WSAStartup(MAKEWORD(2, 2), &wsa);
}

extern "C" DWORD WINAPI XOnlineCleanup() {
	return WSACleanup();
}
