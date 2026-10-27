// GFWL (xlive.dll) types and constants used by Lost Planet 2.
// Layouts follow the public GFWL 3.x SDK headers; all API functions are __stdcall.
#pragma once

#include <winsock2.h>
#include <windows.h>
#include <unknwn.h>

#include <cstdint>

using XUID = ULONGLONG;

// GFWL structures use natural alignment: XUSER_DATA is 16 bytes (8-aligned union), XUSER_PROPERTY 24 (checked
// against the search parameters LP2 passes to XSessionSearchEx).
#pragma pack(push, 8)

struct XNADDR {
	IN_ADDR ina;        // LAN address
	IN_ADDR inaOnline;  // public address
	WORD wPortOnline;   // public port, network byte order
	BYTE abEnet[6];     // "MAC" - we store our instance id here
	BYTE abOnline[20];  // opaque online identification
};
static_assert(sizeof(XNADDR) == 36);

struct XNKID {
	BYTE ab[8];
};
struct XNKEY {
	BYTE ab[16];
};

constexpr BYTE XNET_XNKID_MASK = 0xF0;
constexpr BYTE XNET_XNKID_SYSTEM_LINK = 0x00;
constexpr BYTE XNET_XNKID_ONLINE_PEER = 0x80;

struct XSESSION_INFO {
	XNKID sessionID;
	XNADDR hostAddress;
	XNKEY keyExchangeKey;
};
static_assert(sizeof(XSESSION_INFO) == 60);

struct XOVERLAPPED;
using PXOVERLAPPED_COMPLETION_ROUTINE = void(WINAPI*)(DWORD dwErrorCode, DWORD dwNumberOfBytesTransfered,
                                                      XOVERLAPPED* pOverlapped);
struct XOVERLAPPED {
	ULONG_PTR InternalLow;
	ULONG_PTR InternalHigh;
	ULONG_PTR InternalContext;
	HANDLE hEvent;
	PXOVERLAPPED_COMPLETION_ROUTINE pCompletionRoutine;
	DWORD_PTR dwCompletionContext;
	DWORD dwExtendedError;
};
static_assert(sizeof(XOVERLAPPED) == 28);

enum XUSER_SIGNIN_STATE : DWORD {
	eXUserSigninState_NotSignedIn = 0,
	eXUserSigninState_SignedInLocally = 1,
	eXUserSigninState_SignedInToLive = 2,
};

constexpr DWORD XUSER_GET_SIGNIN_INFO_ONLINE_XUID_ONLY = 0x1;
constexpr DWORD XUSER_GET_SIGNIN_INFO_OFFLINE_XUID_ONLY = 0x2;
constexpr DWORD XUSER_INFO_FLAG_LIVE_ENABLED = 0x1;
constexpr DWORD XUSER_INFO_FLAG_GUEST = 0x2;
constexpr DWORD XUSER_NAME_SIZE = 16;
constexpr DWORD XUSER_MAX_COUNT = 4;
constexpr DWORD XUSER_INDEX_NONE = 0xFE;

struct XUSER_SIGNIN_INFO {
	XUID xuid;
	DWORD dwInfoFlags;
	XUSER_SIGNIN_STATE UserSigninState;
	DWORD dwGuestNumber;
	DWORD dwSponsorUserIndex;
	CHAR szUserName[XUSER_NAME_SIZE];
};
static_assert(sizeof(XUSER_SIGNIN_INFO) == 40);

struct XUSER_DATA {
	BYTE type;
	union {
		LONG nData;
		LONGLONG i64Data;
		double dblData;
		struct {
			DWORD cbData;
			LPWSTR pwszData;
		} string;
		float fData;
		struct {
			DWORD cbData;
			PBYTE pbData;
		} binary;
		FILETIME ftData;
	};
};

struct XUSER_PROPERTY {
	DWORD dwPropertyId;
	XUSER_DATA value;
};
static_assert(sizeof(XUSER_DATA) == 16 && sizeof(XUSER_PROPERTY) == 24);

