# LP2 Netplay

A replacement for the Games for Windows LIVE layer in **Lost Planet 2 (Steam)**. When Capcom removed GFWL in 2025
it shipped a dummy `xlive.dll` that answers every network call with "nothing here", which left even System Link
dead. LP2 Netplay is a new `xlive.dll` that implements that layer for real:

- **LAN play**: System Link works between PCs on the same network.
- **The game's own Online menus**: Player Match (and the rest of the Online menu) work again. Hosts are found by
  the game's own Quick/Custom Match search, over the LAN and through Steam lobbies, with the game's matchmaking
  filters. The online save LIVE used to keep is stored next to your save.
- **Steam**: your Steam name in game, Steam friends playing LP2 in the overlay, the lobby's "Game invite" and
  "Gamer card" open the Steam overlay, Steam invites and "Join Game" bring you into the host's game, rich presence,
  and Steam's relay servers when a direct connection fails. Uses the Steam client you already run; no Valve files
  ship with the mod.
- **Internet play without Steam**: add a friend's address under *Join by IP*, and the game's own System Link
  search finds their games. UPnP opens the host's ports automatically when the router allows it.
- **Overlay** (Home key, or hold Back on a controller): session browser, join by IP, gamertag and network
  settings, in place of the old GFWL Guide. Text scales with the game's resolution; the game gets no input while
  it is open.
- **System Link prompt**: the first time you reach the Campaign or System Link menu, a short card explains that
  System Link now works over the internet too and who is online.
- **Several copies on one PC** (for testing): each copy gets its own ports and player identity.
- Saves, profile settings and everything else are handled by Capcom's own stub, which is kept as
  `xlive_capcom.dll`.

Status: early development. In-game System Link works between two PCs on the same home network: one hosted a
Campaign game, the other found it with Quick Match, and they played through the first mission of Episode 1
(about 15 minutes, no crash). The game's own Online menus work with two copies on one PC: Player Match (Create
Match, the other copy's Custom Match search lists it, join, versus battle) and Campaign with the LIVE
connection type (`docs/online-player-match-2-copies.jpg`, `docs/online-campaign-live-2-copies.jpg`). Not yet
tried: two PCs over the internet, Steam invites / Join Game / relay between two Steam accounts, Ranked Match.
The installer is a test script for now. See `MODLOG.md` for the full engineering log.

## Installing (players)

You need Lost Planet 2 on Steam, on your own Steam account, fully updated.

1. Download `LP2Netplay-test.zip` from the [Releases](../../releases) page and unzip it anywhere.
2. Quit the game, then double-click `Install LP2 Netplay.bat`. It finds the game, keeps Capcom's `xlive.dll` as
   `xlive_capcom.dll` and installs LP2 Netplay. It only replaces Capcom's 2025 file, nothing else.
3. Start the game from Steam as usual. `HOW TO TEST.txt` in the zip explains how to host and join.

To remove it, run `Uninstall LP2 Netplay.bat` (or let Steam verify the game files). Problems: the log is
`lp2net.log` in the game folder (Steam > right-click Lost Planet 2 > Manage > Browse local files).

## Building the test package

`powershell -File tools\make_package.ps1` builds `dist\LP2Netplay-test.zip`: the DLL, an install/uninstall
script, a firewall helper and `HOW TO TEST.txt`.

## Building

Needs Visual Studio 2022 Build Tools (C++ workload). Clone with the Dear ImGui submodule, then build from a
terminal:

```
git clone --recursive https://github.com/saitotetsuo-coder/lost-planet-2-netplay.git
cd lost-planet-2-netplay
tools\build.bat
```

Output: `build\xlive.dll` (x86), `build\nettest.exe` and `build\voicetest.exe`.

## Developer install

```
powershell -File tools\dev_install.ps1             # install (keeps Capcom's stub as xlive_capcom.dll)
powershell -File tools\dev_install.ps1 -Uninstall  # put Capcom's stub back
powershell -File tools\run_lp2.ps1 [-Dx11]         # start the game directly; prints the PID
```

Settings live in `lp2net.ini` next to the game exe, the log in `lp2net.log` (`lp2net.2.log` for a second copy).

## Transport test

```
cd build
nettest host        (in one terminal)
nettest client      (in another; prints PASS or FAIL)
```

## Voice test

Two processes talk through the voice engine without the game: put an `lp2net.ini` next to `voicetest.exe` with
`[Network] Peers=127.0.0.1:3075,127.0.0.1:3175`, `[Steam] Enabled=0` and `[Debug] VoiceFeedWav=<a speech WAV>`,
`VoiceDumpWav=<out.wav>`, then run `voicetest a` and `voicetest b` at the same time (each prints PASS or FAIL;
the dumps hold what each side played).

## Layout

- `src/xlive/` the DLL: `net.*` (peers, virtual addresses, packet header, core socket), `xsocket.cpp`, `xnet.cpp`,
  `xsession.cpp`, `xuser.cpp`, `xnotify.cpp`, `xhandle.cpp`, `overlay.cpp`, `presence.cpp` (where the player is,
  System Link prompt), `xui.cpp` (Guide screens -> Steam overlay), `xstorage.cpp` / `xstats.cpp` (LIVE storage
  and stats, online mode), `matchmaking.cpp` + `lp2_queries.inc` (session search with the game's own queries),
  `steam.cpp` (Steam client: name, lobbies, invites, relay, rich presence), `instances.cpp` (input gating,
  multi-copy), `upnp.cpp`, `gamelog.cpp`, `xhv.cpp` (voice chat: the game's XHV engine, mute list),
  `exports.def` + `forwards.cpp` (ordinal table; the rest goes to Capcom's stub)
- `tools/` build, dev install, game launcher, Ghidra headless scripts
- `tests/` transport test, voice test
- `third_party/imgui` Dear ImGui 1.91.9 (MIT), as a git submodule
- `re-work/` (git-ignored) backups, analysis copies, Ghidra projects, decompiler output. Never commit or share.

## Credits

- [Dear ImGui](https://github.com/ocornut/imgui) by Omar Cornut (MIT) draws the overlay.
- The Steam interface layouts follow Valve's published Steamworks headers (as in Valve's Source SDK 2013
  repository). No Valve code or binaries are included: the mod talks to the Steam client already installed.
- Built with [Claude Code](https://claude.com/claude-code) (Anthropic's AI coding agent, Claude Opus 5.5): reverse
  engineering, code, in-game testing and these docs, directed and play-tested by the repository owner and family.

## License and disclaimer

The code in this repository is MIT licensed (see `LICENSE`); Dear ImGui keeps its own MIT license.

LP2 Netplay is a fan project, not affiliated with or endorsed by Capcom, Microsoft or Valve. Lost Planet 2 is a
trademark of Capcom. No game files are included or distributed: everything installs onto your own copy of the
game, and Capcom's own `xlive.dll` stays on your PC as `xlive_capcom.dll`.
