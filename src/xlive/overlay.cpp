// In-game "LP2 Netplay" overlay: the GFWL Guide replacement. GFWL titles already hand xlive.dll their D3D device
// (XLiveInitializeEx), call XLiveRender once per frame and route window messages through
// XLivePreTranslateMessage, so the overlay needs no D3D or window hooks of its own (apart from DX9 Reset, below).
#include "overlay.h"

#include "config.h"
#include "log.h"
#include "net.h"
#include "steam.h"
#include "xhv.h"
#include "xnotify.h"

#include <d3d11.h>
#include <d3d9.h>
#include <xinput.h>

#include <atomic>
#include <deque>
#include <mutex>

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_dx9.h"
#include "imgui_impl_win32.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace lp2net {
namespace {
enum class Api { None, D3D9, D3D11 };
Api g_api = Api::None;
IDirect3DDevice9* g_dev9 = nullptr;
ID3D11Device* g_dev11 = nullptr;
ID3D11DeviceContext* g_ctx11 = nullptr;
HWND g_hwnd = nullptr;
bool g_ready = false;
std::atomic<bool> g_visible{false};
bool g_justOpened = false;
std::recursive_mutex g_imgui;  // XLiveRender and the message pump may run on different threads

struct Toast {
	std::string title;  // empty for a one-line toast
	std::string text;
	ULONGLONG until;
};

// Controllers: holding Back opens or closes the overlay (GFWL used the Guide button, which Steam takes now). Inside
// the overlay ImGui's own XInput navigation drives it, and the game's pad input is blocked (instances.cpp).
constexpr ULONGLONG kBackHoldMs = 800;
using XInputGetState_t = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
XInputGetState_t g_padGetState = nullptr;
ULONGLONG g_padRetryAt[XUSER_MAX_COUNT] = {};  // XInputGetState on an empty slot is slow; retry those every 2 s
ULONGLONG g_backSince = 0;
bool g_backHandled = false;

void PollPadToggle() {
	static bool loaded = false;
	if (!loaded) {
		loaded = true;
		HMODULE m = LoadLibraryW(L"xinput1_3.dll");  // what the game uses
		if (!m) {
			m = LoadLibraryW(L"xinput1_4.dll");
		}
		g_padGetState = m ? (XInputGetState_t)GetProcAddress(m, "XInputGetState") : nullptr;
	}
	if (!g_padGetState) {
		return;
	}
	ULONGLONG now = GetTickCount64();
	bool back = false;
	for (DWORD i = 0; i < XUSER_MAX_COUNT; ++i) {
		if (g_padRetryAt[i] > now) {
			continue;
		}
		XINPUT_STATE st{};
		if (g_padGetState(i, &st) != ERROR_SUCCESS) {
			g_padRetryAt[i] = now + 2000;
			continue;
		}
		back |= (st.Gamepad.wButtons & XINPUT_GAMEPAD_BACK) != 0;
	}
	if (!back) {
		g_backSince = 0;
		g_backHandled = false;
		return;
	}
	if (!g_backSince) {
		g_backSince = now;
	}
	if (!g_backHandled && now - g_backSince >= kBackHoldMs) {
		g_backHandled = true;
		OverlayShow(!g_visible);
	}
}
std::mutex g_toastMutex;
std::deque<Toast> g_toasts;

char g_joinBuf[128] = "";
char g_nameBuf[16] = "";
char g_publicBuf[64] = "";

// IDirect3DDevice9::Reset is vtable slot 16. ImGui's DX9 buffers live in D3DPOOL_DEFAULT and must be released
// before a reset; XLiveOnResetDevice only arrives afterwards, so wrap Reset itself.
using ResetFn = HRESULT(WINAPI*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
ResetFn g_origReset = nullptr;

HRESULT WINAPI HookedReset(IDirect3DDevice9* dev, D3DPRESENT_PARAMETERS* pp) {
	std::lock_guard lock(g_imgui);
	if (g_ready) {
		ImGui_ImplDX9_InvalidateDeviceObjects();
	}
	HRESULT hr = g_origReset(dev, pp);
	LOG("IDirect3DDevice9::Reset -> 0x%08lX", hr);
	if (g_ready && SUCCEEDED(hr)) {
		ImGui_ImplDX9_CreateDeviceObjects();
	}
	return hr;
}

void PatchReset(IDirect3DDevice9* dev) {
	void** vtbl = *(void***)dev;
	if (vtbl[16] == (void*)&HookedReset) {
		return;
	}
	DWORD old;
	VirtualProtect(&vtbl[16], sizeof(void*), PAGE_EXECUTE_READWRITE, &old);
	g_origReset = (ResetFn)vtbl[16];
	vtbl[16] = (void*)&HookedReset;
	VirtualProtect(&vtbl[16], sizeof(void*), old, &old);
}

BOOL CALLBACK FindGameWindow(HWND hwnd, LPARAM lp) {
	DWORD pid = 0;
	GetWindowThreadProcessId(hwnd, &pid);
	if (pid == GetCurrentProcessId() && IsWindowVisible(hwnd) && !GetWindow(hwnd, GW_OWNER)) {
		*(HWND*)lp = hwnd;
		return FALSE;
	}
	return TRUE;
}

void ApplyStyle() {
	ImGuiStyle& s = ImGui::GetStyle();
	ImGui::StyleColorsDark();
	s.WindowRounding = 6.0f;
	s.FrameRounding = 4.0f;
	s.TabRounding = 4.0f;
	s.WindowPadding = ImVec2(12, 10);
	ImVec4* c = s.Colors;
	c[ImGuiCol_WindowBg] = ImVec4(0.07f, 0.08f, 0.09f, 0.94f);
	c[ImGuiCol_TitleBgActive] = ImVec4(0.55f, 0.27f, 0.07f, 1.0f);  // LP2 orange
	c[ImGuiCol_Tab] = ImVec4(0.25f, 0.16f, 0.09f, 1.0f);
	c[ImGuiCol_TabSelected] = ImVec4(0.66f, 0.33f, 0.09f, 1.0f);
	c[ImGuiCol_TabHovered] = ImVec4(0.80f, 0.42f, 0.12f, 1.0f);
	c[ImGuiCol_Button] = ImVec4(0.45f, 0.23f, 0.07f, 1.0f);
	c[ImGuiCol_ButtonHovered] = ImVec4(0.70f, 0.36f, 0.10f, 1.0f);
	c[ImGuiCol_ButtonActive] = ImVec4(0.85f, 0.45f, 0.13f, 1.0f);
	c[ImGuiCol_Header] = ImVec4(0.45f, 0.23f, 0.07f, 0.6f);
	c[ImGuiCol_HeaderHovered] = ImVec4(0.70f, 0.36f, 0.10f, 0.8f);
}

// Text sized for the game's resolution (ImGui's built-in 13 px font is unreadable on a TV at 1080p): Segoe UI at
// 1/40 of the window height, with the rest of the style scaled to match.
float g_uiScale = 1.0f;

void LoadScaledFont(ImGuiIO& io) {
	RECT rc{};
	GetClientRect(g_hwnd, &rc);
	float px = (float)(rc.bottom - rc.top) / 40.0f;
	px = px < 15.0f ? 15.0f : (px > 34.0f ? 34.0f : px);
	wchar_t dir[MAX_PATH];
	GetWindowsDirectoryW(dir, MAX_PATH);
	char path[MAX_PATH];
	snprintf(path, sizeof(path), "%ls\\Fonts\\segoeui.ttf", dir);
	g_uiScale = px / 13.0f;
	if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES || !io.Fonts->AddFontFromFileTTF(path, px)) {
		io.FontGlobalScale = g_uiScale;  // no Segoe UI: enlarge the built-in font
	}
	ImGui::GetStyle().ScaleAllSizes(g_uiScale);
	LOG("overlay: font %.0f px for a %ld px tall window", px, rc.bottom - rc.top);
}

const char* PingText(int ms) {
	static thread_local char buf[16];
	if (ms < 0) {
		return "-";
	}
	snprintf(buf, sizeof(buf), "%d ms", ms);
	return buf;
}

void DrawSessionsTab() {
	if (ImGui::Button("Refresh")) {
		RefreshSessions();
	}
	ImGui::SameLine();
	ImGui::TextDisabled("Games hosted on your network, by your peers and (online games) on Steam.");
	auto sessions = ListSessions();
	if (ImGui::BeginTable("sessions", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
		ImGui::TableSetupColumn("Host");
		ImGui::TableSetupColumn("Players");
		ImGui::TableSetupColumn("Mode");
		ImGui::TableSetupColumn("Ping");
		ImGui::TableSetupColumn("Where");
		ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 60 * g_uiScale);
		ImGui::TableHeadersRow();
		int i = 0;
		for (const auto& s : sessions) {
			ImGui::PushID(i++);
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			ImGui::TextUnformatted(s.host.c_str());
			ImGui::TableNextColumn();
			ImGui::Text("%d/%d", s.players, s.maxPlayers);
			ImGui::TableNextColumn();
			ImGui::Text("%lu/%lu", s.gameType, s.gameMode);
			ImGui::TableNextColumn();
			ImGui::TextUnformatted(PingText(s.pingMs));
			ImGui::TableNextColumn();
			ImGui::Text("%s %s", s.lan ? "LAN" : "Net", s.endpoint.c_str());
			ImGui::TableNextColumn();
			if (ImGui::SmallButton("Join")) {
				if (s.steamLobby) {
					SteamJoinLobbyById(s.steamLobby);  // same path as a Steam invite
					OverlayShow(false);
				}
				else {
					AddManualPeer(s.endpoint);
					OverlayToast("Added " + s.host + ". Open System Link to join their game.");
				}
			}
			ImGui::PopID();
		}
		ImGui::EndTable();
	}
	if (sessions.empty()) {
		ImGui::TextDisabled("No games found yet. Ask the host for their IP and use Join by IP.");
	}
}

void DrawSteamTab() {
	if (!SteamReady()) {
		ImGui::TextWrapped("Steam is not connected. Start Lost Planet 2 from Steam to see your friends here, invite "
		                   "them and join their games.");
		return;
	}
	ImGui::Text("Signed in to Steam as %s", SteamMyName().c_str());
	if (ImGui::Button("Invite friends")) {
		if (!SteamInviteDialog()) {
			OverlayToast(SteamHostedLobby() ? "The Steam overlay is not available."
			                                : "Host a game first (Create Game / Create Match), then invite.");
		}
	}
	ImGui::SameLine();
	if (ImGui::Button("Steam friends list") && !SteamOverlay("Friends")) {
		OverlayToast("The Steam overlay is not available (start the game from Steam).");
	}
	ImGui::Separator();
	ImGui::TextDisabled("Friends playing Lost Planet 2:");
	auto friends = SteamFriendsInGame();
	if (ImGui::BeginTable("friends", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
		ImGui::TableSetupColumn("Friend");
		ImGui::TableSetupColumn("Game");
		ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 70 * g_uiScale);
		ImGui::TableHeadersRow();
		int i = 0;
		for (const auto& f : friends) {
			ImGui::PushID(i++);
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			ImGui::TextUnformatted(f.name.c_str());
			ImGui::TableNextColumn();
			ImGui::TextUnformatted(f.lobby ? "hosting or in a game" : "in the menus");
			ImGui::TableNextColumn();
			if (f.lobby && ImGui::SmallButton("Join")) {
				SteamJoinLobbyById(f.lobby);
				OverlayShow(false);
			}
			ImGui::PopID();
		}
		ImGui::EndTable();
	}
	if (friends.empty()) {
		ImGui::TextDisabled("None right now. Friends who host a game can also invite you through Steam.");
	}
}

void DrawJoinTab() {
	ImGui::TextWrapped("Enter the host's IP address (and port if they gave you one). The game's System Link "
	                   "search will then also find games at that address.");
	ImGui::SetNextItemWidth(260 * g_uiScale);
	bool enter = ImGui::InputTextWithHint("##join", "203.0.113.7 or host.example.net:3075", g_joinBuf,
	                                      sizeof(g_joinBuf), ImGuiInputTextFlags_EnterReturnsTrue);
	ImGui::SameLine();
	if ((ImGui::Button("Add peer") || enter) && g_joinBuf[0]) {
		AddManualPeer(g_joinBuf);
		OverlayToast(std::string("Peer added: ") + g_joinBuf);
		g_joinBuf[0] = 0;
	}
	ImGui::Separator();
	auto peers = ListPeers();
	if (ImGui::BeginTable("peers", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
		ImGui::TableSetupColumn("Name");
		ImGui::TableSetupColumn("Address");
		ImGui::TableSetupColumn("Ping");
		ImGui::TableSetupColumn("Status");
		ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 70 * g_uiScale);
		ImGui::TableHeadersRow();
		int i = 0;
		for (const auto& p : peers) {
			ImGui::PushID(i++);
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			ImGui::TextUnformatted(p.name.empty() ? "?" : p.name.c_str());
			ImGui::TableNextColumn();
			ImGui::TextUnformatted(p.endpoint.c_str());
			ImGui::TableNextColumn();
			ImGui::TextUnformatted(PingText(p.pingMs));
			ImGui::TableNextColumn();
			ImGui::TextColored(p.alive ? ImVec4(0.4f, 0.9f, 0.4f, 1) : ImVec4(0.8f, 0.6f, 0.3f, 1), "%s",
			                   p.alive ? "connected" : "waiting");
			ImGui::TableNextColumn();
			if (p.manual && ImGui::SmallButton("Remove")) {
				RemoveManualPeer(p.endpoint);
			}
			ImGui::PopID();
		}
		ImGui::EndTable();
	}
}

struct PttKey {
	UINT vk;
	const char* name;
};
const PttKey kPttKeys[] = {{0, "Off (open microphone)"}, {VK_CAPITAL, "Caps Lock"}, {'V', "V"}, {'T', "T"},
                           {'B', "B"}, {VK_XBUTTON1, "Mouse button 4"}, {VK_XBUTTON2, "Mouse button 5"}};

void DrawVoiceTab() {
	Config& c = Cfg();
	VoiceView v = VoiceStatus();
	bool changed = ImGui::Checkbox("Voice chat", &c.voice);
	ImGui::SameLine();
	ImGui::TextDisabled(v.engine ? "" : "(the game has not started its voice engine yet)");
	char other[32];
	snprintf(other, sizeof(other), "Key code %u", c.pushToTalk);
	const char* current = other;
	for (const auto& k : kPttKeys) {
		if (k.vk == c.pushToTalk) {
			current = k.name;
		}
	}
	ImGui::SetNextItemWidth(220 * g_uiScale);
	if (ImGui::BeginCombo("Push to talk", current)) {
		for (const auto& k : kPttKeys) {
			if (ImGui::Selectable(k.name, k.vk == c.pushToTalk)) {
				c.pushToTalk = k.vk;
				changed = true;
			}
		}
		ImGui::EndCombo();
	}
	ImGui::SetNextItemWidth(220 * g_uiScale);
	changed |= ImGui::SliderInt("Volume", &c.voiceVolume, 0, 200, "%d%%");
	changed |= ImGui::Checkbox("Use the microphone set in Steam (when Steam runs)", &c.voiceSteamMic);
	if (changed) {
		SaveConfig();
	}
	bool test = v.micTest;
	if (ImGui::Checkbox("Microphone test (hear yourself)", &test)) {
		VoiceSetMicTest(test);
	}
	ImGui::Text("Microphone: %s", v.active ? v.source.c_str() : "closed (opens in a game with other players)");
	if (v.talking) {
		ImGui::SameLine();
		ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1), "talking");
	}
	ImGui::Separator();
	if (ImGui::BeginTable("talkers", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
		ImGui::TableSetupColumn("Player");
		ImGui::TableSetupColumn("Voice");
		ImGui::TableSetupColumn("Mute", ImGuiTableColumnFlags_WidthFixed, 60 * g_uiScale);
		ImGui::TableHeadersRow();
		int i = 0;
		for (const auto& t : v.talkers) {
			ImGui::PushID(i++);
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			ImGui::TextUnformatted(t.name.c_str());
			ImGui::TableNextColumn();
			if (t.talking) {
				ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1), "talking");
			}
			else {
				ImGui::TextDisabled("%s", t.gameMuted && !t.muted ? "muted by the game"
				                          : t.reachable          ? "quiet"
				                                                 : "no voice yet");
			}
			ImGui::TableNextColumn();
			bool muted = t.muted;
			if (ImGui::Checkbox("##mute", &muted)) {
				VoiceSetMuted(t.xuid, muted);
			}
			ImGui::PopID();
		}
		ImGui::EndTable();
	}
	if (v.talkers.empty()) {
		ImGui::TextDisabled("Players you can talk to appear here once you are in a game or lobby together.");
	}
	ImGui::TextDisabled("%s", v.counters.c_str());
}

