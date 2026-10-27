// Voice round trip without the game: two processes load xlive.dll (slots 0 and 1), create the XHV engine and
// register each other as talkers the way LP2 does (NetworkXboxVoice). Set up lp2net.ini next to the exe first:
//   [Network] Peers=127.0.0.1:3075,127.0.0.1:3175     (so the two find each other)
//   [Debug]   VoiceFeedWav=<a speech WAV>  VoiceFeedCopy=0  VoiceDumpWav=<out.wav>
//   [Steam]   Enabled=0                                 (feed and dump only, no microphone)
// then run `voicetest a` and `voicetest b` at the same time. Each prints PASS when it heard the other talk.
#include "../src/xlive/gfwl.h"

#include <tlhelp32.h>

#include <cstdio>
#include <string>

namespace {
using XHV_PROCESSING_MODE = DWORD;

struct XHV_INIT_PARAMS {
	DWORD dwMaxRemoteTalkers;
	DWORD dwMaxLocalTalkers;
	const XHV_PROCESSING_MODE* localTalkerEnabledModes;
	DWORD dwNumLocalTalkerEnabledModes;
	const XHV_PROCESSING_MODE* remoteTalkerEnabledModes;
	DWORD dwNumRemoteTalkerEnabledModes;
	BOOL bCustomVADProvided;
	BOOL bRelaxPrivileges;
	void* pfnMicrophoneRawDataReady;
	HWND hwndFocus;
};

// IXHVEngine as the game calls it
struct IXHVEngine {
	virtual ULONG __stdcall AddRef() = 0;
	virtual ULONG __stdcall Release() = 0;
	virtual HRESULT __stdcall Lock(DWORD) = 0;
	virtual HRESULT __stdcall StartLocalProcessingModes(DWORD, const XHV_PROCESSING_MODE*, DWORD) = 0;
	virtual HRESULT __stdcall StopLocalProcessingModes(DWORD, const XHV_PROCESSING_MODE*, DWORD) = 0;
	virtual HRESULT __stdcall StartRemoteProcessingModes(XUID, const XHV_PROCESSING_MODE*, DWORD) = 0;
	virtual HRESULT __stdcall StopRemoteProcessingModes(XUID, const XHV_PROCESSING_MODE*, DWORD) = 0;
	virtual HRESULT __stdcall SetMaxDecodePackets(DWORD) = 0;
	virtual HRESULT __stdcall RegisterLocalTalker(DWORD) = 0;
	virtual HRESULT __stdcall UnregisterLocalTalker(DWORD) = 0;
	virtual HRESULT __stdcall RegisterRemoteTalker(XUID, void*, void*, void*) = 0;
	virtual HRESULT __stdcall UnregisterRemoteTalker(XUID) = 0;
	virtual HRESULT __stdcall GetRemoteTalkers(DWORD*, XUID*) = 0;
	virtual BOOL __stdcall IsHeadsetPresent(DWORD) = 0;
	virtual BOOL __stdcall IsLocalTalking(DWORD) = 0;
	virtual BOOL __stdcall IsRemoteTalking(XUID) = 0;
	virtual DWORD __stdcall GetDataReadyFlags() = 0;
	virtual HRESULT __stdcall GetLocalChatData(DWORD, BYTE*, DWORD*, DWORD*) = 0;
	virtual HRESULT __stdcall SetPlaybackPriority(XUID, DWORD, DWORD) = 0;
	virtual HRESULT __stdcall SubmitIncomingChatData(XUID, const BYTE*, DWORD*) = 0;
};

HMODULE g_dll;
template <typename T>
T Ord(WORD n) {
	auto p = (T)GetProcAddress(g_dll, MAKEINTRESOURCEA(n));
	if (!p) {
		printf("missing ordinal %u\n", n);
		ExitProcess(2);
	}
	return p;
}

// A running game holds slot 0/1 and their core ports: our two processes would take other slots and say hello to
// the game instead of each other.
bool GameRunning() {
	HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	PROCESSENTRY32W pe{sizeof(pe)};
	bool found = false;
	for (BOOL ok = Process32FirstW(snap, &pe); ok && !found; ok = Process32NextW(snap, &pe)) {
		found = !_wcsicmp(pe.szExeFile, L"LP2DX9.exe") || !_wcsicmp(pe.szExeFile, L"LP2DX11.exe");
	}
	CloseHandle(snap);
	return found;
}

std::string Here(const char* file) {
	char path[MAX_PATH];
	GetModuleFileNameA(nullptr, path, MAX_PATH);
	std::string p = path;
	return p.substr(0, p.find_last_of("\\/") + 1) + file;
}
}  // namespace

