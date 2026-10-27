// LP2NetSetup.exe: installs, repairs and uninstalls LP2 Netplay on a PC with Lost Planet 2 (Steam). One file for
// players: our xlive.dll is inside it as a resource. Double-click for the window; the command line is for tests and
// scripts (see kUsage). Exit code 0 = done, 1 = refused or failed, 2 = bad command line.
#include "firewall.h"
#include "lp2net_version.h"
#include "resource.h"
#include "setup.h"

#include <commctrl.h>
#include <objbase.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <wrl/client.h>

using namespace lp2setup;
using Microsoft::WRL::ComPtr;

namespace {
const wchar_t kTitle[] = L"LP2 Netplay Setup";
const wchar_t kUsage[] =
    L"LP2NetSetup                       open the window\r\n"
    L"LP2NetSetup --status              show what is installed\r\n"
    L"LP2NetSetup --install [--name NAME] [--peer ADDRESS] [--cleanup]\r\n"
    L"                                  install, update or repair (--repair is the same)\r\n"
    L"LP2NetSetup --uninstall           put Capcom's file and any moved GFWL files back\r\n"
    L"LP2NetSetup --firewall [--dry-run]   allow the game through Windows Firewall (as administrator)\r\n"
    L"  --game-dir DIR                  use this game folder instead of looking for it";

enum class Action { Window, Status, Install, Uninstall, Firewall, Help };

struct Args {
	Action action = Action::Window;
	std::wstring gameDir;
	Settings settings;
	bool cleanup = false;
	bool dryRun = false;
	bool bad = false;
};

Args ParseArgs() {
	Args a;
	int argc = 0;
	LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
	for (int i = 1; argv && i < argc; ++i) {
		std::wstring s = argv[i];
		bool value = i + 1 < argc;
		if (s == L"--status") {
			a.action = Action::Status;
		} else if (s == L"--install" || s == L"--repair") {
			a.action = Action::Install;
		} else if (s == L"--uninstall") {
			a.action = Action::Uninstall;
		} else if (s == L"--firewall") {
			a.action = Action::Firewall;
		} else if (s == L"--help" || s == L"-h" || s == L"/?") {
			a.action = Action::Help;
		} else if (s == L"--cleanup") {
			a.cleanup = true;
		} else if (s == L"--dry-run") {
			a.dryRun = true;
		} else if (s == L"--game-dir" && value) {
			a.gameDir = argv[++i];
		} else if (s == L"--name" && value) {
			a.settings.name = argv[++i];
		} else if (s == L"--peer" && value) {
			a.settings.peers = argv[++i];
		} else {
			a.bad = true;
		}
	}
	LocalFree(argv);
	return a;
}

// A Windows (not console) program: command-line output goes to the stdout the caller handed over (a pipe or a
// file), else to the console it was started from.
void Print(const std::wstring& text) {
	static HANDLE out = [] {
		HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
		if ((!h || h == INVALID_HANDLE_VALUE) && AttachConsole(ATTACH_PARENT_PROCESS)) {
			h = CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0,
			                nullptr);
		}
		return h;
	}();
	if (!out || out == INVALID_HANDLE_VALUE) {
		return;
	}
	std::wstring line = text + L"\r\n";
	DWORD n = 0;
	DWORD mode = 0;
	if (GetConsoleMode(out, &mode)) {
		WriteConsoleW(out, line.data(), (DWORD)line.size(), &n, nullptr);
		return;
	}
	int len = WideCharToMultiByte(CP_UTF8, 0, line.data(), (int)line.size(), nullptr, 0, nullptr, nullptr);
	std::string utf8(len, '\0');
	WideCharToMultiByte(CP_UTF8, 0, line.data(), (int)line.size(), utf8.data(), len, nullptr, nullptr);
	WriteFile(out, utf8.data(), (DWORD)utf8.size(), &n, nullptr);
}

int PrintResult(const Result& r) {
	Print((r.ok ? L"OK: " : L"FAILED: ") + r.summary);
	for (const std::wstring& d : r.details) {
		Print(L"  " + d);
	}
	return r.ok ? 0 : 1;
}

