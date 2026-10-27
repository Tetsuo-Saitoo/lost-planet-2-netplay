#!/usr/bin/env bash
# Tests for tools/linux/"Install LP2 Netplay.sh" against fake Steam layouts (Steam Deck with an SD card, Flatpak,
# Snap), in terminal mode and in dialog mode (stub kdialog / zenity). Runs on Linux or in Git Bash on Windows:
#   bash tests/linux_install_test.sh <Capcom's 2025 xlive.dll, or any stand-in file> <our xlive.dll>
# The copy under test accepts the first file as "Capcom's" (CI has no game files); a separate check makes sure the
# real script expects the same Capcom SHA-256 as the Windows installer.
set -uo pipefail

capcom_dll="${1:?usage: linux_install_test.sh <Capcom xlive.dll> <our xlive.dll>}"
our_dll="${2:?usage: linux_install_test.sh <Capcom xlive.dll> <our xlive.dll>}"
repo="$(cd "$(dirname "$0")/.." && pwd)"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
pass=0
failed=0

check() {
    if eval "$2"; then
        pass=$((pass + 1))
    else
        failed=$((failed + 1))
        echo "FAIL: $1"
    fi
}
sha() { sha256sum "$1" | cut -d' ' -f1; }

pkg="$work/pkg"
mkdir -p "$pkg"
cp "$repo/tools/linux/"*.sh "$pkg/"
cp "$our_dll" "$pkg/xlive.dll"
sed -i "s/^CAPCOM_SHA=.*/CAPCOM_SHA=$(sha "$capcom_dll")/" "$pkg/Install LP2 Netplay.sh"
shipped="$(sed -n 's/^CAPCOM_SHA=//p' "$repo/tools/linux/Install LP2 Netplay.sh")"
windows="$(sed -n "s/^\\\$CapcomSha = '\\(.*\\)'.*/\\1/p" "$repo/tools/package/install.ps1" | tr 'A-F' 'a-f')"
check "Capcom SHA-256 matches the Windows installer" "[ -n \"$shipped\" ] && [ \"$shipped\" = \"$windows\" ]"
install=("$pkg/Install LP2 Netplay.sh")
uninstall=("$pkg/Uninstall LP2 Netplay.sh")

# A fake game folder with Capcom's stub in it.
new_game() {
    mkdir -p "$1"
    cp "$capcom_dll" "$1/xlive.dll"
    echo exe > "$1/LP2DX9.exe"
}

# ---- layouts, terminal mode ---------------------------------------------------------------------------------------
layout() {  # name, home, game dir
    local name="$1" home="$2" game="$3"
    new_game "$game"
    local out
    out="$(HOME="$home" LP2_NO_GUI=1 bash "${install[@]}" 2>&1)"
    check "$name: install exit 0" "[ $? = 0 ]"
    check "$name: found the game" "grep -qF -- \"$game\" <<< \"\$out\""
    check "$name: ours installed" "[ \"$(sha "$game/xlive.dll")\" = \"$(sha "$our_dll")\" ]"
    check "$name: Capcom kept" "[ \"$(sha "$game/xlive_capcom.dll")\" = \"$(sha "$capcom_dll")\" ]"
    HOME="$home" LP2_NO_GUI=1 bash "${install[@]}" > /dev/null 2>&1
    check "$name: update exit 0" "[ $? = 0 ]"
    HOME="$home" LP2_NO_GUI=1 bash "${uninstall[@]}" > /dev/null 2>&1
    check "$name: uninstall exit 0" "[ $? = 0 ]"
    check "$name: Capcom restored byte-identical" "[ \"$(sha "$game/xlive.dll")\" = \"$(sha "$capcom_dll")\" ]"
    check "$name: no xlive_capcom.dll left" "[ ! -e \"$game/xlive_capcom.dll\" ]"
}

# Steam Deck, game on the SD card, found through libraryfolders.vdf
home="$work/deck"
mkdir -p "$home/.local/share/Steam/steamapps"
cat > "$home/.local/share/Steam/steamapps/libraryfolders.vdf" << EOF
"libraryfolders"
{
	"0"
	{
		"path"		"$home/.local/share/Steam"
	}
	"1"
	{
		"path"		"$work/sdcard"
	}
}
EOF
layout "Deck + SD card" "$home" "$work/sdcard/steamapps/common/Lost Planet 2"

