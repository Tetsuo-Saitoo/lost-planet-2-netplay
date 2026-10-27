// XNotify: per-listener notification queues signalled through the listener's event handle.
#include "xnotify.h"

#include "config.h"
#include "log.h"

#include <deque>
#include <map>
#include <mutex>
#include <utility>

namespace lp2net {
namespace {
struct Listener {
	ULONGLONG areas;
	std::deque<std::pair<DWORD, ULONG_PTR>> queue;
};
std::mutex g_mutex;
std::map<HANDLE, Listener> g_listeners;

ULONGLONG AreaBit(DWORD id) {
	return 1ull << ((id >> 25) & 0x3F);
}
}  // namespace

void PostNotification(DWORD id, ULONG_PTR param) {
	std::lock_guard lock(g_mutex);
	LOG("notify 0x%08lX param=0x%IX", id, param);
	for (auto& [h, l] : g_listeners) {
		if (l.areas & AreaBit(id)) {
			l.queue.emplace_back(id, param);
			SetEvent(h);
		}
	}
}

bool IsNotifyListener(HANDLE h) {
	std::lock_guard lock(g_mutex);
	return g_listeners.count(h) != 0;
}

bool CloseNotifyListener(HANDLE h) {
	std::lock_guard lock(g_mutex);
	if (!g_listeners.erase(h)) {
		return false;
	}
	CloseHandle(h);
	return true;
}
}  // namespace lp2net

using namespace lp2net;

extern "C" HANDLE WINAPI XNotifyCreateListener(ULONGLONG qwAreas) {
	HANDLE h = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	{
		std::lock_guard lock(g_mutex);
		Listener& l = g_listeners[h] = Listener{qwAreas, {}};
		// GFWL tells a new listener the LIVE connection state. LP2 keeps it and treats every online menu as
		// "Communication error" until it has seen XONLINE_S_LOGON_CONNECTION_ESTABLISHED.
		if (Cfg().onlineMode && (qwAreas & AreaBit(XN_LIVE_CONNECTIONCHANGED))) {
			l.queue.emplace_back(XN_LIVE_CONNECTIONCHANGED, XONLINE_S_LOGON_CONNECTION_ESTABLISHED);
			SetEvent(h);
		}
	}
	LOG("XNotifyCreateListener(areas=0x%llX) -> %p", qwAreas, h);
	return h;
}

extern "C" BOOL WINAPI XNotifyGetNext(HANDLE hNotification, DWORD dwMsgFilter, DWORD* pdwId, ULONG_PTR* pParam) {
	if (!hNotification || hNotification == INVALID_HANDLE_VALUE || !pdwId) {
		SetLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	*pdwId = 0;
	if (pParam) {
		*pParam = 0;
	}
	std::lock_guard lock(g_mutex);
	auto it = g_listeners.find(hNotification);
	if (it == g_listeners.end()) {
		return FALSE;
	}
	auto& q = it->second.queue;
	for (auto e = q.begin(); e != q.end(); ++e) {
		if (dwMsgFilter == 0 || e->first == dwMsgFilter) {
			*pdwId = e->first;
			if (pParam) {
				*pParam = e->second;
			}
			LOG("XNotifyGetNext -> 0x%08lX param=0x%IX", e->first, e->second);
			q.erase(e);
			break;
		}
	}
	if (q.empty()) {
		ResetEvent(hNotification);
	}
	return *pdwId != 0;
}
