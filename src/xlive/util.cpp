#include "util.h"

#include <bcrypt.h>
#include <ws2tcpip.h>

#include <cstdio>
#include <cstring>

namespace lp2net {

std::wstring Widen(const std::string& s) {
	if (s.empty()) {
		return {};
	}
	int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
	std::wstring w(n, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
	return w;
}

std::string Narrow(const std::wstring& w) {
	if (w.empty()) {
		return {};
	}
	int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
	std::string s(n, '\0');
	WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
	return s;
}

void RandomBytes(void* out, size_t n) {
	BCryptGenRandom(nullptr, (PUCHAR)out, (ULONG)n, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
}

ULONGLONG RandomU64() {
	ULONGLONG v;
	RandomBytes(&v, sizeof(v));
	return v;
}

std::string AddrToString(const sockaddr_in& a) {
	char ip[INET_ADDRSTRLEN] = "?";
	inet_ntop(AF_INET, &a.sin_addr, ip, sizeof(ip));
	return std::string(ip) + ":" + std::to_string(ntohs(a.sin_port));
}

std::string XnAddrToString(const XNADDR& x) {
	char buf[160];
	char lan[INET_ADDRSTRLEN], wan[INET_ADDRSTRLEN];
	inet_ntop(AF_INET, &x.ina, lan, sizeof(lan));
	inet_ntop(AF_INET, &x.inaOnline, wan, sizeof(wan));
	snprintf(buf, sizeof(buf), "lan=%s wan=%s:%u id=%02x%02x%02x%02x off=%u", lan, wan, ntohs(x.wPortOnline),
	         x.abEnet[0], x.abEnet[1], x.abEnet[2], x.abEnet[3], x.abEnet[4] | (x.abEnet[5] << 8));
	return buf;
}

DWORD CompleteOverlapped(XOVERLAPPED* ov, DWORD error, DWORD result) {
	if (!ov) {
		return error;
	}
	ov->InternalHigh = result;
	ov->dwExtendedError = error ? HRESULT_FROM_WIN32(error) : 0;
	ov->InternalLow = error;
	if (ov->hEvent) {
		SetEvent(ov->hEvent);
	}
	if (ov->pCompletionRoutine) {
		ov->pCompletionRoutine(error, result, ov);
	}
	return ERROR_IO_PENDING;
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

}  // namespace lp2net
