// XStorage: LIVE's title/user storage on Microsoft's servers, gone with GFWL. The game only uses it when the player
// is signed in online: it keeps an online save there (per-user facility, netSaveData00.dat, 8 KB) and lists title
// files (facility 2). Capcom's stub pops up an "Unknown Function" message box for some of these calls and the game
// retries them forever, so all of them are answered here, from folders on this PC:
//   per-user files  Documents\CAPCOM\LOST PLANET 2\<gamertag>\LP2Netplay\user\<item>   (next to the save)
//   title files     <game>\LP2Netplay\title\<item>                                      (none ship yet)
#include "config.h"
#include "gfwl.h"
#include "log.h"
#include "net.h"
#include "util.h"

#include <cwchar>
#include <string>
#include <vector>

namespace lp2net {
std::wstring ProfileDir();
}

using namespace lp2net;

namespace {
struct XSTORAGE_DOWNLOAD_TO_MEMORY_RESULTS {
	DWORD dwBytesTotal;
	XUID xuidOwner;
	FILETIME ftCreated;
};

struct XSTORAGE_ENUMERATE_RESULTS {
	DWORD dwTotalNumItems;
	DWORD dwNumItemsReturned;
	void* pItems;
};

constexpr DWORD kFacilityTitle = 2, kFacilityUser = 3;

std::string Str(LPCWSTR w) {
	return w ? Narrow(w) : "(null)";
}

std::wstring FacilityDir(DWORD facility) {
	return facility == kFacilityUser ? ProfileDir() + L"\\LP2Netplay\\user\\" : GameDir() + L"LP2Netplay\\title\\";
}

// Server paths look like "lp2net://<facility>/<owner xuid>/<item>"; only the owner's own files are kept locally.
bool LocalPath(LPCWSTR serverPath, std::wstring* out, DWORD* facility) {
	unsigned long fac = 0;
	unsigned long long owner = 0;
	int consumed = 0;
	if (!serverPath || swscanf_s(serverPath, L"lp2net://%lu/%llx/%n", &fac, &owner, &consumed) != 2 || !consumed) {
		return false;
	}
	std::wstring item = serverPath + consumed;
	if (item.empty() || item.find(L"..") != std::wstring::npos || item.find_first_of(L"\\/:") != std::wstring::npos) {
		return false;
	}
	bool mine = owner == 0 || (owner & 0x0000FFFFFFFFFFFFull) == (Me().xuid & 0x0000FFFFFFFFFFFFull);
	if (fac == kFacilityUser && !mine) {
		return false;  // other players' files are not on this PC
	}
	*facility = fac;
	*out = FacilityDir(fac) + item;
	return true;
}

void MakeDirs(const std::wstring& dir) {
	for (size_t i = dir.find(L'\\', 3); i != std::wstring::npos; i = dir.find(L'\\', i + 1)) {
		CreateDirectoryW(dir.substr(0, i).c_str(), nullptr);
	}
}

DWORD BuildPath(DWORD facility, XUID owner, LPCWSTR item, WCHAR* out, DWORD* cch) {
	if (!out || !cch) {
		return ERROR_INVALID_PARAMETER;
	}
	WCHAR path[260];
	swprintf_s(path, L"lp2net://%lu/%016llX/%ls", facility, owner, item ? item : L"");
	DWORD need = (DWORD)wcslen(path) + 1;
	if (*cch < need) {
		*cch = need;
		return ERROR_INSUFFICIENT_BUFFER;
	}
	wcscpy_s(out, *cch, path);
	*cch = need;
	return ERROR_SUCCESS;
}

DWORD Progress(XOVERLAPPED* ov, DWORD* percent, ULONGLONG* num, ULONGLONG* den) {
	if (percent) {
		*percent = 100;
	}
	if (num) {
		*num = 1;
	}
	if (den) {
		*den = 1;
	}
	return ERROR_SUCCESS;
}
}  // namespace

