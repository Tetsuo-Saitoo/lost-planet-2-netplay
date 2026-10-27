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

## Multi-copy testing ([Debug] AllowMultipleCopies=1, off by default)
- Single-instance check: CreateMutexA(NULL, TRUE, "sPlanetMain") + FindWindowW -> hooked via the game's IAT.
- Keyboard reaches every copy (DirectInput background mode + GetAsyncKeyState) -> background copies get zeroed
  keyboard/mouse state (gamepads untouched).
- tools\run_two.ps1 starts both and lays them out; tools\focus_lp2.ps1 -Index N gives one the keyboard (Alt tap +
  SetForegroundWindow; clicking a game window does not reliably activate it).

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

## Next
See `ROADMAP.md`: phases 1-3 (hardening, voice chat, installer + CI) run in parallel, then stats and
leaderboards, an optional lobby server, and 1.0.
