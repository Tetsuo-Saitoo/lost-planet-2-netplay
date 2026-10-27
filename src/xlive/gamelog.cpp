// Debug aid: route the game's own (release-build silent) debug log into lp2net.log.
//
// LP2's network code logs through a global system object: `mov ecx,[global]; mov edx,[ecx]; ...;
// call [edx+0x20|0x24|0x28]` with cdecl printf-style arguments (this, [level,] fmt, ...). We find the global by
// looking for the code that pushes a known format string, then wrap those three vtable slots. The address is found
// at runtime, so this works for both LP2DX9.exe and LP2DX11.exe.
#include "config.h"
#include "log.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace lp2net {
namespace {
using LogLevelFn = void(__cdecl*)(void* self, int level, const char* fmt, ...);
using LogFn = void(__cdecl*)(void* self, const char* fmt, ...);
void** g_global = nullptr;
bool g_installed = false;

void Emit(const char* tag, const char* fmt, va_list ap) {
	char buf[1024];
	vsnprintf(buf, sizeof(buf), fmt, ap);
	size_t n = strlen(buf);
	while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) {
		buf[--n] = 0;
	}
	Log("game%s %s", tag, buf);
}

// Only network classes are interesting and the rest is noisy, so filter on the "Class::method" prefix.
bool Wanted(const char* fmt, va_list ap) {
	if (strncmp(fmt, "%s:", 3) != 0) {
		return true;
	}
	va_list copy;
	va_copy(copy, ap);
	const char* func = va_arg(copy, const char*);
	va_end(copy);
	return !func || IsBadStringPtrA(func, 128) || strstr(func, "Network") || strstr(func, "Xbox") ||
	       strstr(func, "Session") || strstr(func, "Lobby") || strstr(func, "Net");
}

void __cdecl HookLevel(void* self, int level, const char* fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	if (fmt && Wanted(fmt, ap)) {
		char tag[8];
		snprintf(tag, sizeof(tag), "[%d]", level);
		Emit(tag, fmt, ap);
	}
	va_end(ap);
}

void __cdecl HookA(void* self, const char* fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	if (fmt && Wanted(fmt, ap)) {
		Emit("", fmt, ap);
	}
	va_end(ap);
}

void __cdecl HookB(void* self, const char* fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	if (fmt && Wanted(fmt, ap)) {
		Emit("!", fmt, ap);
	}
	va_end(ap);
}

bool FindSection(const char* name, BYTE** start, size_t* size) {
	auto* base = (BYTE*)GetModuleHandleW(nullptr);
	auto* nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
	auto* sec = IMAGE_FIRST_SECTION(nt);
	for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
		if (strncmp((const char*)sec->Name, name, 8) == 0) {
			*start = base + sec->VirtualAddress;
			*size = sec->Misc.VirtualSize;
			return true;
		}
	}
	return false;
}

void** LocateGlobal() {
	// the game's string continues with "\n"; match the prefix only
	static const char kAnchor[] = "%s: XNetSetSystemLinkPort() failed Return=%d";
	constexpr size_t kAnchorLen = sizeof(kAnchor) - 1;
	BYTE *rdata, *text;
	size_t rsize, tsize;
	if (!FindSection(".rdata", &rdata, &rsize) || !FindSection(".text", &text, &tsize)) {
		return nullptr;
	}
	BYTE* str = nullptr;
	for (size_t i = 0; i + kAnchorLen <= rsize; ++i) {
		if (memcmp(rdata + i, kAnchor, kAnchorLen) == 0) {
			str = rdata + i;
			break;
		}
	}
	if (!str) {
		return nullptr;
	}
	for (size_t i = 0; i + 5 <= tsize; ++i) {
		if (text[i] != 0x68 || *(BYTE**)(text + i + 1) != str) {
			continue;
		}
		// walk back to the `mov reg, [global]` (8B 05/0D/15/... or A1) that loaded the object
		for (size_t back = 6; back <= 32 && back <= i; ++back) {
			BYTE* p = text + i - back;
			if (p[0] == 0x8B && (p[1] & 0xC7) == 0x05) {
				return *(void***)(p + 2);
			}
			if (p[0] == 0xA1) {
				return *(void***)(p + 1);
			}
		}
	}
	return nullptr;
}

// Replace a slot only if the original does nothing (release-build stub), so behaviour cannot change.
void Patch(void** slot, void* fn) {
	const BYTE* orig = (const BYTE*)*slot;
	bool trivial = orig[0] == 0xC3 || (orig[0] == 0x33 && orig[1] == 0xC0 && orig[2] == 0xC3);
	LOG("gamelog: slot %p original %p (%02X %02X %02X) %s", slot, orig, orig[0], orig[1], orig[2],
	    trivial ? "hooked" : "left alone");
	if (!trivial) {
		return;
	}
	DWORD old;
	VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old);
	*slot = fn;
	VirtualProtect(slot, sizeof(void*), old, &old);
}
}  // namespace

// Call until it returns true: the system object may not exist yet when xlive is first initialised.
bool GameLogTryInstall() {
	if (g_installed || !Cfg().gameLog) {
		return true;
	}
	if (!g_global) {
		g_global = LocateGlobal();
		LOG("gamelog: system object global at %p", g_global);
		if (!g_global) {
			g_installed = true;  // nothing to hook in this build; stop trying
			return true;
		}
	}
	void* obj = *g_global;
	if (!obj) {
		return false;
	}
	void** vtbl = *(void***)obj;
	Patch(&vtbl[0x20 / 4], (void*)&HookLevel);
	Patch(&vtbl[0x24 / 4], (void*)&HookA);
	Patch(&vtbl[0x28 / 4], (void*)&HookB);
	g_installed = true;
	LOG("gamelog: hooked object %p vtable %p", obj, vtbl);
	return true;
}

}  // namespace lp2net
