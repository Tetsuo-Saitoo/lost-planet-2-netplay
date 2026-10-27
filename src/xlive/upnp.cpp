#include "upnp.h"

#include "config.h"
#include "log.h"
#include "net.h"
#include "util.h"

#include <natupnp.h>
#include <ws2tcpip.h>

#include <condition_variable>
#include <deque>
#include <mutex>
#include <set>

namespace lp2net {
namespace {
std::mutex g_mutex;
std::condition_variable g_cv;
std::deque<uint16_t> g_pending;
std::set<uint16_t> g_requested, g_mapped;
// plain Win32 threads: a std::thread still running at process exit aborts the game (0xC0000409)
HANDLE g_worker = nullptr;
bool g_stop = false;
void (*g_onPublicIp)(const std::string&) = nullptr;
std::string g_status = "UPnP: idle";

void SetStatus(const std::string& s) {
	std::lock_guard lock(g_mutex);
	g_status = s;
}

IStaticPortMappingCollection* GetCollection() {
	IUPnPNAT* nat = nullptr;
	if (FAILED(CoCreateInstance(__uuidof(UPnPNAT), nullptr, CLSCTX_ALL, __uuidof(IUPnPNAT), (void**)&nat))) {
		return nullptr;
	}
	IStaticPortMappingCollection* coll = nullptr;
	// slow: SSDP discovery of the router happens here
	nat->get_StaticPortMappingCollection(&coll);
	nat->Release();
	return coll;
}

void Worker() {
	CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	IStaticPortMappingCollection* coll = nullptr;
	bool reportedIp = false;
	for (;;) {
		uint16_t port;
		{
			std::unique_lock lock(g_mutex);
			g_cv.wait(lock, [] { return g_stop || !g_pending.empty(); });
			if (g_stop) {
				break;
			}
			port = g_pending.front();
			g_pending.pop_front();
		}
		if (!coll && !(coll = GetCollection())) {
			SetStatus("UPnP: no router answered (forward ports manually if hosting over the internet)");
			LOG("upnp: no IGD found");
			continue;
		}
		char lan[INET_ADDRSTRLEN];
		inet_ntop(AF_INET, &Me().lan, lan, sizeof(lan));
		BSTR proto = SysAllocString(L"UDP");
		BSTR client = SysAllocString(Widen(lan).c_str());
		BSTR desc = SysAllocString(L"Lost Planet 2 (LP2Net)");
		IStaticPortMapping* map = nullptr;
		HRESULT hr = coll->Add(port, proto, port, client, VARIANT_TRUE, desc, &map);
		LOG("upnp: map UDP %u -> %s:%u: 0x%08lX", port, lan, port, hr);
		if (FAILED(hr)) {
			// The router already has this port, usually our own mapping left over from a game that was killed
			// before it could remove it. If it points at this PC it is just as good; another PC's we leave alone.
			IStaticPortMapping* existing = nullptr;
			if (SUCCEEDED(coll->get_Item(port, proto, &existing)) && existing) {
				BSTR owner = nullptr;
				long internalPort = 0;
				existing->get_InternalClient(&owner);
				existing->get_InternalPort(&internalPort);
				bool ours = owner && Narrow(owner) == lan && internalPort == port;
				LOG("upnp: UDP %u already mapped to %s:%ld (%s)", port, owner ? Narrow(owner).c_str() : "?",
				    internalPort, ours ? "this PC, keeping it" : "ANOTHER PC");
				if (owner) {
					SysFreeString(owner);
				}
				if (ours) {
					map = existing;
					hr = S_OK;
				}
				else {
					existing->Release();
				}
			}
		}
		if (SUCCEEDED(hr) && map) {
			{
				std::lock_guard lock(g_mutex);
				g_mapped.insert(port);
			}
			SetStatus("UPnP: forwarding " + std::to_string(g_mapped.size()) + " port(s)");
			BSTR ext = nullptr;
			if (!reportedIp && SUCCEEDED(map->get_ExternalIPAddress(&ext)) && ext) {
				reportedIp = true;
				if (g_onPublicIp) {
					g_onPublicIp(Narrow(ext));
				}
				SysFreeString(ext);
			}
			map->Release();
		}
		else {
			SetStatus("UPnP: router refused the port mapping");
		}
		SysFreeString(proto);
		SysFreeString(client);
		SysFreeString(desc);
	}
	if (coll) {
		std::set<uint16_t> mapped;
		{
			std::lock_guard lock(g_mutex);
			mapped.swap(g_mapped);
		}
		BSTR proto = SysAllocString(L"UDP");
		for (uint16_t p : mapped) {
			coll->Remove(p, proto);
		}
		SysFreeString(proto);
		coll->Release();
	}
	CoUninitialize();
}
}  // namespace

void UpnpMapUdp(uint16_t port) {
	if (!Cfg().upnp) {
		return;
	}
	std::lock_guard lock(g_mutex);
	if (!g_requested.insert(port).second) {
		return;
	}
	if (!g_worker) {
		g_worker = CreateThread(nullptr, 0, [](void*) -> DWORD { Worker(); return 0; }, nullptr, 0, nullptr);
	}
	g_pending.push_back(port);
	g_cv.notify_one();
}

void UpnpSetPublicIpCallback(void (*cb)(const std::string& ip)) {
	g_onPublicIp = cb;
}

void UpnpUnmapAll() {
	{
		std::lock_guard lock(g_mutex);
		g_stop = true;
	}
	g_cv.notify_one();
	if (g_worker) {
		WaitForSingleObject(g_worker, 3000);
		CloseHandle(g_worker);
		g_worker = nullptr;
	}
}

std::string UpnpStatus() {
	std::lock_guard lock(g_mutex);
	return g_status;
}

}  // namespace lp2net
