#!/usr/bin/env bash
# Put Capcom's original xlive.dll back (see install-steamdeck.sh).
exec bash "$(cd "$(dirname "$0")" && pwd)/install-steamdeck.sh" --uninstall
