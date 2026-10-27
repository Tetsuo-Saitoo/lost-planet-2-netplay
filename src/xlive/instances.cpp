// Input gating, done by patching the game's import table (which the SteamStub wrapper leaves alone).
// - While the overlay is open the game gets no keyboard, mouse or controller input, so typing an address or moving
//   through the overlay does not also drive the game.
// - Optional: several copies of the game on one PC ([Debug] AllowMultipleCopies=1), for testing System Link
//   without a second machine. LP2 refuses to start twice: WinMain calls CreateMutexA(NULL, TRUE, "sPlanetMain")
//   and quits on ERROR_ALREADY_EXISTS, so we report that mutex as new. LP2 reads the keyboard with GetAsyncKeyState
//   and DirectInput in background mode, so every copy reacts to every key: copies whose window is not in the
//   foreground get no keyboard/mouse input.
#define DIRECTINPUT_VERSION 0x0800
#include "config.h"
#include "log.h"
#include "overlay.h"

#include <dinput.h>
#include <windows.h>
#include <xinput.h>

#include <cstring>

namespace lp2net {
void EnsureInit();

namespace {
using CreateMutexA_t = HANDLE(WINAPI*)(LPSECURITY_ATTRIBUTES, BOOL, LPCSTR);
CreateMutexA_t g_realCreateMutexA = nullptr;

HANDLE WINAPI HookCreateMutexA(LPSECURITY_ATTRIBUTES sa, BOOL initialOwner, LPCSTR name) {
	HANDLE h = g_realCreateMutexA(sa, initialOwner, name);
	DWORD err = GetLastError();
	if (h && err == ERROR_ALREADY_EXISTS && initialOwner && name) {
		EnsureInit();  // runs from the game's WinMain, outside the loader lock
		if (Cfg().allowMultipleCopies) {
			LOG("instances: mutex '%s' exists, starting another copy (AllowMultipleCopies=1)", name);
			SetLastError(ERROR_SUCCESS);
			return h;
		}
	}
	SetLastError(err);
	return h;
}

bool MultiCopy() {
	EnsureInit();
	return Cfg().allowMultipleCopies;
}

bool InForeground() {
	DWORD pid = 0;
	GetWindowThreadProcessId(GetForegroundWindow(), &pid);
	return pid == GetCurrentProcessId();
}

// keyboard and mouse
bool BlockKeys() {
	return OverlayVisible() || (MultiCopy() && !InForeground());
}

using GetAsyncKeyState_t = SHORT(WINAPI*)(int);
GetAsyncKeyState_t g_realGetAsyncKeyState = nullptr;

SHORT WINAPI HookGetAsyncKeyState(int vk) {
	if (BlockKeys()) {
		return 0;
	}
	return g_realGetAsyncKeyState(vk);
}

// The game delay-loads XINPUT1_3.dll. Its delay-import slot starts out pointing at a resolver thunk that would
// overwrite the slot, so the real function is looked up here instead.
using XInputGetState_t = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
XInputGetState_t g_realXInputGetState = nullptr;

DWORD WINAPI HookXInputGetState(DWORD pad, XINPUT_STATE* state) {
	if (!g_realXInputGetState) {
		HMODULE m = LoadLibraryW(L"xinput1_3.dll");
		if (!m) {
			m = LoadLibraryW(L"xinput1_4.dll");  // DirectX 2010 runtime missing: Windows' own XInput still works
		}
		g_realXInputGetState = m ? (XInputGetState_t)GetProcAddress(m, "XInputGetState") : nullptr;
		if (!g_realXInputGetState) {
			return ERROR_DEVICE_NOT_CONNECTED;
		}
	}
	DWORD r = g_realXInputGetState(pad, state);
	if (r == ERROR_SUCCESS && state && OverlayVisible()) {
		memset(&state->Gamepad, 0, sizeof(state->Gamepad));
	}
	return r;
}

// IDirectInputDevice8A vtable: 9 = GetDeviceState, 10 = GetDeviceData. Keyboard (256-byte state) and mouse
// (DIMOUSESTATE/2) are blanked in the background; gamepads pass through.
using GetDeviceState_t = HRESULT(WINAPI*)(IDirectInputDevice8A*, DWORD, LPVOID);
using GetDeviceData_t = HRESULT(WINAPI*)(IDirectInputDevice8A*, DWORD, LPDIDEVICEOBJECTDATA, LPDWORD, DWORD);
using CreateDevice_t = HRESULT(WINAPI*)(IDirectInput8A*, REFGUID, LPDIRECTINPUTDEVICE8A*, LPUNKNOWN);
using DirectInput8Create_t = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
GetDeviceState_t g_realGetDeviceState = nullptr;
GetDeviceData_t g_realGetDeviceData = nullptr;
CreateDevice_t g_realCreateDevice = nullptr;
DirectInput8Create_t g_realDirectInput8Create = nullptr;

HRESULT WINAPI HookGetDeviceState(IDirectInputDevice8A* dev, DWORD cb, LPVOID data) {
	HRESULT hr = g_realGetDeviceState(dev, cb, data);
	bool kbOrMouse = cb == 256 || cb == sizeof(DIMOUSESTATE) || cb == sizeof(DIMOUSESTATE2);
	if (SUCCEEDED(hr) && kbOrMouse && BlockKeys()) {
		memset(data, 0, cb);
	}
	return hr;
}

HRESULT WINAPI HookGetDeviceData(IDirectInputDevice8A* dev, DWORD cbObj, LPDIDEVICEOBJECTDATA rgdod, LPDWORD inOut,
                                 DWORD flags) {
	HRESULT hr = g_realGetDeviceData(dev, cbObj, rgdod, inOut, flags);
	if (SUCCEEDED(hr) && inOut && BlockKeys()) {
		*inOut = 0;  // drop buffered key/mouse events meant for the overlay or the foreground copy
	}
	return hr;
}

void PatchSlot(void** slot, void* hook, void** real) {
	if (*slot == hook) {
		return;
	}
	DWORD old;
	if (VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) {
		*real = *slot;
		*slot = hook;
		VirtualProtect(slot, sizeof(void*), old, &old);
	}
}

HRESULT WINAPI HookCreateDevice(IDirectInput8A* di, REFGUID guid, LPDIRECTINPUTDEVICE8A* out, LPUNKNOWN outer) {
	HRESULT hr = g_realCreateDevice(di, guid, out, outer);
	if (SUCCEEDED(hr) && out && *out) {
		void** vtbl = *(void***)*out;  // shared by every DirectInput device of this class
		PatchSlot(&vtbl[9], (void*)&HookGetDeviceState, (void**)&g_realGetDeviceState);
		PatchSlot(&vtbl[10], (void*)&HookGetDeviceData, (void**)&g_realGetDeviceData);
	}
	return hr;
}

HRESULT WINAPI HookDirectInput8Create(HINSTANCE inst, DWORD version, REFIID iid, LPVOID* out, LPUNKNOWN outer) {
	HRESULT hr = g_realDirectInput8Create(inst, version, iid, out, outer);
	if (SUCCEEDED(hr) && out && *out) {
		void** vtbl = *(void***)*out;
		PatchSlot(&vtbl[3], (void*)&HookCreateDevice, (void**)&g_realCreateDevice);  // IDirectInput8::CreateDevice
	}
	return hr;
}

void** FindImport(HMODULE mod, const char* dll, const char* func) {
	auto* base = (BYTE*)mod;
	auto* nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
	auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
	for (auto* d = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress); d->Name; ++d) {
		if (_stricmp((const char*)(base + d->Name), dll) != 0 || !d->OriginalFirstThunk) {
			continue;
		}
		auto* names = (IMAGE_THUNK_DATA*)(base + d->OriginalFirstThunk);
		auto* slots = (IMAGE_THUNK_DATA*)(base + d->FirstThunk);
		for (; names->u1.AddressOfData; ++names, ++slots) {
			if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) {
				continue;
			}
			auto* ibn = (IMAGE_IMPORT_BY_NAME*)(base + names->u1.AddressOfData);
			if (strcmp((const char*)ibn->Name, func) == 0) {
				return (void**)&slots->u1.Function;
			}
		}
	}
	return nullptr;
}