void DrawSettingsTab() {
	Config& c = Cfg();
	if (!g_nameBuf[0]) {
		strncpy_s(g_nameBuf, c.name.c_str(), _TRUNCATE);
		strncpy_s(g_publicBuf, c.publicAddress.c_str(), _TRUNCATE);
	}
	ImGui::SetNextItemWidth(200 * g_uiScale);
	ImGui::InputText("Gamertag", g_nameBuf, sizeof(g_nameBuf));
	ImGui::SetNextItemWidth(200 * g_uiScale);
	ImGui::InputTextWithHint("Public address", "auto", g_publicBuf, sizeof(g_publicBuf));
	ImGui::Checkbox("Open ports with UPnP", &c.upnp);
	ImGui::Checkbox("Log every packet (debugging)", &c.logPackets);
	if (ImGui::Button("Save")) {
		c.name = g_nameBuf;
		c.publicAddress = g_publicBuf;
		SaveConfig();
		OverlayToast("Saved. A new gamertag applies the next time you start the game.");
	}
	ImGui::Separator();
	ImGui::TextDisabled("Settings file: %ls", (GameDir() + L"lp2net.ini").c_str());
}

void DrawHelpTab() {
	ImGui::TextWrapped(
	    "Same network: both players pick System Link in the game. One hosts, the other searches.\n\n"
	    "Over the internet: the host may need to forward UDP ports (shown in the header) on their router, or "
	    "enable UPnP. The joining player adds the host's address under Join by IP, then searches System Link.\n\n"
	    "Press %s to open or close this window. On a controller, hold Back to open or close it; B also closes it.",
	    Cfg().overlayKey == VK_HOME ? "Home" : "the overlay key");
}

