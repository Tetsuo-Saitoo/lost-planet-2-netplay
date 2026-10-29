// A log line when the game crashes: the exception, where it happened and the code addresses on the stack, so a
// crash on a tester's PC names the code that led to it (Windows' own report of it has no dump and no stack).
// A vectored handler sees an exception before the game's own handlers do, so it also logs ones the game handles:
// only the kinds that are crashes when nobody handles them, and only the first few.
#include "log.h"
#include "util.h"

#include <windows.h>

#include <cstdio>

namespace lp2net {
namespace {
constexpr LONG kMaxLogged = 4;
constexpr int kStackWords = 256;  // how far up the stack to look for return addresses
constexpr int kMaxTrail = 12;

LONG g_logged = 0;
BYTE* g_text = nullptr;  // the exe's code
size_t g_textSize = 0;

bool Read(const void* p, void* out, size_t cb) {
	MEMORY_BASIC_INFORMATION mbi;
	if (!VirtualQuery(p, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS))) {
		return false;
	}
	if ((const BYTE*)p + cb > (const BYTE*)mbi.BaseAddress + mbi.RegionSize) {
		return false;
	}
	memcpy(out, p, cb);
	return true;
}

// "0084F8E8" for the game's code, "xlive.dll+1A2B" for another module's, "" for anything else.
void Describe(DWORD v, char* out, size_t cb) {
	*out = 0;
	if (v - (DWORD)g_text < g_textSize) {
		snprintf(out, cb, "%08lX", v);
		return;
	}
	MEMORY_BASIC_INFORMATION mbi;
	if (!VirtualQuery((void*)v, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT ||
	    !(mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))) {
		return;
	}
	HMODULE mod = nullptr;
	char path[MAX_PATH];
	if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
	                        (LPCSTR)v, &mod) ||
	    !GetModuleFileNameA(mod, path, sizeof(path))) {
		return;
	}
	const char* name = strrchr(path, '\\');
	snprintf(out, cb, "%s+%lX", name ? name + 1 : path, v - (DWORD)mod);
}

bool IsCrashKind(DWORD code) {
	switch (code) {
	case EXCEPTION_ACCESS_VIOLATION:
	case EXCEPTION_ILLEGAL_INSTRUCTION:
	case EXCEPTION_PRIV_INSTRUCTION:
	case EXCEPTION_INT_DIVIDE_BY_ZERO:
	case EXCEPTION_STACK_OVERFLOW:
	case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
		return true;
	default:
		return false;
	}
}

LONG CALLBACK OnException(EXCEPTION_POINTERS* ep) {
	const EXCEPTION_RECORD* er = ep->ExceptionRecord;
	if (!IsCrashKind(er->ExceptionCode) || InterlockedIncrement(&g_logged) > kMaxLogged) {
		return EXCEPTION_CONTINUE_SEARCH;
	}
	const CONTEXT* c = ep->ContextRecord;
	char where[MAX_PATH + 16], access[64] = "";
	Describe(c->Eip, where, sizeof(where));
	if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2) {
		snprintf(access, sizeof(access), " (%s %08lX)",
		         er->ExceptionInformation[0] == 8 ? "executing" : er->ExceptionInformation[0] ? "writing" : "reading",
		         (DWORD)er->ExceptionInformation[1]);
	}
	// A call through a null or bad pointer leaves its return address at [esp].
	char trail[kMaxTrail * (MAX_PATH + 2)] = "";
	size_t n = 0;
	const DWORD* stack = (const DWORD*)c->Esp;
	for (int i = 0, found = 0; i < kStackWords && found < kMaxTrail; ++i) {
		DWORD v;
		char d[MAX_PATH + 16];
		if (!Read(stack + i, &v, 4)) {
			break;
		}
		Describe(v, d, sizeof(d));
		if (*d) {
			n += snprintf(trail + n, sizeof(trail) - n, " [esp+%X] %s", i * 4, d);
			++found;
		}
	}
	LOG("crash: exception %08lX at %08lX %s%s on thread %lu; eax=%08lX ebx=%08lX ecx=%08lX edx=%08lX esi=%08lX "
	    "edi=%08lX ebp=%08lX esp=%08lX; code addresses on the stack:%s",
	    er->ExceptionCode, c->Eip, where, access, GetCurrentThreadId(), c->Eax, c->Ebx, c->Ecx, c->Edx, c->Esi, c->Edi,
	    c->Ebp, c->Esp, trail);
	return EXCEPTION_CONTINUE_SEARCH;
}
}  // namespace

void CrashLogStart() {
	static PVOID handler = nullptr;
	if (handler) {
		return;
	}
	FindSection(".text", &g_text, &g_textSize);
	handler = AddVectoredExceptionHandler(1, OnException);
}

}  // namespace lp2net
