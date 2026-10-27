#pragma once

#include "gfwl.h"

#include <string>

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

// A section of the game exe in memory (".text", ".rdata", ...).
bool FindSection(const char* name, BYTE** start, size_t* size);

}  // namespace lp2net