void DrawWindow() {
	bool open = true;
	ImGuiIO& io = ImGui::GetIO();
	ImGui::SetNextWindowSize(ImVec2(660 * g_uiScale, 430 * g_uiScale), ImGuiCond_FirstUseEver);
	ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_FirstUseEver,
	                        ImVec2(0.5f, 0.5f));
	if (g_justOpened) {
		ImGui::SetNextWindowFocus();  // so keyboard and controller navigation start in the overlay
		g_justOpened = false;
	}
	if (ImGui::Begin("LP2 Netplay", &open, ImGuiWindowFlags_NoCollapse)) {
		// B on a controller closes the overlay unless it is editing a field
		if (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) && !ImGui::IsAnyItemActive()) {
			open = false;
		}
		ImGui::TextUnformatted(NetStatusLine().c_str());
		ImGui::Separator();
		if (ImGui::BeginTabBar("tabs")) {
			if (ImGui::BeginTabItem("Sessions")) {
				DrawSessionsTab();
				ImGui::EndTabItem();
			}
			if (ImGui::BeginTabItem("Steam")) {
				DrawSteamTab();
				ImGui::EndTabItem();
			}
			if (ImGui::BeginTabItem("Join by IP")) {
				DrawJoinTab();
				ImGui::EndTabItem();
			}
			if (ImGui::BeginTabItem("Voice")) {
				DrawVoiceTab();
				ImGui::EndTabItem();
			}
			if (ImGui::BeginTabItem("Settings")) {
				DrawSettingsTab();
				ImGui::EndTabItem();
			}
			if (ImGui::BeginTabItem("Help")) {
				DrawHelpTab();
				ImGui::EndTabItem();
			}
			ImGui::EndTabBar();
		}
	}
	ImGui::End();
	if (!open) {
		OverlayShow(false);
	}
}