struct XUSER_CONTEXT {
	DWORD dwContextId;
	DWORD dwValue;
};

enum XSESSION_STATE : DWORD {
	XSESSION_STATE_LOBBY = 0,
	XSESSION_STATE_REGISTRATION,
	XSESSION_STATE_INGAME,
	XSESSION_STATE_REPORTING,
	XSESSION_STATE_DELETED,
};

struct XSESSION_MEMBER {
	XUID xuidOnline;
	DWORD dwUserIndex;
	DWORD dwFlags;
};
constexpr DWORD XSESSION_MEMBER_FLAGS_PRIVATE_SLOT = 0x01;
constexpr DWORD XSESSION_MEMBER_FLAGS_ZOMBIE = 0x02;

struct XSESSION_LOCAL_DETAILS {
	DWORD dwUserIndexHost;
	DWORD dwGameType;
	DWORD dwGameMode;
	DWORD dwFlags;
	DWORD dwMaxPublicSlots;
	DWORD dwMaxPrivateSlots;
	DWORD dwAvailablePublicSlots;
	DWORD dwAvailablePrivateSlots;
	DWORD dwActualMemberCount;
	DWORD dwReturnedMemberCount;
	XSESSION_STATE eState;
	ULONGLONG qwNonce;
	XSESSION_INFO sessionInfo;
	XNKID xnkidArbitration;
	XSESSION_MEMBER* pSessionMembers;
};

struct XSESSION_SEARCHRESULT {
	XSESSION_INFO info;
	DWORD dwOpenPublicSlots;
	DWORD dwOpenPrivateSlots;
	DWORD dwFilledPublicSlots;
	DWORD dwFilledPrivateSlots;
	DWORD cProperties;
	DWORD cContexts;
	XUSER_PROPERTY* pProperties;
	XUSER_CONTEXT* pContexts;
};

struct XSESSION_SEARCHRESULT_HEADER {
	DWORD dwSearchResults;
	XSESSION_SEARCHRESULT* pResults;
};

constexpr DWORD XSESSION_CREATE_HOST = 0x00000001;
constexpr DWORD XSESSION_CREATE_USES_PRESENCE = 0x00000002;
constexpr DWORD XSESSION_CREATE_USES_STATS = 0x00000004;
constexpr DWORD XSESSION_CREATE_USES_MATCHMAKING = 0x00000008;
constexpr DWORD XSESSION_CREATE_USES_ARBITRATION = 0x00000010;
constexpr DWORD XSESSION_CREATE_USES_PEER_NETWORK = 0x00000020;
constexpr DWORD XSESSION_CREATE_MODIFIERS_MASK = 0x00000F80;

// XUser context / property ids the session layer cares about
constexpr DWORD X_CONTEXT_PRESENCE = 0x00008001;
constexpr DWORD X_CONTEXT_GAME_TYPE = 0x0000800A;
constexpr DWORD X_CONTEXT_GAME_MODE = 0x0000800B;
constexpr DWORD X_CONTEXT_GAME_TYPE_RANKED = 0;
constexpr DWORD X_CONTEXT_GAME_TYPE_STANDARD = 1;

struct XNQOSINFO {
	BYTE bFlags;
	BYTE bReserved;
	WORD cProbesXmit;
	WORD cProbesRecv;
	WORD cbData;
	BYTE* pbData;
	WORD wRttMinInMsecs;
	WORD wRttMedInMsecs;
	DWORD dwUpBitsPerSec;
	DWORD dwDnBitsPerSec;
};
static_assert(sizeof(XNQOSINFO) == 24);

struct XNQOS {
	UINT cxnqos;
	UINT cxnqosPending;
	XNQOSINFO axnqosinfo[1];
};

