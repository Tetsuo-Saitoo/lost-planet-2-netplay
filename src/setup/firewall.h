// Windows Firewall and the game: a read-only status for the window, and the fix that runs elevated only after the
// player clicks "Allow through firewall" and accepts the Windows (UAC) prompt.
#pragma once

#include <string>
#include <vector>

namespace lp2setup {

enum class Firewall { Allowed, Blocked, NoRule, Unknown };

Firewall FirewallStatus(const std::wstring& dir, std::vector<std::wstring>* blockingRules = nullptr);
std::wstring FirewallText(Firewall state, const std::vector<std::wstring>& blockingRules);

// Turns off inbound rules that block LP2DX9.exe / LP2DX11.exe and adds "Lost Planet 2 (LP2 Netplay) <exe>" rules
// (inbound UDP, private and public networks; the names tools\package\install.ps1 -FixFirewall uses). Needs
// administrator rights. With apply = false it changes nothing and only reports what it would do.
bool FirewallAllow(const std::wstring& dir, bool apply, std::vector<std::wstring>& report);

}  // namespace lp2setup
