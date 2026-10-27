// LIVE matchmaking without the LIVE servers: hosts publish their session attributes (the user properties and
// contexts the game set, plus what LIVE itself added for the host) with every session announcement and Steam
// lobby; XSessionSearch runs the game's own query (filters from its SPA) over every session we know of.
#pragma once

#include "gfwl.h"

#include <string>
#include <vector>

namespace lp2net {

struct SessionAttr {
	DWORD id = 0;
	BYTE type = 0;  // XUSER_DATA_TYPE_*: 0 context, 1 int32, 2 int64, 3 double, 4 unicode, 5 float, 6 binary, 7 time
	std::vector<BYTE> value;  // raw value; unicode = UTF-16 including the terminator
};
using SessionAttrs = std::vector<SessionAttr>;

SessionAttrs HostAttributes();
std::vector<BYTE> PackAttrs(const SessionAttrs& attrs);
bool UnpackAttrs(const BYTE* data, size_t cb, SessionAttrs* out);
std::string ToHex(const void* data, size_t cb);
bool FromHex(const std::string& hex, std::vector<BYTE>* out);

// XSessionSearch, XSessionSearchEx (byId == nullptr) and XSessionSearchByID.
DWORD SessionSearch(DWORD procedure, DWORD maxResults, WORD numProps, const XUSER_PROPERTY* props, WORD numContexts,
                    const XUSER_CONTEXT* contexts, const XNKID* byId, DWORD* pcbResults,
                    XSESSION_SEARCHRESULT_HEADER* results, XOVERLAPPED* overlapped);
void MatchmakingTick();  // net worker thread: finishes searches once answers are in

}  // namespace lp2net
