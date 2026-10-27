// Generic GFWL handles: enumerators, XCloseHandle, overlapped results.
#include "gfwl.h"
#include "log.h"
#include "net.h"
#include "util.h"

#include <map>
#include <mutex>
#include <vector>

namespace lp2net {
bool IsNotifyListener(HANDLE h);
bool CloseNotifyListener(HANDLE h);
bool CloseSessionHandle(HANDLE h);

namespace {
struct Enumerator {
	DWORD itemSize;
	std::vector<std::vector<BYTE>> items;
	size_t next = 0;
};
std::mutex g_mutex;
std::map<HANDLE, Enumerator> g_enums;
UINT_PTR g_nextHandle = 0x4C500000;  // 'LP'
}  // namespace

HANDLE NewFakeHandle() {
	std::lock_guard lock(g_mutex);
	return (HANDLE)(g_nextHandle += 4);
}

HANDLE CreateEnumerator(DWORD itemSize, std::vector<std::vector<BYTE>> items) {
	HANDLE h = NewFakeHandle();
	std::lock_guard lock(g_mutex);
	g_enums[h] = Enumerator{itemSize, std::move(items)};
	return h;
}

HANDLE CreateEmptyEnumerator() {
	return CreateEnumerator(1, {});
}
}  // namespace lp2net

using namespace lp2net;

extern "C" DWORD WINAPI XEnumerate(HANDLE hEnum, void* pvBuffer, DWORD cbBuffer, DWORD* pcItemsReturned,
                                   XOVERLAPPED* pOverlapped) {
	DWORD count = 0;
	{
		std::lock_guard lock(g_mutex);
		auto it = g_enums.find(hEnum);
		if (it != g_enums.end()) {
			Enumerator& e = it->second;
			BYTE* out = (BYTE*)pvBuffer;
			while (e.next < e.items.size() && (count + 1) * e.itemSize <= cbBuffer) {
				memcpy(out + count * e.itemSize, e.items[e.next].data(), e.itemSize);
				++e.next;
				++count;
			}
		}
	}
	if (pcItemsReturned) {
		*pcItemsReturned = count;
	}
	DWORD err = count ? ERROR_SUCCESS : ERROR_NO_MORE_FILES;
	return CompleteOverlapped(pOverlapped, err, count);
}

extern "C" BOOL WINAPI XCloseHandle(HANDLE hObject) {
	if (!hObject || hObject == INVALID_HANDLE_VALUE) {
		return TRUE;
	}
	if (CloseNotifyListener(hObject) || CloseSessionHandle(hObject)) {
		return TRUE;
	}
	std::lock_guard lock(g_mutex);
	g_enums.erase(hObject);
	return TRUE;
}

extern "C" DWORD WINAPI XCancelOverlapped(XOVERLAPPED* pOverlapped) {
	// everything completes synchronously, so there is never anything left to cancel
	return ERROR_SUCCESS;
}

extern "C" DWORD WINAPI XGetOverlappedResult(XOVERLAPPED* pOverlapped, DWORD* pdwResult, BOOL bWait) {
	if (!pOverlapped) {
		return ERROR_INVALID_PARAMETER;
	}
	if (bWait && pOverlapped->hEvent &&
	    (pOverlapped->InternalLow == ERROR_IO_PENDING || pOverlapped->InternalLow == ERROR_IO_INCOMPLETE)) {
		WaitForSingleObject(pOverlapped->hEvent, INFINITE);
	}
	DWORD status = (DWORD)pOverlapped->InternalLow;
	if (status == ERROR_IO_PENDING || status == ERROR_IO_INCOMPLETE) {
		return ERROR_IO_INCOMPLETE;
	}
	if (pdwResult) {
		*pdwResult = (DWORD)pOverlapped->InternalHigh;
	}
	return status;
}

extern "C" DWORD WINAPI XGetOverlappedExtendedError(XOVERLAPPED* pOverlapped) {
	if (!pOverlapped) {
		return GetLastError();
	}
	DWORD status = (DWORD)pOverlapped->InternalLow;
	if (status == ERROR_IO_PENDING || status == ERROR_IO_INCOMPLETE) {
		return ERROR_IO_INCOMPLETE;
	}
	return pOverlapped->dwExtendedError;
}
