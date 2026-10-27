// Ordinals handed to Capcom's stub (installed as xlive_capcom.dll): profile/save data, achievements, protected data,
// PBuffers, content and marketplace stubs. Each export is a small thunk that resolves the stub's function on first
// use, logs the first calls (so the log shows which of these the game reaches) and jumps to it with the stack and
// registers untouched. (.def forwarders do not link with this toolset.)
#include "log.h"

#include <windows.h>

namespace lp2net {
FARPROC CapcomProc(WORD ordinal);
}

#define LP2_FORWARDS(X)                            \
	X(XNetInAddrToString, 62)                      \
	X(XLivePBufferAllocate, 5016)                  \
	X(XLivePBufferFree, 5017)                      \
	X(XLivePBufferGetByte, 5018)                   \
	X(XLivePBufferSetByte, 5019)                   \
	X(XLivePBufferGetDWORD, 5020)                  \
	X(XLivePBufferSetDWORD, 5021)                  \
	X(XLiveGetUpdateInformation, 5022)             \
	X(XLiveUpdateSystem, 5024)                     \
	X(XLiveSetDebugLevel, 5031)                    \
	X(XLiveProtectData, 5034)                      \
	X(XLiveUnprotectData, 5035)                    \
	X(XLiveCreateProtectedDataContext, 5036)       \
	X(XLiveQueryProtectedDataInformation, 5037)    \
	X(XLiveCloseProtectedDataContext, 5038)        \
	X(XLocatorServerAdvertise, 5230)               \
	X(XLocatorServerUnAdvertise, 5231)             \
	X(XLocatorGetServiceProperty, 5233)            \
	X(XLocatorCreateServerEnumerator, 5234)        \
	X(XLocatorCreateServerEnumeratorByIDs, 5235)   \
	X(XLocatorServiceInitialize, 5236)             \
	X(XLocatorServiceUnInitialize, 5237)           \
	X(XLocatorCreateKey, 5238)                     \
	X(XUserWriteAchievements, 5278)                \
	X(XUserCreateAchievementEnumerator, 5280)      \
	X(XUserCreateStatsEnumeratorByRank, 5284)      \
	X(XUserCreateStatsEnumeratorByXuid, 5286)      \
	X(XLivePBufferGetByteArray, 5294)              \
	X(XLivePBufferSetByteArray, 5295)              \
	X(XStringVerify, 5303)                         \
	X(XUserReadProfileSettings, 5331)              \
	X(XUserWriteProfileSettings, 5337)             \
	X(XUserReadProfileSettingsByXuid, 5339)        \
	X(XUserEstimateRankForRating, 5346)            \
	X(XLiveProtectedCreateFile, 5348)              \
	X(XLiveProtectedVerifyFile, 5349)              \
	X(XLiveContentCreateAccessHandle, 5350)        \
	X(XLiveContentGetPath, 5355)                   \
	X(XLiveContentCreateEnumerator, 5360)          \
	X(XContentGetMarketplaceCounts, 5367)          \
	X(XMarketplaceCreateOfferEnumerator, 5372)     \
	X(XUserFindUsers, 5376)                        \
	X(XMarketplaceCreateOfferEnumeratorByOffering, 5377)

namespace {
constexpr int kMaxOrdinal = 5400;
}

// Indexed by ordinal; filled on first call.
extern "C" FARPROC g_capcomFn[kMaxOrdinal] = {};
static LONG g_calls[kMaxOrdinal] = {};

extern "C" void __cdecl ResolveForward(const char* name, int ordinal) {
	if (!g_capcomFn[ordinal]) {
		g_capcomFn[ordinal] = lp2net::CapcomProc((WORD)ordinal);
		if (!g_capcomFn[ordinal]) {
			LOG("FATAL: xlive_capcom.dll has no ordinal %d (%s)", ordinal, name);
		}
	}
	LONG n = InterlockedIncrement(&g_calls[ordinal]);
	if (n <= 3 || n % 1000 == 0) {
		LOG("-> Capcom %s (#%d), call %ld", name, ordinal, n);
	}
}

#define LP2_THUNK(name, ord)                                       \
	static const char kName_##name[] = #name;                      \
	extern "C" __declspec(naked) void Fwd_##name() {               \
		__asm pushad                                               \
		__asm push ord                                             \
		__asm push offset kName_##name                             \
		__asm call ResolveForward                                  \
		__asm add esp, 8                                           \
		__asm popad                                                \
		__asm jmp dword ptr [g_capcomFn + ord * 4]                 \
	}                                                              \
	__pragma(comment(linker, "/export:" #name "=_Fwd_" #name ",@" #ord ",NONAME"))

LP2_FORWARDS(LP2_THUNK)