extern "C" DWORD WINAPI XStorageBuildServerPath(DWORD dwUserIndex, DWORD StorageFacility, const void* pvFacilityInfo,
                                                DWORD dwFacilityInfoSize, LPCWSTR wszItemName, WCHAR* pwszServerPath,
                                                DWORD* pdwServerPathLength) {
	LOG("XStorageBuildServerPath(user=%lu, facility=%lu, item='%s')", dwUserIndex, StorageFacility,
	    Str(wszItemName).c_str());
	return BuildPath(StorageFacility, 0, wszItemName, pwszServerPath, pdwServerPathLength);
}

extern "C" DWORD WINAPI XStorageBuildServerPathByXuid(XUID xuidUser, DWORD StorageFacility, const void* pvFacilityInfo,
                                                      DWORD dwFacilityInfoSize, LPCWSTR wszItemName,
                                                      WCHAR* pwszServerPath, DWORD* pdwServerPathLength) {
	LOG("XStorageBuildServerPathByXuid(%016llX, facility=%lu, item='%s')", xuidUser, StorageFacility,
	    Str(wszItemName).c_str());
	return BuildPath(StorageFacility, xuidUser, wszItemName, pwszServerPath, pdwServerPathLength);
}

extern "C" DWORD WINAPI XStorageDownloadToMemory(DWORD dwUserIndex, LPCWSTR wszServerPath, DWORD dwBufferSize,
                                                 const BYTE* pbBuffer, DWORD cbResults, void* pResults,
                                                 XOVERLAPPED* pOverlapped) {
	auto* res = (pResults && cbResults >= sizeof(XSTORAGE_DOWNLOAD_TO_MEMORY_RESULTS))
	                ? (XSTORAGE_DOWNLOAD_TO_MEMORY_RESULTS*)pResults
	                : nullptr;
	if (res) {
		memset(res, 0, sizeof(*res));
	}
	std::wstring path;
	DWORD facility = 0;
	if (!LocalPath(wszServerPath, &path, &facility)) {
		LOG("XStorageDownloadToMemory('%s'): bad path", Str(wszServerPath).c_str());
		return CompleteOverlapped(pOverlapped, ERROR_INVALID_PARAMETER, 0);
	}
	HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
	if (f == INVALID_HANDLE_VALUE) {
		if (facility == kFacilityUser) {
			// First run: the online save does not exist yet. LP2 treats any failed download as a lost connection,
			// so report an empty file and let the game start a new one (it uploads it when it saves).
			LOG("XStorageDownloadToMemory('%s'): no local copy yet, empty", Str(wszServerPath).c_str());
			if (res) {
				res->xuidOwner = Me().xuid;
				GetSystemTimeAsFileTime(&res->ftCreated);
			}
			return CompleteOverlapped(pOverlapped, ERROR_SUCCESS, 0);
		}
		LOG("XStorageDownloadToMemory('%s'): not found", Str(wszServerPath).c_str());
		return CompleteOverlapped(pOverlapped, ERROR_FILE_NOT_FOUND, 0);
	}
	DWORD size = GetFileSize(f, nullptr), got = 0;
	BOOL ok = size <= dwBufferSize && pbBuffer && ReadFile(f, (void*)pbBuffer, size, &got, nullptr) && got == size;
	FILETIME created{};
	GetFileTime(f, &created, nullptr, nullptr);
	CloseHandle(f);
	LOG("XStorageDownloadToMemory('%s'): %lu bytes%s", Str(wszServerPath).c_str(), size, ok ? "" : " (FAILED)");
	if (!ok) {
		return CompleteOverlapped(pOverlapped, size > dwBufferSize ? ERROR_INSUFFICIENT_BUFFER : ERROR_READ_FAULT, 0);
	}
	if (res) {
		res->dwBytesTotal = size;
		res->xuidOwner = Me().xuid;
		res->ftCreated = created;
	}
	return CompleteOverlapped(pOverlapped, ERROR_SUCCESS, size);
}

