# LP2 Netplay roadmap

v0.1.0-test works: System Link on a home network, the game's own Online menus (Player Match, Campaign over
LIVE), Steam names, lobbies and Steam Join Game / invites between two PCs. The phases below take it to a 1.0
that a family can install and play without help. Phases 1-3 run at the same time, each on its own branch;
the others wait for the phase they depend on.

| Phase | What | Depends on |
|---|---|---|
| 1 | Hardening from real play (v0.2) | - |
| 2 | Voice chat | - |
| 3 | Installer, CI builds and releases | - |
| 4 | Stats and leaderboards | 1 |
| 5 | Internet play without Steam (optional) | 1 |
| 6 | 1.0: integration test with real players, docs, release | 1-4 |

## Phase 1: hardening (v0.2)
- The idle Player Match that cancelled itself after about 4 minutes: inactivity kick or our bug.
- Versus over System Link and online between two PCs; what Ranked Match does (works, or fails cleanly).
- The other calls the game still sends to Capcom's stub that touch multiplayer (voice and mute are phase 2).
- DLC (XLiveContentCreateEnumerator): DLC maps and characters in mixed lobbies.
- Achievements still go to Capcom's stub: check they still unlock.
- Overlay with a controller; the second-copy save folder name; log noise.

## Phase 2: voice chat
- XHVCreateEngine and the IXHVEngine interface: microphone capture, encoding, playback, talking indicators,
  mute (XUserMuteListQuery). The game carries voice packets itself (GetLocalChatData / SubmitIncomingChatData), if LP2 uses that
  path. Steam's voice API does capture and Opus encoding when Steam is running.
- Settings: on/off, push-to-talk key, volume, in lp2net.ini and the overlay.

## Phase 3: installer, CI and releases
- LP2NetSetup.exe: install, repair, uninstall; keeps Capcom's stub; optional GFWL leftover cleanup with a
  backup; firewall rule only on the player's click.
- One version number (CMake) in the DLL's version resource, the log and the overlay.
- GitHub Actions: build on every push, a draft release with the zip on every tag.

## Phase 4: stats and leaderboards
- XUserWriteStats / XSessionWriteStats kept locally and shared with peers, so the game's player stats and
  leaderboards show you and the people you play with.

## Phase 5: internet without Steam (optional)
- A small lobby server for session listing and UDP hole punching, for players who don't run Steam.

## Phase 6: 1.0
- Merge, full test with real players on separate PCs and networks, README for players, release.

## Working rules (for the agents building this)
- One branch per phase, in its own git worktree. Commit to your branch; don't push, don't merge into `main`.
- One game install, one screen: take the test-bed lock before installing a DLL into the game, starting the
  game or driving its window (`tools\testbed.ps1`), and release it as soon as the test is done. Release puts
  back the build the user plays with. While the lock is taken, keep working on code and logs.
- Never touch the game while it runs without the lock: the user may be playing.
- `re-work\` holds analysis copies of the game: read them, never commit or ship them. No DRM bypass.
- Prefer log-based checks; the PC is shared with other work, so keep heavy jobs (Ghidra) rare.
- Write what you learn into `MODLOG.md`, under a heading of your own phase.
