// Debug probe for proximity voice ([Debug] VoiceProbe=1, off by default). Finds the game's players, soldiers and
// cameras through MT Framework's type registry and logs them every 10 s, so the layout and the player -> member
// mapping can be read from a real mission. It only reads memory; it never calls into the game.
//
// MtDTI (one static object per class, in .data): +0 vtable (the same for all), +4 name, +8 next sibling,
// +0xC first child, +0x10 parent, +0x18 size (low 24 bits). Every MtObject's vtable slot 4 is getDTI, compiled as
// `mov eax, dti; ret`, so an object's class can be read from its vtable without calling it.
#include "config.h"
#include "log.h"

#include <windows.h>

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace lp2net {
namespace {
struct Section {
	uintptr_t lo = 0, hi = 0;
	bool Has(uintptr_t a) const { return a >= lo && a < hi; }
};
Section g_text, g_rdata, g_data;
std::map<uintptr_t, std::string> g_dtiName;  // dti -> class name
std::map<uintptr_t, uintptr_t> g_vtDti;      // vtable -> dti
HANDLE g_thread = nullptr;
volatile bool g_stop = false;

// Copy memory that may vanish under us (another thread frees it); false if it faulted.
bool SafeCopy(void* dst, const void* src, size_t n) {
	__try {
		memcpy(dst, src, n);
		return true;
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}

template <typename T>
bool Read(uintptr_t a, T* out) {
	return a && SafeCopy(out, (const void*)a, sizeof(T));
}

bool FindSections() {
	auto base = (uintptr_t)GetModuleHandleW(nullptr);
	auto dos = (const IMAGE_DOS_HEADER*)base;
	auto nt = (const IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
	auto sec = IMAGE_FIRST_SECTION(nt);
	for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
		Section s{base + sec[i].VirtualAddress, base + sec[i].VirtualAddress + sec[i].Misc.VirtualSize};
		char name[9] = {};
		memcpy(name, sec[i].Name, 8);
		if (!strcmp(name, ".text")) {
			g_text = s;
		}
		else if (!strcmp(name, ".rdata")) {
			g_rdata = s;
		}
		else if (!strcmp(name, ".data")) {
			g_data = s;
		}
	}
	return g_text.lo && g_rdata.lo && g_data.lo;
}

bool CString(uintptr_t a, std::string* out) {
	char buf[64];
	if (!g_rdata.Has(a) || !SafeCopy(buf, (const void*)a, std::min<size_t>(sizeof(buf), g_rdata.hi - a))) {
		return false;
	}
	size_t n = strnlen(buf, sizeof(buf));
	if (n < 2 || n == sizeof(buf)) {
		return false;
	}
	for (size_t i = 0; i < n; ++i) {
		if (buf[i] < 0x20 || buf[i] > 0x7E) {
			return false;
		}
	}
	out->assign(buf, n);
	return true;
}

// All MtDTI objects: the "MtObject" class's DTI gives the shared DTI vtable, then every .data pair
// [that vtable][name pointer] is a class.
bool BuildRegistry() {
	std::vector<uint32_t> data((g_data.hi - g_data.lo) / 4);
	if (!SafeCopy(data.data(), (const void*)g_data.lo, data.size() * 4)) {
		return false;
	}
	std::map<uint32_t, int> votes;  // candidate DTI vtables: values sitting before a class-name pointer
	for (size_t i = 1; i < data.size(); ++i) {
		std::string n;
		if (g_rdata.Has(data[i - 1]) && CString(data[i], &n) && n == "MtObject") {
			votes[data[i - 1]]++;
		}
	}
	uint32_t vt = 0;
	int best = 0;
	for (const auto& [v, c] : votes) {
		if (c > best) {
			vt = v;
			best = c;
		}
	}
	if (!vt) {
		return false;
	}
	for (size_t i = 0; i + 1 < data.size(); ++i) {
		std::string n;
		if (data[i] == vt && CString(data[i + 1], &n)) {
			g_dtiName[g_data.lo + i * 4] = n;
		}
	}
	// getDTI functions: mov eax, <dti>; ret
	std::map<uintptr_t, uintptr_t> fnDti;
	std::vector<BYTE> text(g_text.hi - g_text.lo);
	if (!SafeCopy(text.data(), (const void*)g_text.lo, text.size())) {
		return false;
	}
	for (size_t i = 0; i + 6 <= text.size(); ++i) {
		if (text[i] == 0xB8 && text[i + 5] == 0xC3) {
			uint32_t d;
			memcpy(&d, &text[i + 1], 4);
			if (g_dtiName.count(d)) {
				fnDti[g_text.lo + i] = d;
			}
		}
	}
	std::vector<uint32_t> rdata((g_rdata.hi - g_rdata.lo) / 4);
	if (!SafeCopy(rdata.data(), (const void*)g_rdata.lo, rdata.size() * 4)) {
		return false;
	}
	for (size_t i = 4; i < rdata.size(); ++i) {
		auto it = fnDti.find(rdata[i]);
		if (it != fnDti.end() && g_text.Has(rdata[i - 4]) && g_text.Has(rdata[i - 1])) {
			g_vtDti[g_rdata.lo + (i - 4) * 4] = it->second;
		}
	}
	LOG("probe: %zu classes, %zu vtables", g_dtiName.size(), g_vtDti.size());
	return !g_vtDti.empty();
}

uintptr_t DtiByName(const char* name) {
	for (const auto& [d, n] : g_dtiName) {
		if (n == name) {
			return d;
		}
	}
	return 0;
}

bool DerivesFrom(uintptr_t dti, uintptr_t base) {
	for (int depth = 0; dti && depth < 32; ++depth) {
		if (dti == base) {
			return true;
		}
		uint32_t parent = 0;
		if (!Read(dti + 0x10, &parent) || parent == dti) {
			return false;
		}
		dti = parent;
	}
	return false;
}

struct Found {
	uintptr_t addr;
	uintptr_t dti;
};

// Every live object of the wanted classes: heap (private read/write memory) and the game's own .data, scanned
// for their vtables.
std::vector<Found> Scan(const std::set<uintptr_t>& wantVt) {
	std::vector<Found> out;
	std::vector<uint32_t> buf(64 * 1024 / 4);
	auto scanRange = [&](uintptr_t lo, uintptr_t end) {
		for (uintptr_t p = lo; p < end && out.size() < 4000; p += buf.size() * 4) {
			size_t n = std::min<size_t>(buf.size(), (end - p) / 4);
			if (!SafeCopy(buf.data(), (const void*)p, n * 4)) {
				break;
			}
			for (size_t i = 0; i < n; ++i) {
				if (g_rdata.Has(buf[i]) && wantVt.count(buf[i])) {
					out.push_back({p + i * 4, g_vtDti[buf[i]]});
				}
			}
		}
	};
	MEMORY_BASIC_INFORMATION mbi;
	for (uintptr_t a = 0x10000; VirtualQuery((void*)a, &mbi, sizeof(mbi)) && a < 0xFFFF0000;
	     a = (uintptr_t)mbi.BaseAddress + mbi.RegionSize) {
		if (mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE && mbi.Protect == PAGE_READWRITE) {
			scanRange((uintptr_t)mbi.BaseAddress, (uintptr_t)mbi.BaseAddress + mbi.RegionSize);
		}
	}
	scanRange(g_data.lo, g_data.hi);
	return out;
}

std::string Vec(uintptr_t a) {
	float v[3] = {};
	char buf[64];
	if (!Read(a, &v)) {
		return "?";
	}
	snprintf(buf, sizeof(buf), "(%.1f %.1f %.1f)", v[0], v[1], v[2]);
	return buf;
}

// Pointer fields of an object that point at other objects we found, as "+off->Class".
std::string Links(uintptr_t obj, size_t size, const std::map<uintptr_t, uintptr_t>& objs) {
	std::vector<uint32_t> f(size / 4);
	std::string s;
	if (!SafeCopy(f.data(), (const void*)obj, f.size() * 4)) {
		return "?";
	}
	for (size_t i = 1; i < f.size(); ++i) {
		auto it = objs.find(f[i]);
		if (it != objs.end()) {
			char buf[96];
			snprintf(buf, sizeof(buf), " +0x%zx->%s@%08X", i * 4, g_dtiName[it->second].c_str(), f[i]);
			s += buf;
		}
	}
	return s.empty() ? " (none)" : s;
}

// Printable strings inside an object (gamertags, ids), as "+off 'text'".
std::string Strings(uintptr_t obj, size_t size) {
	std::vector<char> b(size);
	std::string s;
	if (!SafeCopy(b.data(), (const void*)obj, size)) {
		return "?";
	}
	for (size_t i = 0; i < size;) {
		size_t j = i;
		while (j < size && b[j] >= 0x20 && b[j] < 0x7F) {
			++j;
		}
		if (j - i >= 3 && j < size && b[j] == 0) {
			char buf[80];
			snprintf(buf, sizeof(buf), " +0x%zx '%.40s'", i, std::string(&b[i], j - i).c_str());
			s += buf;
		}
		// UTF-16
		size_t k = i;
		while (k + 1 < size && b[k] >= 0x20 && b[k] < 0x7F && b[k + 1] == 0) {
			k += 2;
		}
		if ((k - i) / 2 >= 3) {
			std::string t;
			for (size_t m = i; m < k; m += 2) {
				t += b[m];
			}
			char buf[80];
			snprintf(buf, sizeof(buf), " +0x%zx L'%.40s'", i, t.c_str());
			s += buf;
			j = std::max(j, k);
		}
		i = std::max(j, i + 1);
	}
	return s;
}

std::string Dwords(uintptr_t a, int n) {
	std::string s;
	for (int i = 0; i < n; ++i) {
		uint32_t v = 0;
		char buf[16];
		Read(a + i * 4, &v);
		snprintf(buf, sizeof(buf), " %X", v);
		s += buf;
	}
	return s;
}

void Probe() {
	uintptr_t human = DtiByName("uHuman"), coord = DtiByName("uCoord"), camera = DtiByName("uCamera");
	uintptr_t player = DtiByName("cPlayer"), ctrlPl = DtiByName("uCtrlPlayer"), ctrlCam = DtiByName("uCtrlCamera");
	uintptr_t sCam = DtiByName("sCamera"), puppet = DtiByName("cThinkNetPuppet");
	std::set<uintptr_t> want;
	for (const auto& [vt, d] : g_vtDti) {
		if (DerivesFrom(d, human) || DerivesFrom(d, camera) || d == player || d == ctrlPl || d == ctrlCam ||
		    d == sCam || DerivesFrom(d, puppet)) {
			want.insert(vt);
		}
	}
	ULONGLONG t0 = GetTickCount64();
	std::vector<Found> found = Scan(want);
	std::map<uintptr_t, uintptr_t> objs;
	std::map<std::string, int> counts;
	for (const auto& f : found) {
		objs[f.addr] = f.dti;
		counts[g_dtiName[f.dti]]++;
	}
	std::string summary;
	for (const auto& [n, c] : counts) {
		summary += " " + n + " x" + std::to_string(c);
	}
	LOG("probe: scan %llu ms, %zu objects:%s", GetTickCount64() - t0, found.size(), summary.c_str());
	for (const auto& f : found) {
		const std::string& n = g_dtiName[f.dti];
		uint32_t size = 0;
		Read(f.dti + 0x18, &size);
		size &= 0xFFFFFF;
		if (f.dti == player) {
			LOG("probe: cPlayer@%08X links%s", (uint32_t)f.addr, Links(f.addr, size, objs).c_str());
			LOG("probe:   strings%s", Strings(f.addr, size).c_str());
			LOG("probe:   +0x10..:%s", Dwords(f.addr + 0x10, 16).c_str());
		}
		else if (f.dti == ctrlPl || f.dti == ctrlCam || DerivesFrom(f.dti, puppet)) {
			LOG("probe: %s@%08X links%s", n.c_str(), (uint32_t)f.addr, Links(f.addr, size, objs).c_str());
		}
		else if (DerivesFrom(f.dti, human) && DerivesFrom(f.dti, coord)) {
			LOG("probe: %s@%08X pos %s links%s", n.c_str(), (uint32_t)f.addr, Vec(f.addr + 0x30).c_str(),
			    Links(f.addr, std::min<uint32_t>(size, 0x2000), objs).c_str());
		}
		else if (DerivesFrom(f.dti, camera)) {
			LOG("probe: %s@%08X +30 %s +40 %s +50 %s +60 %s", n.c_str(), (uint32_t)f.addr, Vec(f.addr + 0x30).c_str(),
			    Vec(f.addr + 0x40).c_str(), Vec(f.addr + 0x50).c_str(), Vec(f.addr + 0x60).c_str());
		}
		else if (f.dti == sCam) {
			LOG("probe: sCamera@%08X links%s", (uint32_t)f.addr, Links(f.addr, size, objs).c_str());
		}
	}
	// globals in .data that point at players or cameras: stable places to find them later
	std::vector<uint32_t> data((g_data.hi - g_data.lo) / 4);
	if (SafeCopy(data.data(), (const void*)g_data.lo, data.size() * 4)) {
		for (size_t i = 0; i < data.size(); ++i) {
			auto it = objs.find(data[i]);
			if (it != objs.end() && (it->second == player || it->second == sCam || it->second == ctrlCam)) {
				LOG("probe: global %08X -> %s@%08X", (uint32_t)(g_data.lo + i * 4), g_dtiName[it->second].c_str(),
				    data[i]);
			}
		}
	}
}

DWORD WINAPI ProbeThread(void*) {
	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
	if (!FindSections()) {
		LOG("probe: no .text/.rdata/.data in the game image");
		return 0;
	}
	for (int tries = 0; !g_stop && g_vtDti.empty() && tries < 30; ++tries) {
		if (!BuildRegistry()) {
			Sleep(2000);
		}
	}
	while (!g_stop) {
		Probe();
		for (int i = 0; i < 100 && !g_stop; ++i) {
			Sleep(100);
		}
	}
	return 0;
}
}  // namespace

void ProbeStart() {
	if (!g_thread && Cfg().voiceProbe) {
		LOG("probe: on ([Debug] VoiceProbe=1)");
		g_thread = CreateThread(nullptr, 0, ProbeThread, nullptr, 0, nullptr);
	}
}

void ProbeStop() {
	if (g_thread) {
		g_stop = true;
		WaitForSingleObject(g_thread, 3000);
		CloseHandle(g_thread);
		g_thread = nullptr;
	}
}

}  // namespace lp2net
