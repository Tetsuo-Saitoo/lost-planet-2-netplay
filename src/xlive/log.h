#pragma once

namespace lp2net {
void LogOpen(const wchar_t* path);
void Log(const char* fmt, ...);
}  // namespace lp2net

#define LOG(...) ::lp2net::Log(__VA_ARGS__)