int RunCommandLine(const Args& a) {
	if (a.bad || a.action == Action::Help) {
		Print(L"LP2 Netplay Setup " LP2NET_VERSION L"\r\n" + std::wstring(kUsage));
		return a.bad ? 2 : 0;
	}
	std::wstring dir = a.gameDir.empty() ? FindGameDir() : NormalizeDir(a.gameDir);
	Print(L"LP2 Netplay Setup " LP2NET_VERSION);
	Print(L"Game folder: " + (dir.empty() ? std::wstring(L"(not found)") : dir));
	switch (a.action) {
	case Action::Status: {
		GameState st = Inspect(dir);
		Print(L"Status: " + Describe(st));
		if (!st.isGame) {
			return 1;
		}
		std::vector<std::wstring> blocking;
		Firewall fw = FirewallStatus(dir, &blocking);
		Print(L"xlive.dll: " + DllText(st.xlive, st.xliveVersion));
		Print(L"xlive_capcom.dll: " + DllText(st.capcom, L""));
		Print(std::wstring(L"Game running: ") + (st.running ? L"yes" : L"no"));
		Print(L"GFWL leftovers: " + (st.leftovers.empty() ? L"none" : FileList(st.leftovers)));
		Print(L"Backed up: " + (st.backedUp.empty() ? L"none" : FileList(st.backedUp)));
		Print(L"Firewall: " + FirewallText(fw, blocking));
		return 0;
	}
	case Action::Install:
		return PrintResult(Install(dir, a.settings, a.cleanup));
	case Action::Uninstall:
		return PrintResult(Uninstall(dir));
	case Action::Firewall: {
		if (!IsGameDir(dir)) {
			Print(L"FAILED: Lost Planet 2 is not in this folder.");
			return 1;
		}
		std::vector<std::wstring> report;
		bool ok = FirewallAllow(dir, !a.dryRun, report);
		for (const std::wstring& line : report) {
			Print((a.dryRun ? L"  would: " : L"  ") + line);
		}
		if (a.dryRun) {
			Print(ok ? L"OK: dry run, nothing was changed." : L"FAILED: could not read the firewall rules.");
		} else {
			Print(ok ? L"OK: Lost Planet 2 is allowed through Windows Firewall."
			         : L"FAILED: Windows Firewall was not changed (or only partly). This needs administrator rights.");
		}
		return ok ? 0 : 1;
	}
	default:
		return 2;
	}
}

// The window

HWND g_dlg;
HFONT g_titleFont;
std::wstring g_dir;

std::wstring GetText(int id) {
	HWND h = GetDlgItem(g_dlg, id);
	std::wstring s(GetWindowTextLengthW(h), L'\0');
	GetWindowTextW(h, s.data(), (int)s.size() + 1);
	return s;
}

void Refresh() {
	GameState st = Inspect(g_dir);
	SetDlgItemTextW(g_dlg, IDC_DIR, g_dir.c_str());
	SetDlgItemTextW(g_dlg, IDC_STATUS, Describe(st).c_str());
	SetDlgItemTextW(g_dlg, IDC_INSTALL, InstallLabel(st).c_str());
	EnableWindow(GetDlgItem(g_dlg, IDC_INSTALL), CanInstall(st));
	EnableWindow(GetDlgItem(g_dlg, IDC_UNINSTALL), CanUninstall(st));

	std::wstring leftovers;
	if (!st.leftovers.empty()) {
		leftovers = L"In the game folder now: " + FileList(st.leftovers) + L".";
	} else if (!st.backedUp.empty()) {
		leftovers = L"Already in the LP2Netplay-backup folder: " + FileList(st.backedUp) + L". Uninstall puts them back.";
	} else if (st.isGame) {
		leftovers = L"None in the game folder.";
	}
	SetDlgItemTextW(g_dlg, IDC_LEFTOVERS, leftovers.c_str());
	EnableWindow(GetDlgItem(g_dlg, IDC_CLEANUP), !st.leftovers.empty());
	if (st.leftovers.empty()) {
		CheckDlgButton(g_dlg, IDC_CLEANUP, BST_UNCHECKED);
	}

	std::vector<std::wstring> blocking;
	Firewall fw = st.isGame ? FirewallStatus(g_dir, &blocking) : Firewall::Unknown;
	SetDlgItemTextW(g_dlg, IDC_FWSTATUS, st.isGame ? FirewallText(fw, blocking).c_str() : L"");
	EnableWindow(GetDlgItem(g_dlg, IDC_FIREWALL), st.isGame && fw != Firewall::Allowed);
}