bool DrawToasts() {
	std::lock_guard lock(g_toastMutex);
	ULONGLONG now = GetTickCount64();
	while (!g_toasts.empty() && g_toasts.front().until < now) {
		g_toasts.pop_front();
	}
	if (g_toasts.empty()) {
		return false;
	}
	ImGuiIO& io = ImGui::GetIO();
	ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - 16, 16), ImGuiCond_Always, ImVec2(1, 0));
	ImGui::SetNextWindowBgAlpha(0.75f);
	ImGui::Begin("##toasts", nullptr,
	             ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
	                 ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
	bool first = true;
	for (const auto& t : g_toasts) {
		if (!first) {
			ImGui::Separator();
		}
		first = false;
		if (!t.title.empty()) {
			ImGui::TextColored(ImVec4(0.95f, 0.55f, 0.18f, 1.0f), "%s", t.title.c_str());
		}
		ImGui::TextUnformatted(t.text.c_str());
	}
	ImGui::End();
	return true;
}

bool HasToasts() {
	std::lock_guard lock(g_toastMutex);
	return !g_toasts.empty();
}
}  // namespace

void OverlayInit(const XLIVE_INITIALIZE_INFO* info) {
	std::lock_guard lock(g_imgui);
	if (g_ready || !info || !info->pD3D) {
		LOG("overlay: %s", g_ready ? "already initialised" : "no D3D device from the game");
		return;
	}
	if (SUCCEEDED(info->pD3D->QueryInterface(__uuidof(IDirect3DDevice9), (void**)&g_dev9))) {
		g_api = Api::D3D9;
		auto* pp = (D3DPRESENT_PARAMETERS*)info->pD3DPP;
		D3DDEVICE_CREATION_PARAMETERS cp{};
		g_dev9->GetCreationParameters(&cp);
		g_hwnd = (pp && pp->hDeviceWindow) ? pp->hDeviceWindow : cp.hFocusWindow;
	}
	else if (SUCCEEDED(info->pD3D->QueryInterface(__uuidof(ID3D11Device), (void**)&g_dev11))) {
		g_api = Api::D3D11;
		g_dev11->GetImmediateContext(&g_ctx11);
		auto* sd = (DXGI_SWAP_CHAIN_DESC*)info->pD3DPP;
		g_hwnd = sd ? sd->OutputWindow : nullptr;
	}
	else {
		LOG("overlay: unknown D3D interface %p", info->pD3D);
		return;
	}
	if (!g_hwnd) {
		EnumWindows(FindGameWindow, (LPARAM)&g_hwnd);
	}
	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGuiIO& io = ImGui::GetIO();
	io.IniFilename = nullptr;
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
	ApplyStyle();
	LoadScaledFont(io);
	ImGui_ImplWin32_Init(g_hwnd);
	if (g_api == Api::D3D9) {
		ImGui_ImplDX9_Init(g_dev9);
		PatchReset(g_dev9);
	}
	else {
		ImGui_ImplDX11_Init(g_dev11, g_ctx11);
	}
	g_ready = true;
	LOG("overlay: ready (%s, hwnd=%p)", g_api == Api::D3D9 ? "D3D9" : "D3D11", g_hwnd);
}