void** FindDelayImport(HMODULE mod, const char* dll, const char* func) {
	auto* base = (BYTE*)mod;
	auto* nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
	auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT];
	if (!dir.VirtualAddress) {
		return nullptr;
	}
	for (auto* d = (IMAGE_DELAYLOAD_DESCRIPTOR*)(base + dir.VirtualAddress); d->DllNameRVA; ++d) {
		// old (VC6) descriptors hold addresses instead of RVAs
		DWORD bias = (d->Attributes.RvaBased) ? 0 : nt->OptionalHeader.ImageBase;
		if (_stricmp((const char*)(base + d->DllNameRVA - bias), dll) != 0) {
			continue;
		}
		auto* names = (IMAGE_THUNK_DATA*)(base + d->ImportNameTableRVA - bias);
		auto* slots = (IMAGE_THUNK_DATA*)(base + d->ImportAddressTableRVA - bias);
		for (; names->u1.AddressOfData; ++names, ++slots) {
			if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) {
				continue;
			}
			auto* ibn = (IMAGE_IMPORT_BY_NAME*)(base + names->u1.AddressOfData - bias);
			if (strcmp((const char*)ibn->Name, func) == 0) {
				return (void**)&slots->u1.Function;
			}
		}
	}
	return nullptr;
}
}  // namespace

// Called from DllMain: only patches pointers, no file I/O. The hooks check the option when they run.
void InstallInstanceHook() {
	HMODULE game = GetModuleHandleW(nullptr);
	if (void** s = FindImport(game, "KERNEL32.dll", "CreateMutexA")) {
		PatchSlot(s, (void*)&HookCreateMutexA, (void**)&g_realCreateMutexA);
	}
	if (void** s = FindImport(game, "USER32.dll", "GetAsyncKeyState")) {
		PatchSlot(s, (void*)&HookGetAsyncKeyState, (void**)&g_realGetAsyncKeyState);
	}
	if (void** s = FindImport(game, "DINPUT8.dll", "DirectInput8Create")) {
		PatchSlot(s, (void*)&HookDirectInput8Create, (void**)&g_realDirectInput8Create);
	}
	if (void** s = FindDelayImport(game, "XINPUT1_3.dll", "XInputGetState")) {
		void* resolverThunk = nullptr;
		PatchSlot(s, (void*)&HookXInputGetState, &resolverThunk);
	}
}

}  // namespace lp2net
