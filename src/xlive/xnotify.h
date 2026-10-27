#pragma once

#include "gfwl.h"

namespace lp2net {
// Queue a notification for every listener subscribed to its area.
void PostNotification(DWORD id, ULONG_PTR param);
}  // namespace lp2net