# Flatpak Steam, game in its own library
home="$work/flatpak"
layout "Flatpak" "$home" "$home/.var/app/com.valvesoftware.Steam/.local/share/Steam/steamapps/common/Lost Planet 2"

# Snap Steam
home="$work/snap"
layout "Snap" "$home" "$home/snap/steam/common/.local/share/Steam/steamapps/common/Lost Planet 2"

# ---- refusals -----------------------------------------------------------------------------------------------------
game="$work/foreign/Lost Planet 2"
new_game "$game"
echo foreign > "$game/xlive.dll"
out="$(LP2_GAME_DIR="$game" LP2_NO_GUI=1 bash "${install[@]}" 2>&1)"
check "foreign xlive.dll: exit 1" "[ $? = 1 ]"
check "foreign xlive.dll: untouched" "[ \"\$(cat \"$game/xlive.dll\")\" = foreign ] && [ ! -e \"$game/xlive_capcom.dll\" ]"
check "foreign xlive.dll: says why" "grep -q 'not the version' <<< \"\$out\""

out="$(HOME="$work/nothing" LP2_NO_GUI=1 bash "${install[@]}" 2>&1)"
check "no game: exit 1" "[ $? = 1 ]"
check "no game: says so" "grep -q 'not found' <<< \"\$out\""

game="$work/nodll/Lost Planet 2"
new_game "$game"
mv "$pkg/xlive.dll" "$pkg/xlive.dll.away"
LP2_GAME_DIR="$game" LP2_NO_GUI=1 bash "${install[@]}" > /dev/null 2>&1
check "package without xlive.dll: exit 1" "[ $? = 1 ]"
check "package without xlive.dll: game untouched" "[ \"$(sha "$game/xlive.dll")\" = \"$(sha "$capcom_dll")\" ]"
mv "$pkg/xlive.dll.away" "$pkg/xlive.dll"

# ---- dialog mode (double-click: no terminal), stub kdialog / zenity -----------------------------------------------
stubs="$work/stubs"
mkdir -p "$stubs"
for tool in kdialog zenity; do
    cat > "$stubs/$tool" << 'EOF'
#!/usr/bin/env bash
echo "$(basename "$0") $*" >> "$DIALOG_LOG"
case "$*" in *--yesno*|*--question*) exit "${DIALOG_ANSWER:-0}" ;; esac
exit 0
EOF
    chmod +x "$stubs/$tool"
done

game="$work/gui/Lost Planet 2"
new_game "$game"
export DIALOG_LOG="$work/dialogs.log"
: > "$DIALOG_LOG"
DIALOG_ANSWER=1 PATH="$stubs:$PATH" LP2_GAME_DIR="$game" bash "${install[@]}" < /dev/null > /dev/null 2>&1
check "dialog No: exit 0" "[ $? = 0 ]"
check "dialog No: nothing installed" "[ ! -e \"$game/xlive_capcom.dll\" ]"
check "dialog No: kdialog asked" "grep -q '^kdialog .*--yesno' \"$DIALOG_LOG\""
: > "$DIALOG_LOG"
DIALOG_ANSWER=0 PATH="$stubs:$PATH" LP2_GAME_DIR="$game" bash "${install[@]}" < /dev/null > /dev/null 2>&1
check "dialog Yes: installed" "[ \"$(sha "$game/xlive.dll")\" = \"$(sha "$our_dll")\" ]"
check "dialog Yes: success shown" "grep -q '^kdialog .*--msgbox.*Done' \"$DIALOG_LOG\""
: > "$DIALOG_LOG"
rm "$stubs/kdialog"
DIALOG_ANSWER=0 PATH="$stubs:$PATH" LP2_GAME_DIR="$game" bash "${uninstall[@]}" < /dev/null > /dev/null 2>&1
check "zenity uninstall: restored" "[ \"$(sha "$game/xlive.dll")\" = \"$(sha "$capcom_dll")\" ]"
check "zenity uninstall: asked with zenity" "grep -q '^zenity .*--question' \"$DIALOG_LOG\""
echo foreign > "$game/xlive.dll"
: > "$DIALOG_LOG"
PATH="$stubs:$PATH" LP2_GAME_DIR="$game" bash "${install[@]}" < /dev/null > /dev/null 2>&1
check "zenity refusal: exit 1" "[ $? = 1 ]"
check "zenity refusal: error dialog" "grep -q '^zenity .*--error' \"$DIALOG_LOG\""

echo "linux installer: $pass passed, $failed failed"
[ "$failed" = 0 ]
