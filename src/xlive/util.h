#pragma once

#include "gfwl.h"

#include <string>
#include <vector>

namespace lp2net {

std::wstring Widen(const std::string& s);
std::string Narrow(const std::wstring& s);
ULONGLONG RandomU64();
void RandomBytes(void* out, size_t n);
std::string AddrToString(const sockaddr_in& a);
std::string XnAddrToString(const XNADDR& x);

// Finish an asynchronous GFWL call. With an XOVERLAPPED the call "completes" now and reports
// ERROR_IO_PENDING as the real API would; without one the error code is returned directly.
DWORD CompleteOverlapped(XOVERLAPPED* ov, DWORD error, DWORD result);

// Small data files of our own (stats, achievements). The write goes to a temp file first and then replaces the old
// one, so a crash never leaves half a file; missing folders are created.
bool ReadWholeFile(const std::wstring& path, std::vector<BYTE>* out);
bool WriteFileAtomic(const std::wstring& path, const void* data, size_t cb);

}  // namespace lp2net
