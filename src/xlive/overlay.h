#pragma once

#include "gfwl.h"

#include <string>

namespace lp2net {
void OverlayInit(const XLIVE_INITIALIZE_INFO* info);
void OverlayRender();
bool OverlayHandleMessage(const MSG* msg);  // true = swallowed, the game must not see it
void OverlayShow(bool show);
void OverlayShowAchievements();  // open on the Achievements tab
bool OverlayVisible();
void OverlayToast(const std::string& text);  // short message in the corner, shown even when closed
void OverlayPrompt(const std::string& title, const std::string& body);  // longer, titled message in the corner
void OverlayShutdown();
}  // namespace lp2net
