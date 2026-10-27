#include "log.h"

#include <share.h>
#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <string>

namespace lp2net {
namespace {
FILE* g_file = nullptr;
SRWLOCK g_lock = SRWLOCK_INIT;
ULONGLONG g_start = 0;
}  // namespace

void LogOpen(const wchar_t* path) {
	AcquireSRWLockExclusive(&g_lock);
	// a second game instance in the same folder can't share the file: use lp2net.2.log, lp2net.3.log, ...
	std::wstring p = path;
	for (int i = 2; !g_file && i <= 9; ++i) {
		g_file = _wfsopen(p.c_str(), L"w", _SH_DENYWR);
		if (!g_file) {
			p = path;
			p.insert(p.find_last_of(L'.'), L"." + std::to_wstring(i));
		}
	}
	g_start = GetTickCount64();
	ReleaseSRWLockExclusive(&g_lock);
}

void Log(const char* fmt, ...) {
	if (!g_file) {
		return;
	}
	char line[2048];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	AcquireSRWLockExclusive(&g_lock);
	ULONGLONG ms = GetTickCount64() - g_start;
	fprintf(g_file, "[%6llu.%03llu %5lu] %s\n", ms / 1000, ms % 1000, GetCurrentThreadId(), line);
	fflush(g_file);
	ReleaseSRWLockExclusive(&g_lock);
}
}  // namespace lp2net
