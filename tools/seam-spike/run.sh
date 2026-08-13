#!/bin/sh
# Run the seam spike inside a throwaway CrossOver bottle and print PASS/FAIL.
#
# The deployment model this exercises is exactly the production one for Level B:
#   - the builtin PE lives at a REAL path Wine will load (here: system32; in
#     production: the game dir or the SteamClientDll64 registry path),
#   - the matching .so is discovered via WINEDLLPATH, beside the PE on DllPath.
#
# A bare LoadLibrary of an unregistered name never probes WINEDLLPATH, so the PE
# must exist as a real marked file where the loader already looks.
set -eu
cd "$(dirname "$0")"

BOTTLE_NAME="${1:-seamspike}"
CXROOT="$HOME/Applications/CrossOver.app/Contents/SharedSupport/CrossOver"
WL="$CXROOT/CrossOver-Hosted Application/wineloader"
BOTTLE="$HOME/Library/Application Support/CrossOver/Bottles/$BOTTLE_NAME"
DIST="$(pwd)/dist"

[ -d "$BOTTLE" ] || { echo "bottle '$BOTTLE_NAME' does not exist; create it with cxbottle --create"; exit 2; }

# Stage 2 drives the real steamclient.dylib, so Steam.app must be running — AND
# ONLINE, or Steam_BConnected reads false off cache and mimics a broken bridge
# (#2's offline-mode confound). We can only assert "running" from here; the exe
# fails loudly on b_connected != 1 to catch the offline case.
if ! pgrep -qx steam_osx && ! pgrep -q "Steam.AppBundle"; then
    echo "WARNING: native Steam.app does not appear to be running."
    echo "         Launch Steam and confirm it is ONLINE, then re-run."
fi

cp "$DIST/bridgetest.dll" "$BOTTLE/drive_c/windows/system32/bridgetest.dll"
cp "$DIST/spike.exe"      "$BOTTLE/drive_c/spike.exe"

export WINEPREFIX="$BOTTLE"
export CX_ROOT="$CXROOT"
# WINEDLLPATH entries are the dirs that hold builtin PEs; the .so is looked up as
# <entry>/<name>.so. Keep CrossOver's own dir first so ntdll et al still resolve.
export WINEDLLPATH="$CXROOT/lib/wine/x86_64-windows:$DIST"

cd "$BOTTLE/drive_c"
exec "$WL" c:\\spike.exe
