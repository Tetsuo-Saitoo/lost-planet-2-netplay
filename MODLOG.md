# Lost Planet 2 — GFWL replacement netplay (MODLOG)

Goal: replace Capcom's no-op GFWL stub with a real backend: LAN, join-by-IP / P2P over internet,
session browser, new menu items, installer that cleans GFWL leftovers.

## Install (recon 2026-10-26)
- Path: `C:\Program Files (x86)\Steam\steamapps\common\Lost Planet 2`
- Steam app 45750, depot 45751, buildid 17599994, files dated 2025-07-04
- Exes: `LP2DX9.exe` (22,288,928 B), `LP2DX11.exe` (22,359,072 B), `Launcher.exe` (Capcom config launcher)
- Exes are the ORIGINAL 2010 builds (PE timestamp 2010-08-03, PDB `D:\capdevPC\LostPlanet2\Win32\MasterRelease`),
  wrapped in SteamStub (`.bind` section; `.text` entropy 8.0 on disk). Capcom only swapped xlive.dll.
- Data: `nativePC\` (MT Framework .arc archives; ARCtool at `D:\Tools\ARCtool.exe`). Menus are Scaleform GFx.
- Backups of every file we touch: `re-work\backup-original\` (+ SHA256SUMS.txt).

## The seam: Capcom's stub xlive.dll ("xlive-dummy")
- 273,920 B, built 2025-03-05, MSVC 14.43, x86; Capcom shipped its PDB (`xlive.pdb`) -> real function names.
- 163 ordinals (no names). Sockets pass straight to Winsock (VDP -> UDP). Everything network/session is a constant:
  - XUser: signed in locally as "Player1", fixed XUID; XUserCheckPrivilege -> ERROR_NOT_LOGGED_ON (denied)
  - XNetGetTitleXnAddr -> fake 0.237.190.239; XNetXnAddrToInAddr -> 0.0.0.0; XNetGetEthernetLinkStatus -> 0
  - XSessionCreate -> error; XSessionSearch* -> nothing; XEnumerate -> ERROR_NO_MORE_FILES
  - XNetGetSystemLinkPort writes 3074 in HOST order; the game ntohs()es it -> System Link actually used port 524.
  => System Link could never work with the stub, even on a LAN.
- Saves: `XUserRead/WriteProfileSettings` -> `ProflieSetting\title_specific{N}.bin` relative to the CWD (typo is
  Capcom's). Overlapped args ignored. No saves exist on this PC yet.

## Route (chosen): replacement xlive.dll that forwards to Capcom's stub
- Installer renames Capcom's xlive.dll -> `xlive_capcom.dll` (user's own file; we never ship it) and installs ours.
- Ours implements 97 ordinals: XSocket*, XNet*, XSession*, XUser*, XNotify*, overlapped/enumerate/close,
  XLiveInitializeEx/Render/PreTranslateMessage/OnResetDevice. The other 66 forward to `xlive_capcom.#N`
  (profile/save, stats, achievements, storage, voice, UI stubs) via `#pragma comment(linker, "/export:...")`.
  GOTCHA: `.def` forwarders (`A=mod.B`) do NOT link with MSVC 14.44 x86 (LNK2001), pragmas do.
- No D3D/window hooks needed for the overlay: the game passes its device to XLiveInitializeEx (cbSize 0x1c,
  version 0x31006300) and calls XLiveRender each frame + XLivePreTranslateMessage per message. Only DX9 Reset
  (vtable slot 16) is wrapped so ImGui's D3DPOOL_DEFAULT buffers survive resets.

## Wire design (src/xlive/net.*)
- Instance slot k (named mutex `Local\LP2Net.Slot.k`, k<8): real port = game port + 10000 + 100*k (core socket
  3075 + 100*k). Lets several copies run on one PC and keeps every port above 1024 for UPnP.
