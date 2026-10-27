#!/usr/bin/env bash
# Put Capcom's original xlive.dll back (see "Install LP2 Netplay.sh").
exec bash "$(cd "$(dirname "$0")" && pwd)/Install LP2 Netplay.sh" --uninstall