void SetDir(const std::wstring& dir) {
	g_dir = dir;
	Settings s = ReadSettings(dir);
	SetDlgItemTextW(g_dlg, IDC_NAME, s.name->c_str());
	SetDlgItemTextW(g_dlg, IDC_PEER, s.peers->c_str());
	SetDlgItemTextW(g_dlg, IDC_LOG, L"");
	Refresh();
}

void ShowResult(const Result& r) {
	std::wstring log = r.summary;
	std::wstring box = r.summary + (r.details.empty() ? L"" : L"\n");
	for (const std::wstring& d : r.details) {
		log += L"\r\n" + d;
		box += L"\n" + d;
	}
	SetDlgItemTextW(g_dlg, IDC_LOG, log.c_str());
	MessageBoxW(g_dlg, box.c_str(), kTitle, MB_OK | (r.ok ? MB_ICONINFORMATION : MB_ICONWARNING));
}

void OnInstall() {
	Settings s;
	s.name = GetText(IDC_NAME);
	s.peers = GetText(IDC_PEER);
	bool cleanup = IsDlgButtonChecked(g_dlg, IDC_CLEANUP) == BST_CHECKED;
	HCURSOR cursor = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
	Result r = Install(g_dir, s, cleanup);
	SetCursor(cursor);
	if (r.ok) {
		SetDlgItemTextW(g_dlg, IDC_NAME, ReadSettings(g_dir).name->c_str());  // as saved (allowed letters only)
	}
	Refresh();
	ShowResult(r);
}

void OnUninstall() {
	if (MessageBoxW(g_dlg,
	                L"Remove LP2 Netplay and put Capcom's original xlive.dll back?\n\nOld Games for Windows LIVE files "
	                L"that Setup moved to the backup folder are put back too.",
	                kTitle, MB_YESNO | MB_ICONQUESTION) != IDYES) {
		return;
	}
	HCURSOR cursor = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
	Result r = Uninstall(g_dir);
	SetCursor(cursor);
	Refresh();
	ShowResult(r);
}

void OnBrowse() {
	ComPtr<IFileOpenDialog> fd;
	if (FAILED(CoCreateInstance(__uuidof(FileOpenDialog), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&fd)))) {
		return;
	}
	DWORD options = 0;
	fd->GetOptions(&options);
	fd->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
	fd->SetTitle(L"Pick the Lost Planet 2 folder (the one with LP2DX9.exe)");
	ComPtr<IShellItem> start;
	if (!g_dir.empty() && SUCCEEDED(SHCreateItemFromParsingName(g_dir.c_str(), nullptr, IID_PPV_ARGS(&start)))) {
		fd->SetFolder(start.Get());
	}
	ComPtr<IShellItem> item;
	PWSTR path = nullptr;
	if (SUCCEEDED(fd->Show(g_dlg)) && SUCCEEDED(fd->GetResult(&item)) &&
	    SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
		SetDir(NormalizeDir(path));
		CoTaskMemFree(path);
	}
}

