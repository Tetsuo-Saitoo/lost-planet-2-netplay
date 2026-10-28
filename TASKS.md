# LP2 Netplay: tasks and state

Read this (and WORKLOG.md) before exploring the code. Update both when you finish something.
Details and findings: MODLOG.md (by phase). Plan: ROADMAP.md. Rules for agents: ROADMAP.md "Working rules".

## State (2026-10-28)
| Phase | Branch / commit | State |
|---|---|---|
| 1 hardening | main `6474c41` (pushed, CI green) | 0.2.0 + freeze check / FreezeGuard / session-leave log |
| 2 voice | `claude/phase2-voice-f1698a` `da540a5` (not pushed, not in main) | works on 2 real PCs; includes main |
| 3 installer, CI | in main (`e461c9e`) | done |
| 4 stats, achievements | `claude/wonderful-taussig-d6cb4d` `a0abe7b` (not pushed, not in main) | 1 and 2 copies OK |
| 5 internet without Steam | - | not started (optional) |
| 6 1.0 | - | after 1-4, 7 |
| 7 Linux / Steam Deck | in main | installer done, never run on real hardware |

Installed on this PC and test-bed known-good: package build `da540a5`, xlive.dll sha256 `C6DDF5DA...3FF5`.
Player package: `.claude\worktrees\phase2-voice-f1698a\dist\LP2Netplay-0.2.0.zip` (+ `-linux.tar.gz`).

## Open tasks (this week, in order)
1. Family two-PC test with the new package (user). If anyone drops: note the time, collect lp2net.log from both
   PCs; the "session leave:" line names the code that started the leave (callers in MODLOG, Phase 1).
2. Merge phase 2 (voice) into main after that test. Needs the user's OK.
3. Review and merge phase 4 into main. Needs the user's OK. Still untested: 2 separate PCs, a real achievement.
   Merge points with the voice branch: phase 4 session's side note `phase4-stats.md`.
4. Phase 7: try on a real Steam Deck / Linux PC (needs someone with one).
5. Unexplained: two copies on one PC dropped after ~4 min (before 0.2.0). Watch the new logs.
6. Proximity voice (user asked): run `[Debug] VoiceProbe=1` in a 2-player mission to find player positions, then
   pan/attenuate. Plan: MODLOG "Proximity voice (requested next)". Owner: voice session.
7. Phase 3 leftovers (installer session): code signing, opt-in update check, remove firewall rules on Uninstall,
   the harmless error `testbed.ps1 Release` prints.
8. Uncommitted in main: ROADMAP.md "Later: modern upgrades and accessibility" section (user's call).

## How to (no code reading needed)
- Build: `tools\build.bat` -> build\xlive.dll, LP2NetSetup.exe. Package: `tools\make_package.ps1` -> dist\.
- Test bed: `tools\testbed.ps1 Status|Acquire|Wait|Release|SetKnownGood -Owner <name>`; Release restores the
  known-good DLL and lp2net.ini / config.ini. Never touch a running game without the lock.
- Two copies: `[Debug] AllowMultipleCopies=1`, `tools\run_two.ps1`, windowed via config.ini (Release restores).
- Offline tests: nettest host/client; voicetest a/b (README "Voice test"); tests\setup_test.ps1;
  phase 4: `spatest <LP2DX9.exe>`, `statstest a|b <LP2DX9.exe>` (README; temp profile).
- Run .ps1 with `powershell -File` (not -ExecutionPolicy Bypass). Push from the Terminal panel (stored login).
- Game RE: Ghidra project re-work\ghidra\LP2Net (headless, read-only). Never commit re-work\.

## Rules the user set
- Short text replies (limited weekly tokens). Commit, push, merge, tag only when asked.
- No personal data (names, gamertags, IPs, SteamIDs, user paths) in committed files.