constexpr BYTE XNET_XNQOSINFO_COMPLETE = 0x01;
constexpr BYTE XNET_XNQOSINFO_TARGET_CONTACTED = 0x02;
constexpr BYTE XNET_XNQOSINFO_TARGET_DISABLED = 0x04;
constexpr BYTE XNET_XNQOSINFO_DATA_RECEIVED = 0x08;
constexpr BYTE XNET_XNQOSINFO_PARTIAL_COMPLETE = 0x10;

constexpr DWORD XNET_QOS_LISTEN_ENABLE = 0x01;
constexpr DWORD XNET_QOS_LISTEN_DISABLE = 0x02;
constexpr DWORD XNET_QOS_LISTEN_SET_DATA = 0x04;
constexpr DWORD XNET_QOS_LISTEN_SET_BITSPERSEC = 0x08;
constexpr DWORD XNET_QOS_LISTEN_RELEASE = 0x10;

struct XNDNS {
	INT iStatus;
	UINT cina;
	IN_ADDR aina[8];
};

constexpr DWORD XNET_GET_XNADDR_PENDING = 0x0000;
constexpr DWORD XNET_GET_XNADDR_NONE = 0x0001;
constexpr DWORD XNET_GET_XNADDR_ETHERNET = 0x0002;
constexpr DWORD XNET_GET_XNADDR_STATIC = 0x0004;
constexpr DWORD XNET_GET_XNADDR_DHCP = 0x0008;
constexpr DWORD XNET_GET_XNADDR_GATEWAY = 0x0020;
constexpr DWORD XNET_GET_XNADDR_DNS = 0x0040;
constexpr DWORD XNET_GET_XNADDR_ONLINE = 0x0080;

constexpr DWORD XNET_ETHERNET_LINK_ACTIVE = 0x01;
constexpr DWORD XNET_ETHERNET_LINK_100MBPS = 0x02;
constexpr DWORD XNET_ETHERNET_LINK_FULL_DUPLEX = 0x08;

constexpr DWORD XNET_CONNECT_STATUS_IDLE = 0;
constexpr DWORD XNET_CONNECT_STATUS_PENDING = 1;
constexpr DWORD XNET_CONNECT_STATUS_CONNECTED = 2;
constexpr DWORD XNET_CONNECT_STATUS_LOST = 3;

constexpr DWORD XNET_BROADCAST_VERSION_OLDER = 0x01;
constexpr DWORD XNET_BROADCAST_VERSION_NEWER = 0x02;

constexpr int IPPROTO_VDP = 254;

struct XLIVE_INITIALIZE_INFO {
	UINT cbSize;
	DWORD dwFlags;
	IUnknown* pD3D;
	void* pD3DPP;
	LANGID langID;
	WORD wReserved1;
	PCHAR pszAdapterName;
	WORD wLivePortOverride;
	WORD wReserved2;
};

// Notifications: XNID(version, area, index)
constexpr DWORD XNID(DWORD version, DWORD area, DWORD index) {
	return (area << 25) | (version << 16) | index;
}
constexpr ULONGLONG XNOTIFY_SYSTEM = 0x00000001;
constexpr ULONGLONG XNOTIFY_LIVE = 0x00000002;
constexpr ULONGLONG XNOTIFY_FRIENDS = 0x00000004;
constexpr DWORD XN_SYS_UI = XNID(0, 0, 0x0009);
constexpr DWORD XN_SYS_SIGNINCHANGED = XNID(0, 0, 0x000A);
constexpr DWORD XN_SYS_INPUTDEVICESCHANGED = XNID(0, 0, 0x0012);
constexpr DWORD XN_LIVE_CONNECTIONCHANGED = XNID(0, 1, 0x0001);
constexpr DWORD XN_LIVE_INVITE_ACCEPTED = XNID(0, 1, 0x0002);
constexpr DWORD XN_LIVE_LINK_STATE_CHANGED = XNID(0, 1, 0x0003);

constexpr HRESULT XONLINE_S_LOGON_CONNECTION_ESTABLISHED = 0x001510F0;
constexpr HRESULT XONLINE_E_LOGON_NOT_LOGGED_ON = 0x80151802;

#pragma pack(pop)
