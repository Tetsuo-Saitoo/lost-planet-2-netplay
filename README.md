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
- **Linux and Steam Deck**: the game runs there through Proton, which loads the same `xlive.dll`; a Linux
  installer (double-click, with dialogs) finds the game in native, Flatpak and Snap Steam and on SD cards. Linux,
  Steam Deck and Windows players play together.
- **Several copies on one PC** (for testing): each copy gets its own ports and player identity.
- Saves, profile settings and everything else are handled by Capcom's own stub, which is kept as
  `xlive_capcom.dll`.

Status: early development. In-game System Link works between two PCs on the same home network: one hosted a
Campaign game, the other found it with Quick Match, and they played through the first mission of Episode 1
(about 15 minutes, no crash). The game's own Online menus work with two copies on one PC: Player Match (Create
Match, the other copy's Custom Match search lists it, join, versus battle) and Campaign with the LIVE
connection type (`docs/online-player-match-2-copies.jpg`, `docs/online-campaign-live-2-copies.jpg`), Ranked Match
(arbitration, a full battle) and Versus over System Link. Two PCs with their own Steam accounts found each other
and joined through Steam; 0.2 fixes a drop seen there (PCs on one network stayed on Steam's relay). Not yet tried:
two PCs on different networks, Linux / Steam Deck on real hardware.
See `MODLOG.md` for the full engineering log.

## Installing (players)

You need Lost Planet 2 on Steam, on your own Steam account, fully updated.

1. Download `LP2NetSetup.exe` from the [Releases](../../releases) page (one file; the zip next to it has the same
   installer plus `HOW TO TEST.txt`).
2. Quit the game, then double-click `LP2NetSetup.exe`. Windows may say "Windows protected your PC" because the file
   is not signed: click **More info**, then **Run anyway**.
3. Setup finds the game and shows what is installed. Click **Install**. It keeps Capcom's `xlive.dll` as
   `xlive_capcom.dll` and only ever replaces Capcom's 2025 file or an older LP2 Netplay, nothing else.
   - Optional: tick the box to move the old Games for Windows LIVE files (`gfwlivesetup.exe`, `*.exe.cat`,
     `*.exe.cfg`) to a backup folder. Setup lists what it found first; Uninstall puts them back.
   - If Setup says Windows Firewall blocks the game, click **Allow through firewall** (Windows asks for permission
     first), or click Allow when Windows asks the first time the game goes online.
4. Start the game from Steam as usual. `HOW TO TEST.txt` explains how to host and join.

To update or repair, run the newer `LP2NetSetup.exe` and click **Update** / **Repair**. To remove it, run
`LP2NetSetup.exe` and click **Uninstall** (or let Steam verify the game files). If the exe is blocked, the zip's
`manual` folder has the older script installer. Problems: the log is `lp2net.log` in the game folder (Steam >
right-click Lost Planet 2 > Manage > Browse local files).

### Linux and Steam Deck

Lost Planet 2 runs through Proton there. Download `LP2Netplay-<version>-linux.tar.gz` from the Releases page
(the zip has the same files in its `linux` folder), unpack it, and double-click `Install LP2 Netplay.sh` (Steam
Deck: in Desktop Mode). It finds the game in native Steam (Steam Deck, SteamOS, Bazzite, most distros), Flatpak or
Snap Steam, on internal storage, SD cards and other drives, keeps Capcom's file as `xlive_capcom.dll` and asks
before changing anything. `LINUX AND STEAM DECK.txt` has the steps, the terminal alternative and where the log
is. Not tried on a real Deck yet: reports welcome.

## Releases and the package

The version number lives in one place, `project(VERSION ...)` in `CMakeLists.txt`; it ends up in both files'
version resources, the first line of `lp2net.log`, the overlay title and the package name.
`powershell -File tools\make_package.ps1` (after a build) makes `dist\LP2Netplay-<version>.zip`: `LP2NetSetup.exe`,
`HOW TO TEST.txt`, `VERSION.txt` (commit and SHA-256s), the script installer in `manual\` and the Linux / Steam
Deck installer in `linux\`, plus `dist\LP2Netplay-<version>-linux.tar.gz`. Python (`tools\pack_dist.py`) writes
both archives so the Linux scripts stay executable. `bash tests/linux_install_test.sh <Capcom xlive.dll or a
stand-in> build/xlive.dll` tests the Linux installer against fake Steam layouts.

GitHub Actions (`.github/workflows/build.yml`) builds and packages every push and pull request (the package is the
run's artifact). Pushing a tag `v<version>` (or `v<version>-something`, marked pre-release) also creates a draft
release with `LP2NetSetup.exe`, the zip and the Linux tarball; check it and publish it by hand.

## Building

Needs Visual Studio 2022 Build Tools (C++ workload). Clone with the Dear ImGui submodule, then build from a
terminal:

```
git clone --recursive https://github.com/Tetsuo-Saitoo/lost-planet-2-netplay.git
cd lost-planet-2-netplay
tools\build.bat
```

Output: `build\xlive.dll` (x86), `build\LP2NetSetup.exe` (the players' installer, with `xlive.dll` inside) and
`build\nettest.exe`. `tools\build.bat` uses the VS 2022 Build Tools, or else the newest Visual Studio that vswhere
finds.

## Developer install

```
powershell -File tools\dev_install.ps1             # install (keeps Capcom's stub as xlive_capcom.dll)
powershell -File tools\dev_install.ps1 -Uninstall  # put Capcom's stub back
powershell -File tools\run_lp2.ps1 [-Dx11]         # start the game directly; prints the PID
```

Settings live in `lp2net.ini` next to the game exe, the log in `lp2net.log` (`lp2net.2.log` for a second copy).

## Transport test and installer test

```
cd build
nettest host        (in one terminal)
nettest client      (in another; prints PASS or FAIL)
```

`powershell -File tests\setup_test.ps1` runs `LP2NetSetup.exe` against fake game folders in `%TEMP%` (install,
repair, update, uninstall, the GFWL backup, the refuse cases). It needs Capcom's stub from a local install and never
touches the real game folder or the firewall. `LP2NetSetup.exe --help` lists the command-line options.

## Layout

- `src/xlive/` the DLL: `net.*` (peers, virtual addresses, packet header, core socket), `xsocket.cpp`, `xnet.cpp`,
  `xsession.cpp`, `xuser.cpp`, `xnotify.cpp`, `xhandle.cpp`, `overlay.cpp`, `presence.cpp` (where the player is,
  System Link prompt), `xui.cpp` (Guide screens -> Steam overlay), `xstorage.cpp` / `xstats.cpp` (LIVE storage
  and stats, online mode), `matchmaking.cpp` + `lp2_queries.inc` (session search with the game's own queries),
  `steam.cpp` (Steam client: name, lobbies, invites, relay, rich presence), `instances.cpp` (input gating,
  multi-copy), `upnp.cpp`, `gamelog.cpp`,
  `exports.def` + `forwards.cpp` (ordinal table; the rest goes to Capcom's stub), `xlive.rc` (version resource)
- `src/setup/` LP2NetSetup.exe: `setup.cpp` (find the game, install / uninstall, GFWL backup, settings),
  `firewall.cpp` (status, and the fix behind the button), `main.cpp` (window and command line)
- `src/version.h.in`, `src/versioninfo.rc` the version number and the shared version resource
- `tools/` build, package, dev install, game launcher, Ghidra headless scripts
- `tests/` transport test, installer test
- `.github/workflows/build.yml` CI build, package artifact, draft release on tags
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
