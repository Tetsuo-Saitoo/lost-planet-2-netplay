#!/usr/bin/env bash
# LP2 Netplay for Linux and Steam Deck. Lost Planet 2 runs through Proton there, which loads the game folder's
# xlive.dll, so this installs the same xlive.dll as on Windows and keeps Capcom's original as xlive_capcom.dll.
# It only replaces Capcom's 2025 file (checked by SHA-256) or an earlier LP2 Netplay.
#
# Double-click it (it shows dialogs), or run it in a terminal:
#   bash "Install LP2 Netplay.sh"                install / update
#   bash "Install LP2 Netplay.sh" --uninstall    put Capcom's original back
# Finds Steam installed natively (Steam Deck, SteamOS, Bazzite, most distros), as a Flatpak or as a Snap, and every
# library listed in Steam's libraryfolders.vdf (SD cards, other drives). LP2_GAME_DIR=/path/to/"Lost Planet 2"
# skips the search.
set -euo pipefail

CAPCOM_SHA=867bfe461a0562aee0a43cd832a52b228ac6da4e4216bdfbb94ef14bece85366
TITLE="LP2 Netplay"
here="$(cd "$(dirname "$0")" && pwd)"
uninstall=0
[ "${1:-}" = "--uninstall" ] && uninstall=1

# ---- talking to the player: a terminal, or dialogs when the script was double-clicked ---------------------------
gui=""
if [ ! -t 0 ] && [ ! -t 1 ] && [ -z "${LP2_NO_GUI:-}" ]; then
    if command -v kdialog > /dev/null; then
        gui=kdialog
    elif command -v zenity > /dev/null; then
        gui=zenity
    elif [ -z "${LP2_IN_TERMINAL:-}" ]; then
        # no dialog tool: open a terminal window for the output instead
        for term in konsole gnome-terminal xfce4-terminal x-terminal-emulator xterm; do
            command -v "$term" > /dev/null || continue
            export LP2_IN_TERMINAL=1
            cmd=(bash "$0" "$@")
            case "$term" in
            gnome-terminal) exec "$term" -- "${cmd[@]}" ;;
            *) exec "$term" -e "${cmd[@]}" ;;
            esac
        done
    fi
fi

say() {
    case "$gui" in
    kdialog) kdialog --title "$TITLE" --msgbox "$1" || true ;;
    zenity) zenity --info --title "$TITLE" --width 420 --text "$1" || true ;;
    *) printf '%s\n' "$1" ;;
    esac
}

ask() {
    case "$gui" in
    kdialog) kdialog --title "$TITLE" --yesno "$1" ;;
    zenity) zenity --question --title "$TITLE" --width 420 --text "$1" ;;
    *) return 0 ;;  # run from a terminal: typing the command was the answer
    esac
}

finish() {
    if [ -n "${LP2_IN_TERMINAL:-}" ]; then
        read -r -p "Press Enter to close this window." _ || true
    fi
    exit "$1"
}

fail() {
    case "$gui" in
    kdialog) kdialog --title "$TITLE" --error "$1" || true ;;
    zenity) zenity --error --title "$TITLE" --width 420 --text "$1" || true ;;
    *) printf '\nERROR: %s\n' "$1" ;;
    esac
    finish 1
}

# ---- finding the game -----------------------------------------------------------------------------------------
find_game() {
    local steams=(
        "$HOME/.local/share/Steam"
        "$HOME/.steam/steam"
        "$HOME/.steam/root"
        "$HOME/.var/app/com.valvesoftware.Steam/.local/share/Steam"
        "$HOME/.var/app/com.valvesoftware.Steam/data/Steam"
        "$HOME/snap/steam/common/.local/share/Steam"
    )
    local libs=() steam vdf lib
    for steam in "${steams[@]}"; do
        [ -d "$steam" ] || continue
        libs+=("$steam")
        for vdf in "$steam/steamapps/libraryfolders.vdf" "$steam/config/libraryfolders.vdf"; do
            [ -f "$vdf" ] || continue
            while IFS= read -r lib; do
                libs+=("$lib")
            done < <(sed -n 's/^[[:space:]]*"path"[[:space:]]*"\(.*\)"[[:space:]]*$/\1/p' "$vdf")
        done
    done
    for lib in "${libs[@]}"; do
        if [ -f "$lib/steamapps/common/Lost Planet 2/LP2DX9.exe" ]; then
            echo "$lib/steamapps/common/Lost Planet 2"
            return 0
        fi
    done
    return 1
}

game="${LP2_GAME_DIR:-}"
if [ -z "$game" ]; then
    game="$(find_game)" || fail "Lost Planet 2 was not found in your Steam libraries.
Install it from Steam, start it once, quit, and run this again."
fi
xlive="$game/xlive.dll"
capcom="$game/xlive_capcom.dll"

if command -v pgrep > /dev/null && pgrep -fi 'lp2dx(9|11)\.exe' > /dev/null; then
    fail "Lost Planet 2 is running. Quit the game, then run this again."
fi

# ---- uninstall --------------------------------------------------------------------------------------------------
if [ "$uninstall" = 1 ]; then
    if [ ! -f "$capcom" ]; then
        say "LP2 Netplay is not installed in:
$game"
        finish 0
    fi
    ask "Remove LP2 Netplay and put Capcom's original xlive.dll back in:
$game" || finish 0
    mv -f "$capcom" "$xlive"
    say "Done: LP2 Netplay is removed and Capcom's original xlive.dll is back."
    finish 0
fi

# ---- install / update -------------------------------------------------------------------------------------------
[ -f "$here/xlive.dll" ] || fail "xlive.dll is missing next to this script. Unpack the whole download first."
if [ ! -f "$capcom" ]; then
    [ -f "$xlive" ] || fail "The game has no xlive.dll. In Steam: Lost Planet 2 > Properties > Installed Files >
Verify integrity of game files, then run this again."
    sha="$(sha256sum "$xlive" | cut -d' ' -f1)"
    [ "$sha" = "$CAPCOM_SHA" ] || fail "The game's xlive.dll is not the version LP2 Netplay was made for
(sha256 $sha). Let Steam update the game (or verify its files) and run this again."
    action="Install"
else
    action="Update"
fi
ask "$action LP2 Netplay in:
$game

Capcom's original xlive.dll is kept as xlive_capcom.dll." || finish 0
if [ "$action" = "Install" ]; then
    mv "$xlive" "$capcom"
fi
cp -f "$here/xlive.dll" "$xlive"
say "Done: LP2 Netplay is installed in:
$game

Start Lost Planet 2 from Steam as usual. Your Steam name is your name in game.
If the game folder has no lp2net.log after you played, add this in Steam > Lost Planet 2 > Properties >
Launch Options:   WINEDLLOVERRIDES=\"xlive=n,b\" %command%"
finish 0
