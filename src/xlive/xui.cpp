// GFWL Guide screens the game asks for (friends, invites, gamer cards, message boxes, keyboard). With Steam they
// open the matching Steam overlay page: the lobby's "Game invite" opens Steam's invite dialog for our lobby, a gamer
// card opens that player's Steam profile. Without Steam (or for the rest) the call goes to Capcom's stub as before.
// Every call is logged, so the log shows which of the game's own menu entries open them.
#include "gfwl.h"
#include "log.h"
#include "overlay.h"
#include "steam.h"
#include "util.h"

#include <string>

namespace lp2net {
FARPROC CapcomProc(WORD ordinal);
}

using namespace lp2net;

namespace {
template <typename Fn>
Fn Capcom(WORD ordinal) {
	return (Fn)CapcomProc(ordinal);
}

std::string Utf8(LPCWSTR w) {
	if (!w) {
		return "(null)";
	}
	char buf[512];
	WideCharToMultiByte(CP_UTF8, 0, w, -1, buf, sizeof(buf), nullptr, nullptr);
	buf[sizeof(buf) - 1] = 0;
	return buf;
}

// A player's Steam profile from their XUID; recent players when they are not on Steam.
bool SteamProfile(const char* dialog, XUID xuid) {
	uint64_t id = SteamIdFromXuid(xuid);
	return id ? SteamOverlayToUser(dialog, id) : SteamOverlay("Players");
}
}  // namespace

extern "C" DWORD WINAPI XShowMessagesUI(DWORD dwUserIndex) {
	LOG("XShowMessagesUI(user=%lu)", dwUserIndex);
	if (SteamOverlay("Friends")) {
		return ERROR_SUCCESS;
	}
	auto fn = Capcom<DWORD(WINAPI*)(DWORD)>(5206);
	return fn ? fn(dwUserIndex) : ERROR_SUCCESS;
}

extern "C" DWORD WINAPI XShowGameInviteUI(DWORD dwUserIndex, const XUID* pXuidRecipients, DWORD cRecipients,
                                          LPCWSTR wszUnused) {
	LOG("XShowGameInviteUI(user=%lu, recipients=%lu)", dwUserIndex, cRecipients);
	if (SteamInviteDialog()) {
		return ERROR_SUCCESS;
	}
	OverlayToast(SteamReady() ? "Steam invites open once your game is listed on Steam (a few seconds after hosting)."
	                          : "Game invites need the game to be started from Steam.");
	auto fn = Capcom<DWORD(WINAPI*)(DWORD, const XUID*, DWORD, LPCWSTR)>(5208);
	return fn ? fn(dwUserIndex, pXuidRecipients, cRecipients, wszUnused) : ERROR_SUCCESS;
}

extern "C" DWORD WINAPI XShowMessageComposeUI(DWORD dwUserIndex, const XUID* pXuidRecipients, DWORD cRecipients,
                                              LPCWSTR wszText) {
	LOG("XShowMessageComposeUI(user=%lu, recipients=%lu, text='%s')", dwUserIndex, cRecipients,
	    Utf8(wszText).c_str());
	if (cRecipients && pXuidRecipients && SteamProfile("chat", pXuidRecipients[0])) {
		return ERROR_SUCCESS;
	}
	auto fn = Capcom<DWORD(WINAPI*)(DWORD, const XUID*, DWORD, LPCWSTR)>(5209);
	return fn ? fn(dwUserIndex, pXuidRecipients, cRecipients, wszText) : ERROR_SUCCESS;
}

extern "C" DWORD WINAPI XShowFriendRequestUI(DWORD dwUserIndex, XUID xuidUser) {
	LOG("XShowFriendRequestUI(user=%lu, xuid=%016llX)", dwUserIndex, xuidUser);
	if (SteamProfile("friendadd", xuidUser)) {
		return ERROR_SUCCESS;
	}
	auto fn = Capcom<DWORD(WINAPI*)(DWORD, XUID)>(5210);
	return fn ? fn(dwUserIndex, xuidUser) : ERROR_SUCCESS;
}

extern "C" DWORD WINAPI XShowPlayerReviewUI(DWORD dwUserIndex, XUID xuidTarget) {
	LOG("XShowPlayerReviewUI(user=%lu, xuid=%016llX)", dwUserIndex, xuidTarget);
	if (SteamProfile("steamid", xuidTarget)) {
		return ERROR_SUCCESS;
	}
	auto fn = Capcom<DWORD(WINAPI*)(DWORD, XUID)>(5214);
	return fn ? fn(dwUserIndex, xuidTarget) : ERROR_SUCCESS;
}