void OverlayRender() {
	if (g_ready) {
		PollPadToggle();
	}
	if (!g_ready || (!g_visible && !HasToasts())) {
		return;
	}
	std::lock_guard lock(g_imgui);
	if (g_api == Api::D3D9) {
		ImGui_ImplDX9_NewFrame();
	}
	else {
		ImGui_ImplDX11_NewFrame();
	}
	ImGui_ImplWin32_NewFrame();
	ImGui::NewFrame();
	ImGui::GetIO().MouseDrawCursor = g_visible;
	DrawToasts();
	if (g_visible) {
		DrawWindow();
	}
	ImGui::Render();
	if (g_api == Api::D3D9) {
		ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
		return;
	}
	ID3D11RenderTargetView* rtv = nullptr;
	ID3D11DepthStencilView* dsv = nullptr;
	g_ctx11->OMGetRenderTargets(1, &rtv, &dsv);
	if (rtv) {
		g_ctx11->OMSetRenderTargets(1, &rtv, nullptr);
		ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
		g_ctx11->OMSetRenderTargets(1, &rtv, dsv);
		rtv->Release();
	}
	if (dsv) {
		dsv->Release();
	}
}

bool OverlayHandleMessage(const MSG* msg) {
	if (!g_ready || !msg) {
		return false;
	}
	bool firstPress = !(msg->lParam & (1 << 30));
	if (msg->message == WM_KEYDOWN && firstPress &&
	    (msg->wParam == Cfg().overlayKey || (g_visible && msg->wParam == VK_ESCAPE))) {
		OverlayShow(!g_visible);
		return true;
	}
	if (!g_visible) {
		return false;
	}
	{
		std::lock_guard lock(g_imgui);
		ImGui_ImplWin32_WndProcHandler(msg->hwnd, msg->message, msg->wParam, msg->lParam);
	}
	switch (msg->message) {
	case WM_KEYDOWN: case WM_KEYUP: case WM_SYSKEYDOWN: case WM_SYSKEYUP: case WM_CHAR:
	case WM_MOUSEMOVE: case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
	case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_MBUTTONDOWN: case WM_MBUTTONUP:
	case WM_MOUSEWHEEL: case WM_INPUT:
		return true;
	default:
		return false;
	}
}

