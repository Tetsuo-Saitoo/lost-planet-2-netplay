// LP2Net xlive.dll: entry point and the XLive lifecycle calls (init, per-frame render, message hook).
#include "config.h"
#include "gfwl.h"
#include "log.h"
#include "lp2net_version.h"
#include "net.h"
#include "overlay.h"
#include "xnotify.h"

#include <mutex>

using namespace lp2net;

namespace lp2net {
namespace {
std::once_flag g_initOnce;
HMODULE g_capcom = nullptr;
}  // namespace

// Config + log, on the first call from the game (not in DllMain: no file I/O under the loader lock).
void EnsureInit() {
	std::call_once(g_initOnce, [] {
		LoadConfig();
		if (Cfg().log) {
			LogOpen((GameDir() + L"lp2net.log").c_str());
		}
		LOG("LP2Net " LP2NET_VERSION " (" __DATE__ " " __TIME__ ") in %ls", GameDir().c_str());
		g_capcom = GetModuleHandleW(L"xlive_capcom.dll");
		if (!g_capcom) {
			g_capcom = LoadLibraryW((GameDir() + L"xlive_capcom.dll").c_str());
		}
		LOG("Capcom stub: %p", g_capcom);
	});
}

bool GameLogTryInstall();
void CarrySaveToGamertag();
void WatchdogStart();
void WatchdogStop();

FARPROC CapcomProc(WORD ordinal) {
	EnsureInit();
	return g_capcom ? GetProcAddress(g_capcom, MAKEINTRESOURCEA(ordinal)) : nullptr;
}
}  // namespace lp2net

namespace lp2net {
void InstallInstanceHook();
}

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, void*) {
	if (reason == DLL_PROCESS_ATTACH) {
		DisableThreadLibraryCalls(hinst);
		InstallInstanceHook();
	}
	return TRUE;
}

extern "C" HRESULT WINAPI XLiveInitializeEx(XLIVE_INITIALIZE_INFO* pInfo, DWORD dwTitleXLiveVersion) {
	EnsureInit();
	LOG("XLiveInitializeEx(info=%p flags=0x%lX d3d=%p pp=%p lang=%u, version=0x%08lX)", pInfo,
	    pInfo ? pInfo->dwFlags : 0, pInfo ? pInfo->pD3D : nullptr, pInfo ? pInfo->pD3DPP : nullptr,
	    pInfo ? pInfo->langID : 0, dwTitleXLiveVersion);
	// Capcom's stub keeps the profile/save state the forwarded calls use; let it initialise too.
	if (auto capcomInit = (HRESULT(WINAPI*)(XLIVE_INITIALIZE_INFO*, DWORD))CapcomProc(5297)) {
		capcomInit(pInfo, dwTitleXLiveVersion);
	}
	NetStartup();
	if (pInfo && pInfo->pD3D) {
		CarrySaveToGamertag();  // only the game itself (not the transport test) has saves
	}
	OverlayInit(pInfo);
	GameLogTryInstall();
	if (pInfo && pInfo->pD3D) {
		WatchdogStart();
	}
	PostNotification(XN_SYS_SIGNINCHANGED, 1);
	return S_OK;
}

extern "C" HRESULT WINAPI XLiveInitialize(XLIVE_INITIALIZE_INFO* pInfo) {
	return XLiveInitializeEx(pInfo, 0);
}

extern "C" void WINAPI XLiveUninitialize() {
	LOG("XLiveUninitialize");
	WatchdogStop();
	OverlayShutdown();
	NetShutdown();
}

extern "C" HRESULT WINAPI XLiveRender() {
	static int tries = 0;
	if (tries < 600 && !GameLogTryInstall()) {
		++tries;  // the game's system object may appear a few frames after xlive starts
	}
	OverlayRender();
	return S_OK;
}

struct XLIVE_INPUT_INFO {
	UINT cbSize;
	HWND hWnd;
	UINT uMSG;
	WPARAM wParam;
	LPARAM lParam;
	BOOL fHandled;
	LRESULT lRet;
};

extern "C" HRESULT WINAPI XLiveInput(XLIVE_INPUT_INFO* pii) {
	if (!pii) {
		return E_POINTER;
	}
	MSG msg{pii->hWnd, pii->uMSG, pii->wParam, pii->lParam};
	pii->fHandled = OverlayHandleMessage(&msg);
	pii->lRet = 0;
	return S_OK;
}

extern "C" BOOL WINAPI XLivePreTranslateMessage(const MSG* lpMsg) {
	return OverlayHandleMessage(lpMsg);
}

extern "C" HRESULT WINAPI XLiveOnResetDevice(void* pD3DPP) {
	return S_OK;  // the overlay wraps IDirect3DDevice9::Reset itself
}

extern "C" HRESULT WINAPI XLiveOnDestroyDevice() {
	OverlayShutdown();
	return S_OK;
}