- Every datagram: 12-byte header {magic 'L2', ver, type, instanceId, srcGamePort, srcOffset}.
- Peers get virtual addresses 10.x.y.z (from instance id); XNADDR.abEnet carries instance id + port offset;
  ina = LAN IP, inaOnline = public IP (learned via UPnP or peers' HelloAck), wPortOnline = core port.
- Game broadcasts (255.255.255.255:P) fan out to every local slot's port on the LAN broadcast, plus manual
  (join-by-IP) peers and known peers -> the game's own System Link search finds internet hosts.
- Core UDP socket per instance at 3075+100k: Hello/HelloAck, QoS probe/reply (XNetQosListen data), session
  query/beacon for the overlay's session browser. UPnP (Windows IUPnPNAT) maps core + bound game ports.

## Game network internals (from decrypted LP2DX9, analysis copy only)
- Each xlive import is called through a `jmp [iat]` thunk table at 0x01235f44..0x0123612a (DX9).
- Socket wrapper class 0x44c0e0: type 1=TCP, 2=UDP, else VDP; binds INADDR_ANY:htons(port).
- Classes (debug strings): NetworkSessionManagerBroadcast (= System Link: moveInitNetwork/CreateSession/
  SearchSession/JoinSessionCommon/InquireSession/LockSession, management msgs Search/InquireAns, custom msgs
  MatchSession/TerminateSession/SessionChat, connectMember, setSessionPassword), NetworkSessionManagerLive,
  NetworkXboxSession, NetworkXboxMediator, NetworkMultipleUdp(Live), NetworkSingleTcp, NetworkPeerLive
  ("Ignored TYPE_TCP" -> peers use UDP/VDP).
- Debug logger: global system object (DX9 0x018CE2D8, DX11 0x018DE398), vtable +0x20 (this, level, fmt, ...),
  +0x24/+0x28 (this, fmt, ...), all `xor eax,eax; ret` in release. gamelog.cpp finds the global through the
  code that pushes "%s: XNetSetSystemLinkPort() failed Return=%d\n" and routes those slots to lp2net.log.
- Config: `%s\Config.ini` (GRAPHICS section, FullScreen key) — folder not identified yet; not created until the
  Launcher saves settings.

## Verified
- Build: tools\build.bat (VS 2022 BuildTools 14.44, bundled CMake+Ninja) -> build\xlive.dll, /W4 clean.
- Exports: exactly Capcom's 163 ordinals; 66 forwarded.
- nettest (two processes, slots 0/1): broadcast search, host reply with XSESSION_INFO, XNetXnAddrToInAddr maps to
  the reply's virtual source, QoS returns host data, session join (2 members), 10/10 pings. PASS.
- Game: LP2DX9 and LP2DX11 start with our DLL, overlay inits (D3D9 / D3D11), game log hook installs, UPnP maps
  the core port and learns the public IP, game sets System Link port 3074. Ran 6 min in attract loop, no crash.

## In-game System Link VERIFIED (2026-10-26, two copies on one PC)
- Campaign co-op over System Link: copy 1 ("Player 2", slot 1) Campaign > Create Game (Connection Type: System
  Link) hosts; copy 0 ("Player", slot 0) Campaign > Quick Match lists it in SEARCH RESULTS ("Player 2, 1, Standing
  by", Ep 1 / Ch 0 / Normal / Blowback), joins, both lobbies show both soldiers, host Start Game -> both play the
  Episode 1 intro and drop into the mission. Game's own traffic stats (NetworkConnectionStable[1] 1min) match on
  both sides, e.g. client sent 17,146 B/min = host received 17,146. Screenshots in docs\.
- Game flow (from the hooked game log): NetworkSessionManagerLive with connection type SystemLink. UDP socket on
  game port 1001 (real 1001+100k). `MM[b]: Search` broadcast -> host `MM[b]: SearchAns` (152 B) -> client
  XNetQosLookup on host (needs the host's 53-byte session binary in QoS data) -> `applySessionInfo: Join ... by
  search` -> XNetXnAddrToInAddr -> `MM[h]: Join`/`JoinAns` -> XSessionJoinRemote/JoinLocal -> CM EntrySession
  -> XSessionStart when the host begins the mission.
- Display config: `Documents\CAPCOM\LOST PLANET 2\config.ini` (wherever Documents is redirected) [DISPLAY] FullScreen,
  Resolution. Game rewrote 960x540 to 1280x720 (unsupported mode). Test windows then resized with SetWindowPos.

## Bugs found in game and fixed
1. XNetGetOpt(XNET_OPTID_STARTUP_PARAMS) must return XNetStartupParams: LP2 reads cfgQosDataLimitDiv4 and, at 0,
   publishes no session description ("size is over cfgQosDataLimitDiv4 53>0"); joiners then reject the host after
   QoS. Now: game's XNetStartup params merged over GFWL defaults (256 B limit).
2. Broadcasts arrived twice (LAN broadcast + direct copy to a known peer) -> FNV hash de-dup within 100 ms.
3. Saves live in Documents\CAPCOM\LOST PLANET 2\<XUserGetSigninInfo name>\ -> renaming "Player1" hid the save.
   saves.cpp copies the previous profile folder (ProfileFolder in ini, else Player1) to the new gamertag's folder.
4. LP2 joins remote players by online XUID, which is 0 for local profiles -> zero XUIDs never merged as one member.
5. UPnP refused the game ports 1000/1001 (0x800700AA) but mapped 1100/1101/3075: home routers (miniupnpd default
   rules) deny ports < 1024. Game traffic now travels on game port + 10000 + slot offset (11000/11001 for slot 0;
   kGamePortShift / RealGamePort in net.h). Host port forwarding by hand: UDP 11000, 11001, 3075.
   Also: when the router reports a port already mapped to this PC (left over after a killed game), keep it.

## Two real PCs on the LAN VERIFIED (2026-10-26)
- PC 1 (DX9) hosted; PC 2 installed from the test package and joined by Quick Match. Same router, no friend's
  address set.
- Host log: peer hello from PC 2's LAN address -> `MM[c]: Search` / `SearchAns` -> `Join` / `JoinAns` ->
  XSessionStart at 91 s. Played Episode 1 through the end of the first mission, back to lobby, a second
  start, then host left (ReselectHost, XSessionDelete) at 1015 s: ~15 min in session. Players saw no crash and
  no stutter. Host sent 55-117 KB/min to the brother and received 23-63 KB/min.
- Port shift works in game: real ports 11001 / 11000 bound, UPnP mapped 3075, 11001 and 11000 (0x0) on the
  home router.
- Also exercised: DX9 Reset mid-session (OK), overlay opened with Home for ~100 s and closed (no error; what
  it showed not checked), XLiveUninitialize on exit.
- Brother's peer reported no public address (wan = its LAN address). Expected: both PCs use slot 0 ports, and
  the router keeps one UPnP mapping per external port, which this PC already held. Only matters if two PCs
  behind one router both want to host internet games.

## Menu side (2026-10-26, evening)
- Rich presence (from the game's SPA resource `RT_RCDATA/SPAFILE`, XLAST source inside, title 43430808;
  `.re/rsrc.py` + `.re/xdbf.py`, output in `re-work/spa/`): context 0 RICH_MODE (5 Campaign, 6 Online, 12 System
  Link, 2 Ranked, 3 Player Match, ...), context 1 RICH_SCENE, X_CONTEXT_PRESENCE 0x8001 (1 menu, 2 battle,
  3 campaign, 5 lobby), properties 0x1000000A/0x10000009 = episode/chapter. presence.cpp tracks them.
- System Link prompt VERIFIED in game: a titled card (top right, 12 s) the first time per run the player reaches
  Online > System Link or the Campaign menu: what System Link does now, who is reachable, Home / hold Back.
- Overlay VERIFIED in game: Segoe UI at 1/40 of the window height (18 px at 720p, ~27 px at 1080p); while it is
  open the game gets no keyboard/mouse (DirectInput + GetAsyncKeyState) and no XInput (hook on the game's
  delay-import slot of XINPUT1_3!XInputGetState). Keyboard blocking checked in game. Controller: hold Back 0.8 s
  toggles, B closes, ImGui gamepad navigation — NOT tested (no controller on the test bed).
- Game menus: main menu CAMPAIGN / ONLINE / TRAINING / MY PAGE / OPTIONS / CREDITS, with a "[Home] LIVE" hint
  bottom left (the old Guide key — it now opens our overlay). With a local profile, ONLINE greys out Ranked /
  Player / Faction Match (System Link works) and Campaign Connection Type offers only Offline / System Link.
- Guide screens (xui.cpp): XShowGameInviteUI, XShowFriendsUI, XShowGamerCardUI, XShowMessageBoxUI,
  XShowKeyboardUI, XInviteSend, ... are logged, then handed to Capcom's stub. Later: Steam overlay.
- The 46 remaining forwards are now thunks (forwards.cpp) that log the first calls of each, so traces show which
  Capcom functions the game reaches.

### Experiment: [Debug] OnlineSignIn=1 (off by default)
Reports the player as signed in to LIVE (state 2, online XUID 0x0009..., XNET_GET_XNADDR_ONLINE). Findings:
- Ranked / Player / Faction Match become selectable; the main menu shows faction news and "Refresh Online Data".
- Capcom's stub answers XStorageBuildServerPath/Delete/*GetProgress with an "Unknown Function" message box and the
  game retries forever -> xstorage.cpp answers all XStorage calls (nothing to download, uploads dropped).
- The game keeps an ONLINE save in LIVE per-user storage: facility 3 `netSaveData00.dat` (8 KB); also enumerates
  title storage (facility 2). Download failure (0x80070002) is retried 5x per load.
- XUserReadStats must succeed or the Online menu fails -> xstats.cpp returns empty rows.
- LIVE connection flag: mediator object (global 0x18CE2DC) +0x58C must equal XONLINE_S_LOGON_CONNECTION_ESTABLISHED
  (0x1510F0), set only from XN_LIVE_CONNECTIONCHANGED (handler 0x42A310 -> 0x404A00). New listeners now get that
  notification queued (verified delivered).
- Still open: Player Match shows "Communication error. Connection lost." with no xlive call in between. Suspect:
  netSaveData never loaded. Next: keep per-user storage in a local folder and find the not-found code the game
  accepts for a first run.

## Online mode: the game's own Online menus (2026-10-26, night) — [Network] OnlineMode=1 (default)
- Per-user LIVE storage kept locally (xstorage.cpp): Documents\CAPCOM\LOST PLANET 2\<gamertag>\LP2Netplay\user\.
  LP2 treats any failed download as a lost connection, so a missing online save is reported as an empty file; the
  game then creates one and uploads it (netSaveData00.dat, 394 bytes, uploaded when a match starts). That fixed
  "Communication error" on Player Match.
- Matchmaking without LIVE (matchmaking.cpp): hosts publish their session attributes (all user properties and
  contexts + what LIVE added itself: X_PROPERTY_GAMER_HOSTNAME 0x40008109, GAMER_PUID 0x20008107, zone/country/...)
  in the LAN/peer session beacon (SessionExt, appended so older builds still parse it) and in Steam lobby data.
  XSessionSearch/Ex/ByID run the game's own query (9 queries, filters + returns generated from the SPA by
  .re/genqueries.py into lp2_queries.inc) over every known session and build LIVE-style results asynchronously
  (1.2 s for LAN/peers, up to 4 s for the Steam lobby list).
- GOTCHA: GFWL structures use natural alignment, not pack(4): XUSER_DATA 16 bytes, XUSER_PROPERTY 24, stats rows 48.
  Found from the search parameters LP2 passed (second "property" id was the first one's value). gfwl.h now pack(8)
  with static_asserts.
- VERIFIED in game (two copies on one PC, both on Steam): Online > Player Match > Create Match on one, Custom Match >
  Search on the other lists "<name> 2, 1/16, Team Elimination / Battle Gauge / Random <All> / Standard" in the
  game's own SEARCH RESULTS; join -> both in the Player Match lobby (2/16) -> host Start Match -> both in a Team
  Elimination battle (docs/online-player-match-2-copies.jpg). Search log: query 7, parameters 0x10000001=1,
  0x10000008=5, contexts 0x800A=1, 0x800B=1.
- VERIFIED: Campaign > Connection Type "Games for Windows - LIVE" > Create Game on one copy; the other's Campaign
  Custom Match (query 3, 7 parameters) lists it, joins, host Begin Mission -> both play the Episode 1 intro
  (docs/online-campaign-live-2-copies.jpg).
- Open: the idle Player Match (nobody moving, background copy has no input) ended after ~4 min: the client left by
  itself ("close-leave", no xlive call before it), both then showed "Failed to initiate game session. Session has
  been cancelled." The game has IdleTime/KickPenalty logic, so most likely an inactivity kick; check with two
  real players. Points are not awarded for a cancelled match.

## Steam (2026-10-26, night) — steam.cpp, no steam_api.dll shipped
- steamclient.dll from HKCU\Software\Valve\Steam\ActiveProcess\SteamClientDll; CreateInterface("SteamClient020"),
  CreateSteamPipe, ConnectToGlobalUser; SteamUser023, SteamFriends017, SteamMatchMaking009, SteamNetworking006,
  SteamUtils010 with vtables declared from Valve's published headers (source-sdk-2013, re-work/steamsdk, not
  shipped). Callbacks via Steam_BGetCallback/Steam_FreeLastCallback on the net worker; call results via
  ISteamUtils::IsAPICallCompleted/GetAPICallResult. Handlers run after the Steam lock is released (lock order).
- VERIFIED: connects as the player's account (app 45750) when the game is started directly with
  SteamAppId; gamertag = sanitised Steam name ([Steam] UseSteamName=1; the save is copied to the new name's
  folder); hosting creates a lobby (listed for matchmaking sessions, friends-only for System Link); lobby search
  finds it.
- Online XUID = 0x00095354 << 32 | Steam account id ("ST" marker), so gamer cards map back to Steam profiles.
- Guide screens -> Steam overlay: lobby "Game invite" -> invite dialog for our lobby, Gamer card / player review ->
  Steam profile, friends -> friends list. GameOverlayActivated posts XN_SYS_UI so the game pauses its input.
- Invites / Join Game: GameLobbyJoinRequested / GameRichPresenceJoinRequested ("connect" = "+lp2net_lobby <id>",
  also passed on the command line when Steam starts the game) -> lobby data -> host introduced as a peer ->
  LIVE-style session: XN_LIVE_INVITE_ACCEPTED + XInviteGetAcceptedInfo (the game joins by itself); System Link
  session: prompt to open System Link > Quick Match.
- Relay: Steam P2P (ISteamNetworking, Valve relays) for peers with a Steam id and no recent direct UDP (20 s);
  frame = [u16 destination game port, 0 = core][PacketHeader][payload]; received game frames are handed to the
  game socket over loopback from an injector socket. Never to our own account (copies on one PC share it).
- Rich presence: "status" = presence text, steam_player_group(_size) = session, "connect" while hosting.
- Overlay: new Steam tab (friends playing LP2 with Join, Invite friends, Steam friends list).
- NOT verified yet (needs a second Steam account / PC): relay, invites, Join Game, Steam overlay pages (the game
  was started directly; started from Steam it stops at Steam's one-time CD key window).

## Two-PC test package
- tools\make_package.ps1 -> dist\LP2Netplay-test\ + .zip: xlive.dll, install.ps1 (finds the game via registry or
  Steam libraryfolders.vdf, only replaces Capcom's 2025 stub by SHA-256, asks gamertag + friend's address,
  checks firewall), Install / Uninstall / "Allow through firewall" .bat files, HOW TO TEST.txt.
- Tested on this PC: uninstall restores Capcom's exact file; install keeps it as xlive_capcom.dll.

## Installer, CI and releases (phase 3)
- One version number: `project(lp2net VERSION 0.2.0)` -> `build\generated\lp2net_version.h` (from
  `src/version.h.in`) -> the VERSIONINFO of xlive.dll and LP2NetSetup.exe (`src/versioninfo.rc`, ProductName
  "LP2 Netplay"), the first log line (`LP2Net 0.2.0 (<date> <time>) in <dir>`), the overlay title (ImGui ID kept as
  `###LP2 Netplay`; there is no imgui.ini, so nothing moves) and the package name.
- LP2NetSetup.exe (`src/setup/`): Win32 dialog, static CRT, imports only Windows DLLs, ~3.5 MB with xlive.dll as
  RCDATA (`build\generated\payload.rc`). CMake: OBJECT_DEPENDS takes no `$<TARGET_FILE>`, and a GENERATED source is
  deleted by `--clean-first`, so payload.rc is written at configure time with the plain path `build\xlive.dll`
  (single-config Ninja only). RC sources get COMPILE_OPTIONS too: xlive's `/W4 /utf-8` are now CXX-only.
  - Manifest: asInvoker (without it, "Setup" in the file name triggers Windows' installer detection and a UAC
    prompt), Common Controls 6, PerMonitorV2.
  - Finds the game like install.ps1: `HKLM\SOFTWARE\Capcom\LostPlanet 2` installdir (32-bit view), else every
    Steam library in libraryfolders.vdf.
  - Tells files apart: Capcom's stub by SHA-256; ours by ProductName, or for builds before 0.2.0 (no version
    resource) by the UTF-16 strings `xlive_capcom.dll` and `lp2net.ini` (both v0.1.0 builds on this PC match).
    Anything else is refused and left alone. A locked xlive.dll is refused with "in use", not "unknown file".
  - Install: Capcom's stub -> xlive_capcom.dll (only when there is none yet), then ours via `xlive.dll.lp2new`,
    hash check, MoveFileEx(REPLACE_EXISTING), hash check; a failure leaves the old file (and moves the stub back if
    it had just been moved). Steam putting the stub back after an update (xlive.dll = xlive_capcom.dll = Capcom's)
    is "not active" -> Install. Refused: unknown xlive.dll, xlive_capcom.dll that is not Capcom's, ours without
    Capcom's stub (-> Steam verify), game or launcher running from that folder.
  - Uninstall: xlive_capcom.dll back over xlive.dll; a duplicate Capcom copy is deleted; settings and log stay.
  - Settings: `[Player] Name` (same rule as config.cpp), `[Network] Peers` (letters, digits, `.:,;_-[]` and
    spaces), `[Debug] AllowMultipleCopies=0` (as install.ps1). From the command line only the given ones change.
  - GFWL leftovers (opt-in): `gfwlivesetup.exe`, `xliveredist.msi`, `wllogin_32/64.msi`, `*.exe.cat`,
    `*.exe.cfg` in the game folder root, matched by name suffix (FindFirstFile's 8.3 matching would let `*.exe.cfg`
    catch `x.exe.cfgbak`). Listed in the window before anything moves; moved (same drive, so nothing is copied or
    deleted) to `<game>\LP2Netplay-backup\`; Uninstall moves them back, dropping backup copies that Steam has
    already replaced with identical files. Steam's install script for 45750 runs `%INSTALLDIR%\gwfwlsetup.exe /q`,
    a misspelling, so Steam never ran gfwlivesetup.exe anyway; neither stub nor ours reads the .cat/.cfg files.
    Steam "verify" downloads moved files again (~260 MB).
  - Firewall: status via INetFwPolicy2 (readable without admin, not tied to netsh's English output). The button
    starts `LP2NetSetup.exe --firewall --game-dir <dir>` through ShellExecute "runas" (UAC): it disables enabled
    inbound block rules for LP2DX9/LP2DX11.exe and adds "Lost Planet 2 (LP2 Netplay) <exe>" (inbound UDP, private
    and public; the names install.ps1 uses). Never automatic; `--firewall --dry-run` only prints the plan.
  - Command line (`--help`): `--status`, `--install`/`--repair [--name N] [--peer A] [--cleanup]`, `--uninstall`,
    `--firewall [--dry-run]`, `--game-dir DIR`. Output goes to the stdout the caller provides; exit 0/1/2.
- `tests\setup_test.ps1`: 57 checks against fake game folders in %TEMP% (copy of Capcom's stub + dummy files),
  including the old-build update (`-OldBuild`), locked files and a fake running game (a copy of ping.exe named
  LP2DX9.exe). The window was also driven on a fake folder (WM_COMMAND clicks, PrintWindow captures): Install with
  the backup ticked, Uninstall, result boxes.
- Real game folder (2026-10-27, under the test-bed lock, command line): `--status` recognised the installed v0.1
  build as "LP2 Netplay (test build)" and read the firewall as allowing the game; `--uninstall` put Capcom's stub
  back; `--install --cleanup` kept the stub, wrote ours (same bytes as build\xlive.dll) and moved the 5 GFWL files
  (rename on the same drive: 121 ms for 259 MB); `--uninstall` put everything back. Every file hash in the game
  folder matched the state before the test, and Release restored the known-good build. While the game ran, Setup
  reported it and refused to touch anything.
- Package (`tools\make_package.ps1`): `dist\LP2Netplay-<version>.zip` with LP2NetSetup.exe, HOW TO TEST.txt,
  VERSION.txt (commit, SHA-256s) and the old script installer in `manual\` (fallback if the exe is blocked).
- CI (`.github/workflows/build.yml`): windows-latest, `tools\build.bat` (falls back to vswhere when the Build Tools
  path is missing), package, artifact; on `v*` tags a check that the tag matches project(VERSION), then a draft
  release (GITHUB_TOKEN, `gh release create`, pre-release when the tag has a suffix). Actions pinned to commit
  SHAs: checkout v7.0.1, upload-artifact v7.0.2, download-artifact v8.0.2. `.gitattributes` keeps .bat/.ps1/.txt
  CRLF on the runner. The workflow has not run yet (needs a push).

## Multi-copy testing ([Debug] AllowMultipleCopies=1, off by default)
- Single-instance check: CreateMutexA(NULL, TRUE, "sPlanetMain") + FindWindowW -> hooked via the game's IAT.
- Keyboard reaches every copy (DirectInput background mode + GetAsyncKeyState) -> background copies get zeroed
  keyboard/mouse state (gamepads untouched).
- tools\run_two.ps1 starts both and lays them out; tools\focus_lp2.ps1 -Index N gives one the keyboard (Alt tap +
  SetForegroundWindow; clicking a game window does not reliably activate it).

## Voice chat (phase 2) — xhv.cpp
### How LP2 uses XHV (analysis copy, DX9 addresses)
- Class NetworkXboxVoice, a 0x100-byte object at global 0x018CE2F0 (engine pointer at +8). init 0x444D70 calls
  XHVCreateEngine with dwMaxRemoteTalkers = min(n, 30) (16 seen in game), dwMaxLocalTalkers 1, local modes {1, 2},
  remote modes {2}, no custom VAD, no raw-data callback, hwndFocus = game window. Mode 1 = XHV_LOOPBACK_MODE,
  2 = XHV_VOICECHAT_MODE (the game switches between them in setLoopbackMode 0x4453B0).
- IXHVEngine slots used (byte offsets): 0x0C/0x10 Start/StopLocalProcessingModes, 0x14 StartRemoteProcessingModes,
  0x20/0x24 Register/UnregisterLocalTalker, 0x28 RegisterRemoteTalker(xuid, 0, 0, 0), 0x2C UnregisterRemoteTalker,
  0x30 GetRemoteTalkers (capacity 30), 0x34 IsHeadsetPresent (sent to the others as "hasVoice",
  checkVoiceHeadsetStatus 0x414700), 0x38 IsLocalTalking, 0x3C IsRemoteTalking (talking indicators, 0x414A20),
  0x40 GetDataReadyFlags, 0x44 GetLocalChatData, 0x48 SetPlaybackPriority (0xFFFFFFFF = muted), 0x4C
  SubmitIncomingChatData. Destructor path calls Release (0x04).
- The game carries voice itself (GFWL's model): move() 0x444F50 runs each frame, and when GetDataReadyFlags & 1 it
  pulls GetLocalChatData (buffer 100 bytes) into a 100-byte queue at +0x18 (fill at +0x7C). takeVoiceData 0x445740
  flushes the queue when it holds > 70 bytes or 0.2 s passed (float 0x016E71CC); 0x4147F0 sends it to each member
  that isMutedRemoteUser (0x404DE0: XUserMuteListQuery, XUserAreUsersFriends) does not block and whose per-member
  voice flag (+0x13D68) is set; the receiver (0x414920) calls putVoiceData 0x445800 -> SubmitIncomingChatData.
  If the queue would overflow the game drops the oldest bytes ("Drop not send VoiceData").
- Startup in game: XHVCreateEngine, RegisterLocalTalker(0) and StartLocalProcessingModes(0, [2]) ~4 s after
  launch. Mute-list changes: XN_SYS_MUTELISTCHANGED (0x11) -> setNotifySysMutelistchanged 0x4049C0 -> updateMutelists.

### Design: audio on our transport, control stays with the game
- The game's channel was sized for the Xbox 360 codec (10-byte packets, ~4 kbit/s). Steam's Opus voice is several
  times that, and the byte-level drops would break any variable-size framing. So GetDataReadyFlags always returns 0
  (the game sends nothing) and voice travels as PacketType::Voice (8) on the core socket, or the Steam relay like
  other core packets (net.cpp: SendVoice, VoicePeers, a handler called without the net lock). Older builds ignore
  type 8.
- The game still decides everything visible: Register/UnregisterRemoteTalker = who we talk with,
  SetPlaybackPriority(never) and XUserMuteListQuery = mute (both directions: we neither play nor send), and it reads
  IsLocalTalking / IsRemoteTalking / IsHeadsetPresent for its indicators.
- Body: VoiceMsg {kind, codec, flags, seq, cbData, xuid (as the game sees it), offline xuid} + data. Every 2 s
  (and right after RegisterRemoteTalker) each copy announces its XUIDs and name to peers heard in the last 15 s; our
  audio goes only to peers whose XUID the game registered and has not muted. Audio from anyone else is dropped.
- Audio inside: 16 kHz mono 16-bit. Microphone: Steam (StartVoiceRecording/GetVoice, ISteamUser slots 7-12 added
  to steam.cpp, codec 1 = Steam's own compressed format, decoded with DecompressVoice at 16 kHz) when Steam runs and
  [Voice] SteamMicrophone=1; else the Windows default recording device (waveIn) as IMA ADPCM (codec 2, 40 ms
  packets of 324 bytes, ~9 KB/s per listener, each packet carries its own decoder state). PCM sources send while an
  RMS gate (200/32768, 400 ms hangover) and push-to-talk allow. The microphone opens only while the game has a
  remote talker registered (or loopback / the overlay's microphone test), not at the menus.
- Playback: per-talker jitter buffer (starts at 100 ms, capped at 0.5 s), mixed with [Voice] Volume into waveOut
  (5 x 20 ms buffers). A talker "talks" for 250 ms after audible audio was played. Loopback mode and the microphone
  test play our own voice back.
- Settings: [Voice] Enabled, PushToTalk (virtual-key code, 0 = open mic), Volume (0-200 %), SteamMicrophone, Muted
  (XUIDs). Overlay tab "Voice": the same, a microphone test, the talkers with talking state and a mute box (posts
  XN_SYS_MUTELISTCHANGED so the game re-reads the list), counters.
- Debug: [Debug] VoiceFeedWav (any PCM WAV, loops, replaces the microphone), VoiceFeedCopy (0 = every copy, N = only
  copy N), VoiceDumpWav (what is played, after volume; copy N > 1 writes "-copyN"). lp2net.log gets counters every
  10 s: blocks captured, packets/bytes sent and received, decoded, decode errors, lost (sequence gaps), underruns
  (ran dry inside a talk spurt; the first packet after a pause carries a spurt-start flag), played blocks, drops.

### Verified
- Build: /W4 clean, still exactly Capcom's 163 ordinals; 5008 and 5314 are ours now (43 forwards left). New import:
  WINMM.dll (part of Windows).
- tests/voicetest.cpp (two processes, no game, Steam off, both feeding a 9.6 s TTS phrase + 1 kHz marker WAV, ini
  next to the exe with Peers=127.0.0.1:3075,127.0.0.1:3175): PASS both ways. In 20 s each side sent 338-342 packets
  (~118 KB) and the other received them all, 0 lost, 0 decode errors, 3 underruns; each side's IsRemoteTalking
  followed the other's speech (19 talk spurts in 25 s). Both dumps hold the feed: the marker tone appears exactly one
  loop (9.6 s) apart and the 20 ms loudness envelope matches the feed with correlation 0.97-0.997, no silent frames
  where the feed is loud.
- In game (two DX9 copies, test bed, WAV feed set): the game creates the engine at startup (maxRemote 16, modes as
  above), registers local talker 0 in voice chat mode, the voice thread starts, Steam's GetVoiceOptimalSampleRate
  returns 24000 (the new ISteamUser slots line up), and on quit the game unregisters the talker, releases the
  engine and the thread stops before XLiveUninitialize. Ran ~6 min at the menus; no errors or failures in the logs.
- Two real PCs (2026-10-27, the build from before the 0.2.0 merge): a System Link battle with this PC as host, the
  Steam microphone on both. The tester heard the other player in game without problems. This PC's log: the game
  registered the remote talker 199 s after start, then an 8-minute battle with 1920 voice packets received, 0 lost,
  0 underruns, 0 decode errors; on quit the engine was released cleanly before XLiveUninitialize. So remote talker
  registration, Steam capture and DecompressVoice with real speech work.
- NOT verified yet: the game's talking icons, mute (overlay and the game's own menus), push-to-talk, the overlay
  Voice tab on screen, voice over the internet (Steam relay), and two PCs on 0.2.0 (packets that arrive twice, see
  Gotchas). The second test-bed copy did not quit on WM_CLOSE (it showed something, probably the quit prompt, that
  needs input) and was ended by PID.

### Proximity voice (requested next): findings and plan
- MT Framework's type registry is readable statically: every class registers through the MtDTI constructor
  (DX9 0x780A60: name, parent DTI, object size), 3540 classes. getDTI is `mov eax, dti; ret` at vtable slot 4,
  createProperty at slot 3 registers each field's name and address (scratch scripts dti.py / props.py).
- Positions: uCoord (child of cUnit, 0xE0 bytes) registers mPos at +0x30, mQuat +0x50, mWmat (world matrix) +0x70,
  so every unit has its world position and orientation at fixed offsets. Soldiers are uHuman (0x2620, child of
  uFighter -> uCharacterBase), with variants uHm00..uHm29; Akrids are uAkrid/uAkNN.
- Players: cPlayer (0x8D0) registers mpCtrlPl (uCtrlPlayer*), mpCtrlCam (uCtrlCamera*) and mInfo
  (cPlayer::PlInfo: mPlayStatus, mPlNo, mCustomPartsID); uCtrlCamera also registers mPlNo. So the likely chain is
  cPlayer -> mpCtrlPl -> the soldier it drives -> +0x30, and mpCtrlCam for the listener (or sCamera, 0xD50, a
  cSystem singleton with sCamera::Viewport). Remote players' soldiers are driven by cThinkNetPuppet; the
  network classes (cNetGameManager, cNetActHm) register no fields. sPlanetUnit (sUnit, with sUnit::MoveLine lists)
  holds the live units.
- Still unknown: where the cPlayer instances live, and how a player number (mPlNo) maps to a session member and
  XUID (candidates: PlInfo may hold the gamertag; sPlanetNetwork / cNetGameManager). Both need memory reads in a
  running mission with two players, plus the world scale (MT titles usually use 1 unit = 1 cm).
- Plan: (1) a position source in xhv.cpp that finds the singletons at runtime by pattern (DX9 and DX11 differ, as in
  gamelog.cpp), reads each player's soldier position and the camera, and maps players to talker XUIDs, logging what
  it finds; (2) a stereo mixer: constant-power pan from the angle to the camera's facing, distance gain from full
  volume inside [Voice] ProximityNear to silent at ProximityMax, smoothed per block; (3) [Voice] Proximity=1 to turn
  it on, falling back to the flat mix whenever positions are missing (menus, lobby, loading, a player not mapped).
  Own panning over waveOut rather than OpenAL Soft: no extra DLL or LGPL notice, and stereo panning is enough for
  "where is that voice"; HRTF could come later.

### Gotchas
- GetTickCount64 stamps from several threads: `now - t` can underflow when another thread stamped t after the
  caller read `now` (this erased a peer's entry the moment it was added). xhv.cpp uses Age(t, now).
- Steam's GetVoice data is only decodable as a whole call's output; never split it across packets.
- While someone talks, GetVoice's result alternates 0 (data) and 3 (no data yet) every ~40 ms. Logging each change
  wrote ~4000 lines in 12 minutes; the log now treats 0 and 3 as one state and notes only errors (2 not recording,
  6 restricted) and the way back.
- Since 0.2.0, core packets go over UDP and the Steam relay until a direct HelloAck arrives (with one-way UDP, for
  the whole game), so a voice packet can arrive twice. OnVoicePacket drops a packet 1..999 behind the next expected
  sequence number within 5 s, before decoding, and counts it as a duplicate.

## Phase 1: hardening (2026-10-27)
- FIXED: two PCs on one home network that met through Steam (Player Match search, lobby data) talked over the
  Steam relay only. `UseDirect` refused plain UDP for any peer with a Steam id until a UDP packet had arrived from
  them, and the other side did the same, so UDP never started. The relay carried a real Player Match for 5.5 min,
  then the game dropped the other player ("norecv timeout (20sec)", error 0x80030035) and the relay to them kept
  failing (P2PSessionConnectFail error 4) afterwards.
  Now: UDP is always tried when we have an address (from their packets, or a guess from their XNADDR: same public
  address = same LAN). The relay carries a copy until UDP is proven both ways: core traffic until they answer a UDP
  hello over UDP (`lastDirectAck`, hellos every 3 s for Steam peers, 10 s window), game traffic until their hello
  reports that our game packets reach them over UDP (`hearYourGame`, new optional field at the end of HelloBody;
  older builds without it get the core rule). A relayed game frame is dropped when that peer's UDP game packets
  arrived within 1.5 s (the game already has it). Hellos and beacons stop 2 min after a peer goes silent.
  The report is sticky: it turns yes when their game packets arrive over UDP and no only when they arrive through
  Steam alone; a quiet lobby keeps the last answer (a plain 10 s freshness flipped it to "no" in every idle lobby).
- Steam Join Game on a friend who hosts a System Link game still asks the player to open the menu themselves (the
  game only accepts LIVE-style invites). In a real test that prompt ("open System Link (or Campaign)") sent the
  player into the wrong menus. Hosts now put their RICH_MODE in the lobby data ("mode"), and the prompt names the
  path: Campaign host -> "open Campaign, set Connection Type to System Link, then pick Quick Match"; Versus host ->
  "open Online > System Link, then pick Quick Match". Older hosts get the old text. The log line carries flags and
  mode.
- VERIFIED (two copies on one PC): Online > Ranked Match > Quick Match on one copy creates a ranked lobby
  (flags 0x43F, arbitration); Quick Match on the other finds and joins it; the countdown starts with 2 players;
  ArbitrateNonce/ArbitrateResult between the copies, XSessionArbitrationRegister as host and peer, XSessionStart,
  Elimination battle. The new UDP report works ("our game packets now reach them over UDP"). A full ranked battle
  ran 10 min with both players idle (no inactivity kick, no drop), ended with TerminateSession, and the profile got
  ranked points (210 pts). A second ranked battle (Team Elimination, both copies idle) lost the client 4.4 min in:
  it sent close-leave on its own with no xlive call or network error before it, the host then dropped it ("Improve
  TrueSkill score of dropped user") and deleted the session. Same signature as the idle Player Match earlier; it
  did not happen in the 10-minute Elimination battle. Most likely the game's own handling of a player who never
  moves; check with real players before treating it as a bug (the real two-PC drop was a different thing: a
  20 s receive timeout, fixed above).
- VERIFIED (two copies): Online > System Link > Create Match (Data Post Battle) on one copy; System Link > Quick
  Match on the other finds it (Search / SearchAns), joins (2/16), host Begin the match -> both in the battle on
  opposite teams.
- Ranked Match end: the game asks XUserEstimateRankForRating (#5346) where the new rating ranks. Capcom's stub
  returned 0 without completing, logged by the game as "XUserEstimateRankForRating(1) failed". Now ours (xstats.cpp):
  completes asynchronously with rank 1 for every rating (no shared leaderboard yet). Not seen in game yet: the
  second ranked test (Team Elimination) ended before the time limit (below). GOTCHA: LP2 builds the rank
  requests as 12-byte records (DWORD view id + LONGLONG rating, no padding), unlike every other GFWL struct;
  results buffer = 8 + 4 per rating.
- Log: friends' Steam lobbies are re-read every 15 s; their "lobby ... data" line is now only logged when the
  lobby is new or its data changed.
- Achievements: LP2 on Steam has no Steam achievements (no steam_api.dll, no stats schema for 45750); Capcom's stub
  answers XUserWriteAchievements with 0 and XUserCreateAchievementEnumerator with an error, so unlocks go nowhere
  since 2025. Candidate for phase 4: keep them locally and show them in the overlay.
- DLC: Capcom's XLiveContentCreateEnumerator returns an error, so the game sees no DLC. Every player is on the same
  content: nothing to match up in lobbies.
- Still forwarded to Capcom's stub and called: XHVCreateEngine and XUserMuteListQuery (phase 2), protected data /
  profile settings (local, fine), the content enumerator (above). Stats enumerators (5284/5286) are phase 4.
- FOUND (static, analysis copy, DX9 addresses), a candidate cause of the "player leaves the match by itself after
  a few minutes" drops: the game's own frozen-game watchdog (not confirmed at runtime, see the test below). A real
  two-PC System Link battle (voice test build) lost the joined player 7 min 49 s after XSessionStart, with the same
  signature as the two-copy drops (all connections close-leave at once, XSessionDelete, no network error, no log
  line). The user says it ends after a set time and being idle makes no difference.
  - sPlanetNetwork (instance pointer 0x018420A4, vtable 0x01369D60) move = vtable slot 6 = 0x84F890. While
    [game+0x14598] bit 0 is set, [game+0x14590] bit 0x100000 is clear, [net+0xA1F8] != -1 (in a session) and
    [net+0xA1E8] == 0 (not leaving), it compares the game object's frame counter [game+0x14800] (game =
    *0x018C080C) with the value it saw last ([net+0x11354C]). Unchanged -> [net+0x113550]++, and at 0x708 (1800)
    it calls vtable slot 30 (0x84EAE0) with 0: [net+0xA1E8] = 1, leave state [net+0x113544] = 0, so
    sPlanetNetwork::leaveProc (0x84A490) runs from the next move and leaves the session. Changed -> the counter
    resets.
  - The frame counter goes up by one in the game object's move (vtable 0x01368C18 slot 6 = 0x8452D0,
    unconditional INC at 0x84530C); FUN_00846930 resets it to 0.
  - So the watchdog fires when the network side keeps ticking while the game object's move does not run for 1800
    network ticks.
- watchdog.cpp (both exes, found by code bytes): logs the check's count (once a minute in a session: game frames
  and the highest count seen; a line from 600 up with window/overlay state; one when the game starts leaving, with
  the count), and with [Network] FreezeGuard=1 (default) raises the 0x708 limit so the check never ends a match.
  It also hooks the entry of both leave functions and logs the caller: "session leave: the session's leave(0)
  called from <address>", marked "(the freeze check ...)" when the check is the caller (return address 0x84F8E8).
- TESTED (two copies, System Link Team Elimination, 20-minute limit, both idle): no drop; the match ran the full 20
  minutes and ended with TerminateSession / XSessionEnd. Both copies ran ~6500-7000 game frames a minute; the
  count never went above 10 (the limit is 1800). Not raised by: our overlay open 30 s (the game read XN_SYS_UI=1),
  the window minimized 35 s, the window thread blocked 25 s (title bar held; the window then stopped responding,
  the game and network still finished the match). So in normal play the check is nowhere near firing, and none of
  the suspected triggers reach it.
- The 0x84EAE0 leave is not the check's alone: it is the session's general leave (vtable slot 30). Seen callers
  (DX9): the PDA's Quit Game 0xF205D2 (then a no-op second call from 0xF205F9), the lobby's Leave 0xE852E4, and the
  host reacting to the last other player leaving a battle 0x89B42E (the host then deletes the session too).
  leave(reason) 0x84A420 has 21 more callers. The next real drop's log names its caller.
- The real two-PC drop (7 min 49 s) was on the voice build from before the 0.2.0 merge, which was based on the
  first commit: it lacked 73c9cda (two PCs that met through Steam talked over the Steam relay only, and "the game
  dropped the other player" after 5.5 min on the relay). 0.2.0 tries UDP first. The two-copy drops (UDP on one PC)
  stay unexplained; the ranked one was after 73c9cda.

## Linux and Steam Deck (phase 7; not tried on real hardware yet)
- LP2 has no Linux build: Steam runs it through Proton, which loads the game folder's xlive.dll as a native DLL
  (Wine has no builtin xlive), so the Windows build is the Linux build and the players share one network.
- `tools/linux/"Install LP2 Netplay.sh"` (+ "Uninstall LP2 Netplay.sh", "LINUX AND STEAM DECK.txt"):
  - finds the game in native Steam (`~/.local/share/Steam`, `~/.steam/steam|root`: Steam Deck, SteamOS, Bazzite,
    most distros), Flatpak (`~/.var/app/com.valvesoftware.Steam/...`) and Snap Steam, and every library in their
    libraryfolders.vdf (SD cards, other drives); `LP2_GAME_DIR` overrides;
  - keeps Capcom's stub by SHA-256 like the Windows installers, refuses anything else, uninstall puts it back;
  - double-clicked (no terminal) it talks through kdialog (KDE, Steam Deck) or zenity (GNOME) and asks before
    changing anything; with neither it reopens itself in a terminal window; from a terminal it just prints;
  - it does not set the launch option `WINEDLLOVERRIDES="xlive=n,b" %command%`; the text says to add it only if
    no lp2net.log appears (Steam's localconfig.vdf is rewritten by a running Steam).
- Packaging: make_package.ps1 adds `linux\` to the zip; `tools/pack_dist.py` (Python, stdlib only) writes the zip
  and `LP2Netplay-<version>-linux.tar.gz` with Unix permissions (0755 for *.sh) so the scripts stay executable and
  double-clickable after unpacking. CI runs `tests/linux_install_test.sh` (41 checks: Deck + SD card, Flatpak,
  Snap, update, uninstall byte-identical, refusals, kdialog / zenity dialog paths with stubs) with a stand-in
  for Capcom's file; one check ties the script's Capcom SHA-256 to the Windows installer's. `.gitattributes`
  keeps `*.sh` LF.
- GOTCHA (found by the first CI run): GNU `sha256sum FILE` puts a `\` before the hash when the path contains a
  backslash (the Windows runner's temp folder), so `| cut -d' ' -f1` no longer matches. Hash from stdin
  (`sha256sum < FILE`), in the installer and the tests.
- Unknowns until real hardware runs it: whether Proton's lsteamclient answers our SteamClient020 interfaces
  (without it the Steam features stay off and System Link still works), UPnP under Wine (expected to fail
  quietly), the overlay on the Deck's controls (hold View) and screen.

## Stats, leaderboards and achievements (phase 4, 2026-10-27)
- Files: `xstats.cpp` (stats store, sharing, read/enumerate/estimate), `achievements.cpp`, `spa.cpp/.h` (reads the
  game's SPA), `stats.h`. Now ours instead of Capcom's: 5278 XUserWriteAchievements, 5280
  XUserCreateAchievementEnumerator, 5284 XUserCreateStatsEnumeratorByRank, 5286 XUserCreateStatsEnumeratorByXuid,
  next to 5281 XUserReadStats and 5346 XUserEstimateRankForRating. The estimate is now real: 1 + the number of
  known rows with a better rating (it used to be rank 1 for everything).
- SPA: an XDBF file (big-endian) in the exe, `FindResourceW(exe, L"SPAFILE", L"RT_RCDATA")`. The type is the
  string "RT_RCDATA", not RT_RCDATA (10); id 10 is tried too. Header 24 bytes, entries 18 bytes, data at
  24 + entries*18 + free*8. Namespace 1: XACH (36-byte achievement records), XVC2 (stats views). Namespace 3, id 1:
  English XSTR strings. It is read from the running exe (GameSpa()); nothing from it is shipped.
- XVC2: u16 shared-view count. Each shared view: u16 columns, u16 rows, 8 reserved, then columns+rows fields, each
  with its own size (0x20 in LP2): size, property id, flags (1 = hidden), u16 attribute id, u16 string id, u16
  aggregation (0x8001 last, 0x800B max), ... Next comes an XPBM block whose size field is wrong: 'XPBM', version,
  size, 4 bytes, then a u32 count (not u16) and that many u32 property ids. Then a u16 view count and 16-byte views:
  id, flags, u16 shared-view index, u16 string id, 4 unused.
- LP2: 50 achievements (1000 G). 44 are secret: no SPA flag 0x8 (shown while locked) and no "unachieved" text.
  There are 63 views, 62 of them rated. Views 1-3 are battle views. Views 13-37 are Training Basic (0xD = map 1
  level 1), 38-62 are Training Advanced (0x26 = map 1 level 1), and 0xFFFF0000 is the TrueSkill view.
- The column ids the game reads are SPA attribute ids, not property ids: [1..9] for battle views, [1..4] for
  training views. The rating comes back in the row's i64Rating.
- Training SCORE (0x2000001D, the rating of every training view) is minus the time in ms: -251308 for 04'11"308.
  So the view's Max aggregation keeps the best time. A run also writes 0x2000001C (options), 0x20000013 (a 64-bit
  day value), 0x2000001E and 0x2000001F.
- Call order of a Training run:
  1. XSessionCreate(flags 0x4, 1 private slot), then JoinLocal and Start.
  2. XUserEstimateRankForRating(view, score).
  3. XSessionWriteStats with no XOVERLAPPED: it is synchronous.
  4. End, then Delete.
  The leaderboard screen calls ByRank(from 1, 1 row) for the world record, then ByRank(from 1, 10 rows) and ByXuid
  for the player's row. The Online menu reads views 1-3 for the local player. A row with rank 0 means "not
  ranked", and the game leaves it out.
- Store: `<profile>\LP2Netplay\stats.dat` (binary 'LP2S' v1: met players, then rows of view, xuid, own, gamertag,
  stamp, writes and properties). It keeps the player's own rows plus the rows of players met in a session
  (XSessionJoinRemote). Ranks use competition ranking by rating; on a tie the earlier write ranks first.
  XSessionWriteStats for other players' xuids (a host writing for everyone) is ignored: each player's copy writes
  and shares its own row. Nothing is written until there is a row to keep.
- Sharing uses PacketType::Stats = 9 (8 is Voice on the phase 2 branch). Own rows go to a peer on its first hello,
  and to every live peer after each write, in packets of at most 1200 bytes. The header has version, row count,
  xuid and a 16-byte gamertag. Each row has view, stamp, writes and n properties. There is no resend: the next write
  or hello fixes a lost packet. A received row replaces ours when it is newer or the name changed. Rows from players
  not met yet stay in memory only, and are shown and saved after a JoinRemote. Lock order is net then stats:
  HandleCore calls StatsReceive/StatsPackets under the net lock, and StatsWrite drops the stats lock before
  SendStatsToPeers.
- The friends leaderboard stays empty. XFriendsCreateEnumerator is untouched because it also feeds
  NetworkCommunityLive::moveGetFriendList (XONLINE_FRIEND is 196 bytes).
- Achievements:
  - The game queues an unlock (NetworkXboxMediator::writeAchievements) only while signed in. It expects
    ERROR_IO_PENDING, then XGetOverlappedResult success.
  - Unlocks are kept in `<profile>\LP2Netplay\achievements.txt` ("id FILETIME" lines). Each new one shows an
    "Achievement unlocked" toast with the name, G and description.
  - The overlay's Achievements tab (between Join by IP and Settings) shows the count, the G total and a "Show secret
    achievements" checkbox. A locked secret one reads "Secret achievement". XShowAchievementsUI opens that tab.
  - XUserCreateAchievementEnumerator writes the XACHIEVEMENT_DETAILS records (36 bytes) first, then their UTF-16
    strings, sized 36 + (33+100+100)*2 per item.
  - Enumerators that fill themselves use CreateFillEnumerator (xhandle.cpp). XEnumerate calls the fill outside the
    handle lock.
- Tests:
  - `spatest <LP2DX9.exe>` checks the SPA reader. PASS needs 50 named achievements, view 1 rated by 0x2000001D, and
    the skill view with 4 properties.
  - `statstest a <exe>`, then `statstest b <exe>` in a second window. These are two processes through xlive.dll,
    with LP2NET_PROFILE_DIR (a temp profile) and LP2NET_SPA_EXE (the SPA from the exe loaded as a data file). They
    cover sharing (~200 ms), Max aggregation, the by-rank board, the estimate, and an achievement write plus the
    enumerator.
- VERIFIED in game (one copy): a Training Basic map 1 level 1 run writes the stats. The Training menu then shows
  WORLD RECORD 04'11"308, and the Leaderboards screen shows "Total Players : 1" with the player at #1. The overlay
  Achievements tab lists all 50 with their SPA names.
- VERIFIED (two copies): each copy gets the other's rows on hello ("1 rows from StatsTest ..., not played with
  yet"). After Ranked Quick Match joins them, the client saves the host's row (JoinRemote passes the session xuid,
  the same one the stats packets carry).
- VERIFIED (two copies, Ranked Quick Match, Team Elimination, both players idle to the time limit, a draw):
  - Call order at the end, on each copy:
    1. XUserEstimateRankForRating(view 2, -1).
    2. XSessionWriteStats (synchronous) of view 2 for its own player: 0x2000001D = -1, 0x2000001C, 0x20000013 and
       0x2000000B-0x20000011.
    3. The TrueSkill view 0xFFFF0000 (0x1000800A relative score, 0x1000800B) for every player in the session.
       The other player's write is logged as "another player's stats, not kept".
  - Each copy's rows reached the other right after its write, and both saved them.
  - Copy 1's Ranked Match > Leaderboards > Team Elimination shows "Total Players : 2" (both players, plus its own
    row at the bottom). The position reads "---" because the board needs 20 matches, as the screen says.
  - The ranked leaderboard menu is Elimination, Team Elimination, Post Grab: views 1, 2, 3.
  - Overlay with one unlock in achievements.txt: "1 of 50 unlocked, 10 of 1000 G", the unlocked one with its date,
    and secret ones hidden until the checkbox is ticked.
- Not verified: a real achievement unlock in game (it needs campaign progress), and anything across two PCs.

## Gotchas
1. SteamStub: run the exe with SteamAppId/SteamGameId=45750 set (as Steam does); without it, it exits code 53
   after ~16 s. Code is decrypted ~1.2 s after start (used once to make analysis-only copies; never shipped).
2. Ghidra headless from the Claude app: call java directly (tools\ghidra_analyze.bat), own settings dirs, no ".."
   in the project path. Full analysis of LP2DX9 = 25 min, ~6.5 GB heap.
3. Python here: long scratch paths break venv/capstone DLL loading; libs live in `.re\pylib`.
4. A std::thread still running at process exit aborts with 0xC0000409 -> workers are Win32 threads.
5. Firewall: Windows created `lp2dx11` = BLOCK (in, UDP+TCP, Private/Public); `lp2dx9` = Allow. Installer must
   offer to fix (admin + user consent).
6. Computer-use: request access for "lp2dx9.exe" while the game runs. A grant for "Lost Planet 2" resolves to the
   Steam shortcut and the game windows stay masked. Window drags that end over desktop icons are refused.
7. Programs that load xlive.dll (nettest, statstest) use the player's real profile folder unless
   LP2NET_PROFILE_DIR is set. nettest's XSessionJoinRemote once wrote a met-list-only stats.dat there.

## Next
See `ROADMAP.md`: phases 1-3 (hardening, voice chat, installer + CI) run in parallel, then stats and
leaderboards, an optional lobby server, and 1.0.
