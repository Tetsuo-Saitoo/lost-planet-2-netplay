#!/usr/bin/env bash
# LP2 Netplay on Steam Deck / Linux (the game runs through Proton, so it uses the same Windows xlive.dll).
# Installs xlive.dll into Lost Planet 2's folder and keeps Capcom's original as xlive_capcom.dll. It only replaces
# Capcom's 2025 file (checked by SHA-256) or an earlier LP2 Netplay. Run it from Desktop Mode:
#   bash install-steamdeck.sh                install / update
#   bash install-steamdeck.sh --uninstall    put Capcom's original back
# LP2_GAME_DIR=/path/to/"Lost Planet 2" overrides the search (used by the tests).
set -euo pipefail

CAPCOM_SHA=867bfe461a0562aee0a43cd832a52b228ac6da4e4216bdfbb94ef14bece85366
here="$(cd "$(dirname "$0")" && pwd)"

fail() {
    echo
    echo "ERROR: $*"
    exit 1
}

# Every Steam library (internal storage, SD card), from Steam's own list.
find_game() {
    local roots=() vdf lib
    for vdf in "$HOME/.local/share/Steam/steamapps/libraryfolders.vdf" "$HOME/.steam/steam/steamapps/libraryfolders.vdf"; do
        [ -f "$vdf" ] || continue
        while IFS= read -r lib; do
            roots+=("$lib")
        done < <(sed -n 's/^[[:space:]]*"path"[[:space:]]*"\(.*\)"[[:space:]]*$/\1/p' "$vdf")
    done
    roots+=("$HOME/.local/share/Steam" "$HOME/.steam/steam")
    for lib in "${roots[@]}"; do
        if [ -f "$lib/steamapps/common/Lost Planet 2/LP2DX9.exe" ]; then
            echo "$lib/steamapps/common/Lost Planet 2"
            return 0
        fi
    done
    return 1
}

game="${LP2_GAME_DIR:-}"
if [ -z "$game" ]; then
    game="$(find_game)" || fail "Lost Planet 2 was not found in your Steam libraries. Is it installed?"
fi
xlive="$game/xlive.dll"
capcom="$game/xlive_capcom.dll"
echo "Lost Planet 2: $game"

if command -v pgrep > /dev/null && pgrep -fi 'lp2dx(9|11)\.exe' > /dev/null; then
    fail "Lost Planet 2 is running. Quit the game, then run this again."
fi

if [ "${1:-}" = "--uninstall" ]; then
    if [ -f "$capcom" ]; then
        mv -f "$capcom" "$xlive"
        echo "Done: Capcom's original xlive.dll is back."
    else
        echo "Nothing to uninstall (LP2 Netplay is not installed here)."
    fi
    exit 0
fi

[ -f "$here/xlive.dll" ] || fail "xlive.dll is missing next to this script. Unzip the whole package first."
if [ ! -f "$capcom" ]; then
    [ -f "$xlive" ] || fail "The game has no xlive.dll. Let Steam verify the game files, then try again."
    sha="$(sha256sum "$xlive" | cut -d' ' -f1)"
    if [ "$sha" != "$CAPCOM_SHA" ]; then
        fail "The game's xlive.dll is not the version this was made for (sha256 $sha).
Let Steam update the game (or verify its files) and try again."
    fi
    mv "$xlive" "$capcom"
    echo "Kept Capcom's original as xlive_capcom.dll"
fi
cp -f "$here/xlive.dll" "$xlive"
echo "Done: LP2 Netplay is installed."
echo
echo "One more step in Steam (only once): Lost Planet 2 > Properties > General > Launch Options, enter"
echo '    WINEDLLOVERRIDES="xlive=n,b" %command%'
echo "so Proton always uses this xlive.dll."