int main(int argc, char** argv) {
	if (argc < 2) {
		printf("usage: voicetest a|b [seconds]\n");
		return 1;
	}
	std::string me = argv[1], other = me == "a" ? "b" : "a";
	int seconds = argc > 2 ? atoi(argv[2]) : 25;
	if (GameRunning()) {
		printf("SKIP: Lost Planet 2 is running; this test would talk to it. Quit the game first.\n");
		return 5;
	}
	g_dll = LoadLibraryW(L"xlive.dll");
	if (!g_dll) {
		printf("cannot load xlive.dll\n");
		return 2;
	}
	Ord<HRESULT(WINAPI*)(XLIVE_INITIALIZE_INFO*, DWORD)>(5297)(nullptr, 0);
	XUID mine = 0, theirs = 0;
	Ord<DWORD(WINAPI*)(DWORD, XUID*)>(5261)(0, &mine);
	std::string mineFile = Here(("voicetest_" + me + ".xuid").c_str());
	std::string theirFile = Here(("voicetest_" + other + ".xuid").c_str());
	if (FILE* f = fopen(mineFile.c_str(), "w")) {
		fprintf(f, "%016llX\n", mine);
		fclose(f);
	}
	for (int i = 0; i < 300 && !theirs; ++i) {
		if (FILE* f = fopen(theirFile.c_str(), "r")) {
			unsigned long long v = 0;
			if (fscanf(f, "%llx", &v) == 1) {
				theirs = v;
			}
			fclose(f);
		}
		Sleep(100);
	}
	if (!theirs) {
		printf("FAIL: the other process did not start\n");
		return 3;
	}
	printf("%s: me %016llX, other %016llX\n", me.c_str(), mine, theirs);

	const XHV_PROCESSING_MODE localModes[] = {1, 2}, remoteModes[] = {2}, chat[] = {2};
	XHV_INIT_PARAMS p{30, 1, localModes, 2, remoteModes, 1, FALSE, FALSE, nullptr, nullptr};
	IXHVEngine* e = nullptr;
	HRESULT hr = Ord<HRESULT(WINAPI*)(XHV_INIT_PARAMS*, HANDLE*, IXHVEngine**)>(5008)(&p, nullptr, &e);
	if (FAILED(hr) || !e) {
		printf("FAIL: XHVCreateEngine 0x%08lX\n", hr);
		return 4;
	}
	e->RegisterLocalTalker(0);
	e->StartLocalProcessingModes(0, chat, 1);
	e->RegisterRemoteTalker(theirs, nullptr, nullptr, nullptr);
	e->StartRemoteProcessingModes(theirs, chat, 1);
	BOOL muted = TRUE;
	Ord<DWORD(WINAPI*)(DWORD, XUID, BOOL*)>(5314)(0, theirs, &muted);
	printf("headset %d, mute list says %d\n", e->IsHeadsetPresent(0), muted);

	bool local = false, remote = false;
	int localOn = 0, remoteOn = 0;
	ULONGLONG start = GetTickCount64();
	while (GetTickCount64() - start < (ULONGLONG)seconds * 1000) {
		bool l = e->IsLocalTalking(0) != 0, r = e->IsRemoteTalking(theirs) != 0;
		double t = (GetTickCount64() - start) / 1000.0;
		if (l != local) {
			printf("%6.2f s  I %s\n", t, l ? "talk" : "stop");
			localOn += l;
			local = l;
		}
		if (r != remote) {
			printf("%6.2f s  other %s\n", t, r ? "talks" : "stops");
			remoteOn += r;
			remote = r;
		}
		if (e->GetDataReadyFlags() != 0) {
			printf("unexpected: data for the game's own channel\n");
		}
		Sleep(20);
	}
	e->UnregisterRemoteTalker(theirs);
	e->UnregisterLocalTalker(0);
	e->Release();
	Ord<void(WINAPI*)()>(5003)();
	remove(mineFile.c_str());
	bool pass = localOn > 0 && remoteOn > 0;
	printf("%s: I talked %d times, heard the other %d times\n", pass ? "PASS" : "FAIL", localOn, remoteOn);
	return pass ? 0 : 1;
}