// Changing the firewall is the player's call: a second copy of Setup does it, started through the Windows (UAC)
// prompt only when the player clicks the button.
void OnFirewall() {
	wchar_t exe[MAX_PATH * 2];
	GetModuleFileNameW(nullptr, exe, ARRAYSIZE(exe));
	std::wstring params = L"--firewall --game-dir \"" + g_dir + L"\"";
	SHELLEXECUTEINFOW sei = {sizeof(sei)};
	sei.fMask = SEE_MASK_NOCLOSEPROCESS;
	sei.hwnd = g_dlg;
	sei.lpVerb = L"runas";
	sei.lpFile = exe;
	sei.lpParameters = params.c_str();
	sei.nShow = SW_HIDE;
	Result r;
	if (!ShellExecuteExW(&sei)) {
		r.summary = GetLastError() == ERROR_CANCELLED
		                ? L"Nothing was changed: Windows asked for permission and it was not given."
		                : L"Setup could not ask Windows for permission to change the firewall.";
	} else if (sei.hProcess) {
		EnableWindow(g_dlg, FALSE);
		while (MsgWaitForMultipleObjects(1, &sei.hProcess, FALSE, INFINITE, QS_ALLINPUT) == WAIT_OBJECT_0 + 1) {
			MSG m;
			while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
				TranslateMessage(&m);
				DispatchMessageW(&m);
			}
		}
		EnableWindow(g_dlg, TRUE);
		DWORD code = 1;
		GetExitCodeProcess(sei.hProcess, &code);
		CloseHandle(sei.hProcess);
		r.ok = code == 0;
		r.summary = r.ok ? L"Done: Windows Firewall now lets Lost Planet 2 through."
		                 : L"Windows Firewall could not be changed. You can still click Allow when Windows asks the "
		                   L"first time the game goes online.";
	}
	Refresh();
	ShowResult(r);
}

INT_PTR CALLBACK DialogProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
	switch (msg) {
	case WM_INITDIALOG: {
		g_dlg = dlg;
		LOGFONTW lf;
		HFONT font = (HFONT)SendMessageW(dlg, WM_GETFONT, 0, 0);
		if (font && GetObjectW(font, sizeof(lf), &lf)) {
			lf.lfHeight = lf.lfHeight * 3 / 2;
			lf.lfWeight = FW_SEMIBOLD;
			g_titleFont = CreateFontIndirectW(&lf);
			SendDlgItemMessageW(dlg, IDC_TITLE, WM_SETFONT, (WPARAM)g_titleFont, FALSE);
		}
		SetDlgItemTextW(dlg, IDC_TITLE, L"LP2 Netplay " LP2NET_VERSION);
		SendDlgItemMessageW(dlg, IDC_NAME, EM_LIMITTEXT, 15, 0);
		SendDlgItemMessageW(dlg, IDC_PEER, EM_SETCUEBANNER, TRUE, (LPARAM)L"optional");
		Button_SetElevationRequiredState(GetDlgItem(dlg, IDC_FIREWALL), TRUE);  // the shield: Windows will ask
		const Args* a = (const Args*)lp;
		SetDir(a->gameDir.empty() ? FindGameDir() : NormalizeDir(a->gameDir));
		HWND first = GetDlgItem(dlg, IsWindowEnabled(GetDlgItem(dlg, IDC_INSTALL)) ? IDC_INSTALL : IDC_BROWSE);
		SendMessageW(dlg, WM_NEXTDLGCTL, (WPARAM)first, TRUE);
		return FALSE;
	}
	case WM_ACTIVATE:
		if (LOWORD(wp) != WA_INACTIVE && IsWindowEnabled(dlg)) {
			Refresh();  // the player may have quit the game or let Steam verify it meanwhile
		}
		return FALSE;
	case WM_COMMAND:
		switch (LOWORD(wp)) {
		case IDC_INSTALL:
			OnInstall();
			return TRUE;
		case IDC_UNINSTALL:
			OnUninstall();
			return TRUE;
		case IDC_BROWSE:
			OnBrowse();
			return TRUE;
		case IDC_FIREWALL:
			OnFirewall();
			return TRUE;
		case IDCANCEL:
			EndDialog(dlg, 0);
			return TRUE;
		}
		break;
	case WM_DESTROY:
		if (g_titleFont) {
			DeleteObject(g_titleFont);
		}
		break;
	}
	return FALSE;
}
}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
	Args a = ParseArgs();
	CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
	int code;
	if (a.action == Action::Window && !a.bad) {
		INITCOMMONCONTROLSEX icc = {sizeof(icc), ICC_STANDARD_CLASSES};
		InitCommonControlsEx(&icc);
		code = (int)DialogBoxParamW(inst, MAKEINTRESOURCEW(IDD_MAIN), nullptr, DialogProc, (LPARAM)&a);
	} else {
		code = RunCommandLine(a);
	}
	CoUninitialize();
	return code;
}
