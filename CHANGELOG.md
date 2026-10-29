# Changelog

Newest first. Each version's section becomes the notes of its GitHub release: for a tag `v0.2.0-test.1`, CI copies
the section headed `## 0.2.0-test.1`, and a tag without a section fails the build. Add the section before tagging.

## 0.2.0-test.1 (2026-10-28)

Test build: voice chat and stats on top of 0.2.0.

### New
- In-game voice chat through Steam's microphone. Overlay (Home) > Voice mutes a player.
- Stats, leaderboards and achievements, kept on your PC and shared with the people you play with. The game's
  leaderboards show you and them; an Achievements tab in the overlay lists what you have earned.
- Drop log: if the game leaves a match by itself, lp2net.log says which code started the leave.
- The game no longer leaves a match after a long stall on its own side (FreezeGuard, on by default).
- Crash log: if the game crashes, lp2net.log ends with a "crash:" line that names the code involved.

### Known issues
- Accepting a Steam invite or "Join Game" for an online game crashes the game. Join with the game's Quick
  Search instead for now.
- Some players saw a little lag or desync in co-op.

### Testing
Follow TWO-PC TEST.txt in the zip, then send lp2net.log from both PCs.

## 0.2.0 (2026-10-27)

### New
- LP2NetSetup.exe: one-click install, repair, update and uninstall, with a firewall check.
- Linux and Steam Deck installer (the game runs through Proton).
- The overlay's join prompt names the exact menu to use for a System Link game; a rank estimate after a Ranked
  Match.

### Fixed
- Players who connect through Steam try a direct connection first and only leave Steam's relay once it works
  both ways (players were dropped after a few minutes when only one direction worked).

## 0.1.0-test

First test build: online and System Link play through Steam (friends, invites, Steam's relay servers).
