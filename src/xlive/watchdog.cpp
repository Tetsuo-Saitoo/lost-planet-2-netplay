// The game's frozen-game check. sPlanetNetwork's per-frame move compares the game object's frame counter with the
// value it saw on its previous tick; while the player is in an online session, 1800 network ticks in a row without
// a new game frame make the game leave the match by itself, with no message and no log line (to the others: "the
// player left after a few minutes"). Both exes have the check once; we find it by its code bytes and take the
// fields it uses from the instructions.
//
// We log when that count climbs, with the game's state around it, and with [Network] FreezeGuard=1 (the default)
// raise its limit so the game never leaves for this reason. A player whose game really stops is still dropped by
// the other players' own 20 s receive timeout.
//
// The check is one of many ways out. The leave it calls is the session's general leave (vtable slot 30, DX9
// 0x84EAE0: the PDA's Quit Game and the host reacting to a player who left use it too), and leave(reason) (DX9
// 0x84A420) is called from 21 places. Both are hooked at their entry to log who started the leave.
#include "config.h"
#include "log.h"
#include "overlay.h"
#include "util.h"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <thread>

namespace lp2net {
namespace {
// sPlanetNetwork::move from the frame check to the leave call (DX9 0x84F890, DX11 0x85ECE0):
//   mov eax,[game]; test byte [eax+A],1; push esi; mov esi,ecx; jz; test dword [eax+B],0x100000; jnz
//   cmp dword [esi+session],-1; jz; cmp byte [esi+leaving],0; jnz; mov eax,[eax+frame]; cmp eax,[esi+last]; jnz
//   inc dword [esi+count]; cmp dword [esi+count],0x708; jb; mov eax,[esi]; mov edx,[eax+0x78]; push 0; call edx
constexpr const char kCheck[] =
	"A1 ?? ?? ?? ?? F6 80 ?? ?? ?? ?? 01 56 8B F1 74 ?? F7 80 ?? ?? ?? ?? 00 00 10 00 75 ?? 83 BE ?? ?? ?? ?? FF "
	"74 ?? 80 BE ?? ?? ?? ?? 00 75 ?? 8B 80 ?? ?? ?? ?? 3B 86 ?? ?? ?? ?? 75 ?? FF 86 ?? ?? ?? ?? 81 BE ?? ?? ?? ?? "
	"08 07 00 00 72 ?? 8B 06 8B 50 78 6A 00 FF D2";
// The leave it calls (vtable slot 0x78, DX9 0x84EAE0), which loads the sPlanetNetwork instance from its global:
//   push esi; mov esi,ecx; cmp byte [esi+leaving],0; jnz; cmp dword [esi+session],-1; jnz; mov ecx,[net]; ...
constexpr const char kLeave[] =
	"56 8B F1 80 BE ?? ?? ?? ?? 00 75 ?? 83 BE ?? ?? ?? ?? FF 75 ?? 8B 0D ?? ?? ?? ?? 80 B9 ?? ?? ?? ?? 00 74 ?? "
	"6A 00 E8";
// The game's leave function, leave(reason): push esi; mov esi,ecx; cmp byte [esi+leaving],0; jz; xor al,al; pop esi;
// ret 4; ... mov ecx,[esp+8]; mov byte [esi+leaving],1. A second function has the same shape on another flag.
constexpr const char kLeaveStart[] =
	"56 8B F1 80 BE ?? ?? ?? ?? 00 74 06 32 C0 5E C2 04 00 80 7E ?? 00 75 ?? 80 3D ?? ?? ?? ?? 00 74 ?? 8D 46 04 50 "
	"FF 15 ?? ?? ?? ?? 80 7E ?? 00 8B 4C 24 08 C6 86 ?? ?? ?? ?? 01";
constexpr size_t kEntryBytes = 10;  // push esi; mov esi,ecx; cmp byte [esi+imm32],0: moved to the trampoline as is
constexpr DWORD kGameLimit = 0x708;
constexpr DWORD kLogFrom = 600;  // a third of the game's limit
constexpr DWORD kPollMs = 50;

struct Fields {
	void** game = nullptr;  // global holding the game object (frame counter, state flags)
	void** net = nullptr;   // global holding sPlanetNetwork (session, leaving, the check's count)
	DWORD flagA = 0, flagB = 0, frame = 0;  // offsets in the game object
	DWORD session = 0, leaving = 0, count = 0;  // offsets in sPlanetNetwork
	DWORD* limit = nullptr;  // the 0x708 in the compare
	BYTE* leave = nullptr;         // leave(reason)
	BYTE* sessionLeave = nullptr;  // the session's leave (vtable slot 30), which the check calls
};

struct Snap {
	DWORD frame = 0, count = 0, flagA = 0, flagB = 0;
	int session = -1;
	BYTE leaving = 0;
};

std::thread g_thread;
std::atomic<bool> g_stop{false};
BYTE* g_text = nullptr;  // the exe's code, to pick return addresses off the stack
size_t g_textSize = 0;
void* g_resumeLeave = nullptr;  // trampolines: the moved entry bytes, then a jump back
void* g_resumeSessionLeave = nullptr;
DWORD g_leaving = 0;      // sPlanetNetwork's "leaving" flag offset
DWORD g_checkCall = 0;    // return address of the check's call to the session's leave

int Hex(char c) {
	return c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10;
}

BYTE* Find(BYTE* start, size_t size, const char* pattern) {
	BYTE bytes[128];
	bool any[128];
	size_t n = 0;
	for (const char* p = pattern; *p && n < sizeof(bytes);) {
		if (*p == ' ') {
			++p;
			continue;
		}
		any[n] = p[0] == '?';
		bytes[n] = any[n] ? 0 : (BYTE)(Hex(p[0]) << 4 | Hex(p[1]));
		++n;
		p += 2;
	}
	for (size_t i = 0; i + n <= size; ++i) {
		size_t k = 0;
		while (k < n && (any[k] || start[i + k] == bytes[k])) {
			++k;
		}
		if (k == n) {
			return start + i;
		}
	}
	return nullptr;
}

DWORD U32(const BYTE* p) {
	DWORD v;
	memcpy(&v, p, 4);
	return v;
}

bool Locate(Fields* f) {
	BYTE* text;
	size_t size;
	if (!FindSection(".text", &text, &size)) {
		return false;
	}
	g_text = text;
	g_textSize = size;
	BYTE* c = Find(text, size, kCheck);
	BYTE* l = Find(text, size, kLeave);
	if (!c || !l) {
		return false;
	}
	for (BYTE* s = Find(text, size, kLeaveStart); s && !f->leave; s = Find(s + 1, text + size - (s + 1), kLeaveStart)) {
		if (U32(s + 5) == U32(c + 0x28)) {
			f->leave = s;
		}
	}
	f->sessionLeave = l;
	f->game = (void**)U32(c + 0x01);
	f->flagA = U32(c + 0x07);
	f->flagB = U32(c + 0x13);
	f->session = U32(c + 0x1F);
	f->leaving = U32(c + 0x28);
	f->frame = U32(c + 0x31);
	f->count = U32(c + 0x3F);
	f->limit = (DWORD*)(c + 0x49);
	f->net = (void**)U32(l + 0x17);
	return U32(l + 0x05) == f->leaving && U32(l + 0x0E) == f->session;  // the same object, by its fields
}

bool Read(const void* p, void* out, size_t n) {
	__try {
		memcpy(out, p, n);
		return true;
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}

bool Take(const Fields& f, Snap* s) {
	BYTE* game = nullptr;
	BYTE* net = nullptr;
	if (!Read(f.game, &game, 4) || !Read(f.net, &net, 4) || !game || !net) {
		return false;
	}
	return Read(game + f.frame, &s->frame, 4) && Read(game + f.flagA, &s->flagA, 4) &&
	       Read(game + f.flagB, &s->flagB, 4) && Read(net + f.session, &s->session, 4) &&
	       Read(net + f.leaving, &s->leaving, 1) && Read(net + f.count, &s->count, 4);
}

bool GameInFront() {
	DWORD pid = 0;
	GetWindowThreadProcessId(GetForegroundWindow(), &pid);
	return pid == GetCurrentProcessId();
}

void Watch(Fields f) {
	Snap last;
	bool haveLast = false;
	ULONGLONG frameAt = 0;    // when the frame counter last moved
	ULONGLONG stallFrom = 0;  // start of the stall being logged
	bool stalled = false, overLimit = false;
	DWORD peak = 0;
	ULONGLONG minuteFrom = 0;  // once a minute in a session: game frames and the highest count seen
	DWORD minuteFrame = 0, minuteMax = 0;
	while (!g_stop) {
		Sleep(kPollMs);
		Snap s;
		if (!Take(f, &s)) {
			haveLast = false;
			continue;
		}
		ULONGLONG now = GetTickCount64();
		if (!haveLast || s.frame != last.frame) {
			frameAt = now;
		}
		bool inSession = s.session != -1;
		if (!inSession || s.frame < minuteFrame) {
			minuteFrom = 0;
		} else if (!minuteFrom) {
			minuteFrom = now;
			minuteFrame = s.frame;
			minuteMax = 0;
		} else {
			if (s.count > minuteMax) {
				minuteMax = s.count;
			}
			if (now - minuteFrom >= 60000) {
				LOG("freeze check: %lu game frames in the last minute, the highest count seen %lu",
				    s.frame - minuteFrame, minuteMax);
				minuteFrom = 0;
			}
		}
		if (!stalled && inSession && s.count >= kLogFrom) {
			stalled = true;
			overLimit = false;
			stallFrom = frameAt;
			peak = s.count;
			LOG("freeze check: no new game frame for %llu ms, the game's count is %lu of %lu; game window in front: "
			    "%s, overlay: %s, game flags %08lX / %08lX",
			    now - frameAt, s.count, kGameLimit, GameInFront() ? "yes" : "no", OverlayVisible() ? "open" : "closed",
			    s.flagA, s.flagB);
		}
		if (stalled) {
			if (s.count > peak) {
				peak = s.count;
			}
			if (!overLimit && s.count >= kGameLimit) {
				overLimit = true;
				LOG(Cfg().freezeGuard
				        ? "freeze check: the game would leave the match now; FreezeGuard keeps the player in"
				        : "freeze check: the count reached the game's limit, so the game leaves the match");
			}
			if (s.count < last.count || !inSession) {
				ULONGLONG ms = now - stallFrom;
				LOG("freeze check: %s after %llu ms; the count peaked at %lu (about %llu network ticks a second)",
				    inSession ? "game frames again" : "session over", ms, peak, ms ? peak * 1000ull / ms : 0);
				stalled = false;
			}
		}
		if (haveLast && !last.leaving && s.leaving) {
			LOG("freeze check: the game is leaving the session (the check's count is %lu)", s.count);
		}
		last = s;
		haveLast = true;
	}
}

// At the entry of a leave, on the thread that calls it: stack[0] is the caller's return address, stack[1] the
// argument, and the hook's PUSHAD put the object (ECX) at stack[-2].
void __cdecl OnLeave(int session, const DWORD* stack) {
	BYTE leaving = 0;
	if (Read((const BYTE*)stack[-2] + g_leaving, &leaving, 1) && leaving) {
		return;  // already leaving: the call does nothing
	}
	char trail[64] = "";
	size_t n = 0;
	for (int i = 2, found = 0; i < 48 && found < 4; ++i) {
		DWORD v;
		if (!Read(stack + i, &v, 4)) {
			break;
		}
		if (v - (DWORD)g_text < g_textSize) {
			n += snprintf(trail + n, sizeof(trail) - n, " %08lX", v);
			++found;
		}
	}
	LOG("session leave: %s(%lu) called from %08lX%s; code addresses further up the stack:%s",
	    session ? "the session's leave" : "leave", stack[1], stack[0],
	    stack[0] == g_checkCall ? " (the freeze check: no new game frame for its limit)" : "", trail);
}

__declspec(naked) void LeaveHook() {
	__asm {
		pushad
		lea eax, [esp + 32]
		push eax
		push 0
		call OnLeave
		add esp, 8
		popad
		jmp dword ptr [g_resumeLeave]
	}
}

__declspec(naked) void SessionLeaveHook() {
	__asm {
		pushad
		lea eax, [esp + 32]
		push eax
		push 1
		call OnLeave
		add esp, 8
		popad
		jmp dword ptr [g_resumeSessionLeave]
	}
}

// Both leaves start with push esi; mov esi,ecx; cmp byte [esi+leaving],0 (10 bytes, nothing relative): they move to
// a trampoline that jumps back, and the entry becomes a jump to our hook.
bool HookEntry(BYTE* fn, DWORD leaving, void* hook, void** resume) {
	static const BYTE kHead[] = {0x56, 0x8B, 0xF1, 0x80, 0xBE};
	if (!fn || memcmp(fn, kHead, sizeof(kHead)) || U32(fn + 5) != leaving || fn[9] != 0) {
		return false;
	}
	auto* t = (BYTE*)VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
	if (!t) {
		return false;
	}
	memcpy(t, fn, kEntryBytes);
	t[kEntryBytes] = 0xE9;
	DWORD rel = (DWORD)(fn + kEntryBytes - (t + kEntryBytes + 5));
	memcpy(t + kEntryBytes + 1, &rel, 4);
	*resume = t;
	DWORD old;
	if (!VirtualProtect(fn, kEntryBytes, PAGE_EXECUTE_READWRITE, &old)) {
		return false;
	}
	BYTE jump[kEntryBytes] = {0xE9, 0, 0, 0, 0, 0x90, 0x90, 0x90, 0x90, 0x90};
	rel = (DWORD)((BYTE*)hook - (fn + 5));
	memcpy(jump + 1, &rel, 4);
	memcpy(fn, jump, kEntryBytes);
	VirtualProtect(fn, kEntryBytes, old, &old);
	FlushInstructionCache(GetCurrentProcess(), fn, kEntryBytes);
	return true;
}

void RaiseLimit(DWORD* imm) {
	DWORD old;
	if (!VirtualProtect(imm, 4, PAGE_EXECUTE_READWRITE, &old)) {
		LOG("freeze check: could not unprotect the limit (%lu)", GetLastError());
		return;
	}
	*imm = 0x7FFFFFFF;
	VirtualProtect(imm, 4, old, &old);
	FlushInstructionCache(GetCurrentProcess(), imm, 4);
}
}  // namespace

// From XLiveInitializeEx, the game's code is unpacked by then and no session exists yet.
void WatchdogStart() {
	if (g_thread.joinable()) {
		return;
	}
	Fields f;
	if (!Locate(&f)) {
		LOG("freeze check: not found in this exe; nothing watched");
		return;
	}
	LOG("freeze check: at %p (game %p+%lX, network %p+%lX), FreezeGuard %s", (void*)((BYTE*)f.limit - 0x49), f.game,
	    f.frame, f.net, f.count, Cfg().freezeGuard ? "on" : "off");
	if (Cfg().freezeGuard) {
		RaiseLimit(f.limit);
	}
	if (Cfg().log) {
		g_leaving = f.leaving;
		g_checkCall = (DWORD)f.limit + 0x0F;  // limit, jb, mov eax,[esi], mov edx,[eax+0x78], push 0, call edx
		bool a = HookEntry(f.leave, f.leaving, (void*)LeaveHook, &g_resumeLeave);
		bool b = HookEntry(f.sessionLeave, f.leaving, (void*)SessionLeaveHook, &g_resumeSessionLeave);
		LOG("session leave: logging leave(reason) at %p (%s) and the session's leave at %p (%s)", f.leave,
		    a ? "ok" : "not hooked", f.sessionLeave, b ? "ok" : "not hooked");
	}
	g_stop = false;
	g_thread = std::thread(Watch, f);
}

void WatchdogStop() {
	g_stop = true;
	if (g_thread.joinable()) {
		g_thread.join();
	}
}

}  // namespace lp2net
