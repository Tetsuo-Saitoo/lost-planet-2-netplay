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
| 4 | Stats, leaderboards and achievements | 1 |
| 5 | Internet play without Steam (optional) | 1 |
| 6 | 1.0: integration test with real players, docs, release | 1-4, 7 |
| 7 | Linux and Steam Deck (priority: players are asking for it) | 3 |

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

## Phase 4: stats, leaderboards and achievements
- XUserWriteStats / XSessionWriteStats kept locally and shared with peers, so the game's player stats and
  leaderboards show you and the people you play with.
- Achievements: LP2 on Steam has none, and Capcom's stub drops the game's unlocks. Keep them locally (names from
  the game's SPA) and show them in the overlay.

## Phase 5: internet without Steam (optional)
- A small lobby server for session listing and UDP hole punching, for players who don't run Steam.

## Phase 7: Linux and Steam Deck
- Done in 0.2: Linux installer (`tools/linux`, double-click with dialogs, native / Flatpak / Snap Steam, SD cards),
  shipped in the zip and as a `-linux.tar.gz`, tested on fake Steam layouts in CI.
- Run it on a real Steam Deck and a Linux PC: does Proton load our xlive.dll without the launch option, does
  Proton's Steam client (lsteamclient) answer our Steam interfaces (names, lobbies, invites, relay), UPnP under
  Wine, mixed Windows / Deck games.
- Deck controls: the overlay on the View button and the Deck's screen size, text entry (Join by IP) with the
  Steam on-screen keyboard, Gaming Mode friendly prompts.
- Optional later: a Decky Loader plugin or a Steam Deck "Install" shortcut that runs from Gaming Mode.

## Phase 6: 1.0
- Merge, full test with real players on separate PCs and networks, README for players, release.

## Later: modern upgrades and accessibility (separate mods, after 1.0)
Separate from LP2 Netplay, each switchable on its own. Like LP2 Netplay they load alongside the game and change it
at runtime: the decrypted exe is for reading only, and no modified game file is ever shipped.
- Video: modern rendering path (e.g. DXVK), widescreen / ultrawide, better anti-aliasing, sharper shadows,
  upscaling. Higher frame rates only after checking whether game speed is tied to the frame rate.
- Voice to text: local speech recognition on voice chat (nothing sent to the cloud), captions in the overlay,
  optionally into the game's text chat.
- Input: detect the device in use (keyboard and mouse, Xbox, PlayStation, Steam Deck) and switch button prompts
  and layouts to match, per device; free remapping.
- Accessibility:
  - seeing: colorblind modes, high-contrast outlines, larger HUD and menu text, less shake, flashing and blur;
  - hearing: cutscene and voice captions, on-screen cues for important sounds, mono audio;
  - movement: one-handed layouts, toggle instead of hold, dead zones, adaptive controllers, slower or skippable
    quick-time events;
  - focus: slower game speed, objective reminders.
- Fairness rule: changes to what a player sees or hears are fine everywhere. Changes to how the game plays (speed,
  aim assist, quick-time event help) are single player only, or need every player in a multiplayer game to agree.

## Working rules (for the agents building this)
- One branch per phase, in its own git worktree. Commit to your branch; don't push, don't merge into `main`.
- One game install, one screen: take the test-bed lock before installing a DLL into the game, starting the
  game or driving its window (`tools\testbed.ps1`), and release it as soon as the test is done. Release puts
  back the build the user plays with. While the lock is taken, keep working on code and logs.
- Never touch the game while it runs without the lock: the user may be playing.
- `re-work\` holds analysis copies of the game: read them, never commit or ship them. No DRM bypass.
- Prefer log-based checks; the PC is shared with other work, so keep heavy jobs (Ghidra) rare.
- Write what you learn into `MODLOG.md`, under a heading of your own phase.