void OverlayShow(bool show) {
	if (g_visible.exchange(show) == show) {
		return;
	}
	LOG("overlay: %s", show ? "open" : "closed");
	if (show) {
		g_justOpened = true;
		RefreshSessions();
	}
	PostNotification(XN_SYS_UI, show ? TRUE : FALSE);  // GFWL titles pause their own input while the guide is up
}

bool OverlayVisible() {
	return g_visible;
}

void OverlayToast(const std::string& text) {
	LOG("toast: %s", text.c_str());
	std::lock_guard lock(g_toastMutex);
	g_toasts.push_back({"", text, GetTickCount64() + 6000});
	while (g_toasts.size() > 5) {
		g_toasts.pop_front();
	}
}

void OverlayPrompt(const std::string& title, const std::string& body) {
	LOG("prompt: %s: %s", title.c_str(), body.c_str());
	std::lock_guard lock(g_toastMutex);
	g_toasts.push_back({title, body, GetTickCount64() + 12000});
	while (g_toasts.size() > 5) {
		g_toasts.pop_front();
	}
}

void OverlayShutdown() {
	std::lock_guard lock(g_imgui);
	if (!g_ready) {
		return;
	}
	g_ready = false;
	if (g_api == Api::D3D9) {
		ImGui_ImplDX9_Shutdown();
	}
	else {
		ImGui_ImplDX11_Shutdown();
	}
	ImGui_ImplWin32_Shutdown();
	ImGui::DestroyContext();
}

}  // namespace lp2net
