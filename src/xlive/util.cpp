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

bool ReadWholeFile(const std::wstring& path, std::vector<BYTE>* out) {
	HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
	if (f == INVALID_HANDLE_VALUE) {
		return false;
	}
	DWORD size = GetFileSize(f, nullptr), got = 0;
	out->resize(size == INVALID_FILE_SIZE ? 0 : size);
	BOOL ok = size != INVALID_FILE_SIZE && (!size || ReadFile(f, out->data(), size, &got, nullptr)) && got == size;
	CloseHandle(f);
	return ok != FALSE;
}

bool WriteFileAtomic(const std::wstring& path, const void* data, size_t cb) {
	for (size_t i = path.find(L'\\', 3); i != std::wstring::npos; i = path.find(L'\\', i + 1)) {
		CreateDirectoryW(path.substr(0, i).c_str(), nullptr);
	}
	std::wstring tmp = path + L".tmp";
	HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
	if (f == INVALID_HANDLE_VALUE) {
		return false;
	}
	DWORD wrote = 0;
	BOOL ok = WriteFile(f, data, (DWORD)cb, &wrote, nullptr) && wrote == cb && FlushFileBuffers(f);
	CloseHandle(f);
	return ok && MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
}

}  // namespace lp2net