extern "C" DWORD WINAPI XShowKeyboardUI(DWORD dwUserIndex, DWORD dwFlags, LPCWSTR wszDefaultText, LPCWSTR wszTitleText,
                                        LPCWSTR wszDescriptionText, LPWSTR wszResultText, DWORD cchResultText,
                                        XOVERLAPPED* pOverlapped) {
	LOG("XShowKeyboardUI(user=%lu, flags=0x%lX, title='%s', text='%s')", dwUserIndex, dwFlags,
	    Utf8(wszTitleText).c_str(), Utf8(wszDescriptionText).c_str());
	auto fn = Capcom<DWORD(WINAPI*)(DWORD, DWORD, LPCWSTR, LPCWSTR, LPCWSTR, LPWSTR, DWORD, XOVERLAPPED*)>(5216);
	return fn ? fn(dwUserIndex, dwFlags, wszDefaultText, wszTitleText, wszDescriptionText, wszResultText,
	               cchResultText, pOverlapped)
	          : ERROR_FUNCTION_FAILED;
}

extern "C" DWORD WINAPI XShowAchievementsUI(DWORD dwUserIndex) {
	LOG("XShowAchievementsUI(user=%lu)", dwUserIndex);
	if (SteamOverlay("Achievements")) {
		return ERROR_SUCCESS;
	}
	auto fn = Capcom<DWORD(WINAPI*)(DWORD)>(5250);
	return fn ? fn(dwUserIndex) : ERROR_SUCCESS;
}

extern "C" DWORD WINAPI XShowGamerCardUI(DWORD dwUserIndex, XUID xuidPlayer) {
	LOG("XShowGamerCardUI(user=%lu, xuid=%016llX)", dwUserIndex, xuidPlayer);
	if (SteamProfile("steamid", xuidPlayer)) {
		return ERROR_SUCCESS;
	}
	auto fn = Capcom<DWORD(WINAPI*)(DWORD, XUID)>(5252);
	return fn ? fn(dwUserIndex, xuidPlayer) : ERROR_SUCCESS;
}

extern "C" DWORD WINAPI XShowMessageBoxUI(DWORD dwUserIndex, LPCWSTR wszTitle, LPCWSTR wszText, DWORD cButtons,
                                          LPCWSTR* pwszButtons, DWORD dwFocusButton, DWORD dwFlags, void* pResult,
                                          XOVERLAPPED* pOverlapped) {
	LOG("XShowMessageBoxUI(user=%lu, title='%s', text='%s', buttons=%lu)", dwUserIndex, Utf8(wszTitle).c_str(),
	    Utf8(wszText).c_str(), cButtons);
	auto fn = Capcom<DWORD(WINAPI*)(DWORD, LPCWSTR, LPCWSTR, DWORD, LPCWSTR*, DWORD, DWORD, void*, XOVERLAPPED*)>(
	    5266);
	return fn ? fn(dwUserIndex, wszTitle, wszText, cButtons, pwszButtons, dwFocusButton, dwFlags, pResult, pOverlapped)
	          : ERROR_FUNCTION_FAILED;
}

extern "C" DWORD WINAPI XShowFriendsUI(DWORD dwUserIndex) {
	LOG("XShowFriendsUI(user=%lu)", dwUserIndex);
	if (SteamOverlay("Friends")) {
		return ERROR_SUCCESS;
	}
	auto fn = Capcom<DWORD(WINAPI*)(DWORD)>(5275);
	return fn ? fn(dwUserIndex) : ERROR_SUCCESS;
}

extern "C" DWORD WINAPI XInviteSend(DWORD dwUserIndex, DWORD cInvitees, const XUID* pXuidInvitees,
                                    const WCHAR* pszText, XOVERLAPPED* pOverlapped) {
	LOG("XInviteSend(user=%lu, invitees=%lu)", dwUserIndex, cInvitees);
	if (SteamInviteDialog()) {
		return CompleteOverlapped(pOverlapped, ERROR_SUCCESS, 0);
	}
	auto fn = Capcom<DWORD(WINAPI*)(DWORD, DWORD, const XUID*, const WCHAR*, XOVERLAPPED*)>(5316);
	return fn ? fn(dwUserIndex, cInvitees, pXuidInvitees, pszText, pOverlapped) : ERROR_FUNCTION_FAILED;
}

extern "C" DWORD WINAPI XShowMarketplaceUI(DWORD dwUserIndex, DWORD dwEntryPoint, ULONGLONG qwOfferID,
                                           DWORD dwContentCategories) {
	LOG("XShowMarketplaceUI(user=%lu, entry=%lu)", dwUserIndex, dwEntryPoint);
	auto fn = Capcom<DWORD(WINAPI*)(DWORD, DWORD, ULONGLONG, DWORD)>(5365);
	return fn ? fn(dwUserIndex, dwEntryPoint, qwOfferID, dwContentCategories) : ERROR_SUCCESS;
}