extern "C" DWORD WINAPI XStorageDownloadToMemoryGetProgress(XOVERLAPPED* pOverlapped, DWORD* pdwPercentComplete,
                                                            ULONGLONG* pqwNumerator, ULONGLONG* pqwDenominator) {
	return Progress(pOverlapped, pdwPercentComplete, pqwNumerator, pqwDenominator);
}

extern "C" DWORD WINAPI XStorageUploadFromMemory(DWORD dwUserIndex, LPCWSTR wszServerPath, DWORD dwBufferSize,
                                                 const BYTE* pbBuffer, XOVERLAPPED* pOverlapped) {
	std::wstring path;
	DWORD facility = 0;
	if (!LocalPath(wszServerPath, &path, &facility) || facility != kFacilityUser || (!pbBuffer && dwBufferSize)) {
		LOG("XStorageUploadFromMemory('%s', %lu bytes): refused", Str(wszServerPath).c_str(), dwBufferSize);
		return CompleteOverlapped(pOverlapped, ERROR_ACCESS_DENIED, 0);
	}
	// write a temp file, then replace, so a crash never leaves half an online save
	MakeDirs(path);
	std::wstring tmp = path + L".tmp";
	HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
	DWORD wrote = 0;
	BOOL ok = f != INVALID_HANDLE_VALUE && WriteFile(f, pbBuffer, dwBufferSize, &wrote, nullptr) &&
	          wrote == dwBufferSize && FlushFileBuffers(f);
	if (f != INVALID_HANDLE_VALUE) {
		CloseHandle(f);
	}
	ok = ok && MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
	LOG("XStorageUploadFromMemory('%s', %lu bytes) -> %ls %s", Str(wszServerPath).c_str(), dwBufferSize,
	    path.c_str(), ok ? "ok" : "FAILED");
	return CompleteOverlapped(pOverlapped, ok ? ERROR_SUCCESS : ERROR_WRITE_FAULT, 0);
}

extern "C" DWORD WINAPI XStorageUploadFromMemoryGetProgress(XOVERLAPPED* pOverlapped, DWORD* pdwPercentComplete,
                                                            ULONGLONG* pqwNumerator, ULONGLONG* pqwDenominator) {
	return Progress(pOverlapped, pdwPercentComplete, pqwNumerator, pqwDenominator);
}

// Title storage holds nothing yet; per-user enumeration is not used by LP2.
extern "C" DWORD WINAPI XStorageEnumerate(DWORD dwUserIndex, LPCWSTR wszStorageFolder, DWORD dwStartingIndex,
                                          DWORD dwMaxResultsToReturn, DWORD cbResults, void* pResults,
                                          XOVERLAPPED* pOverlapped) {
	LOG("XStorageEnumerate('%s'): empty", Str(wszStorageFolder).c_str());
	if (pResults && cbResults >= sizeof(XSTORAGE_ENUMERATE_RESULTS)) {
		memset(pResults, 0, sizeof(XSTORAGE_ENUMERATE_RESULTS));
	}
	return CompleteOverlapped(pOverlapped, ERROR_SUCCESS, 0);
}

extern "C" DWORD WINAPI XStorageDelete(DWORD dwUserIndex, LPCWSTR wszServerPath, XOVERLAPPED* pOverlapped) {
	std::wstring path;
	DWORD facility = 0;
	bool ok = LocalPath(wszServerPath, &path, &facility) && facility == kFacilityUser &&
	          (DeleteFileW(path.c_str()) || GetLastError() == ERROR_FILE_NOT_FOUND);
	LOG("XStorageDelete('%s') %s", Str(wszServerPath).c_str(), ok ? "ok" : "refused");
	return CompleteOverlapped(pOverlapped, ok ? ERROR_SUCCESS : ERROR_ACCESS_DENIED, 0);
}
