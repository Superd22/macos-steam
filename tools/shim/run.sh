#!/bin/sh
# Deploy the shim into a clean bottle and run the #7 achievement harness through
# it, with NO Windows Steam — the Level B deliverable of #11.
#
# Production hook (#13): the game's steam_api64.dll loads our steamclient64.dll
# via HKCU\...\ActiveProcess\SteamClientDll64. The unix .so is discovered beside
# it on WINEDLLPATH (#8/#10). Provenance: delete the .dll and the run must flip
# back to SteamAPI_Init()=0 FATAL (the negative control from #13).
#
# Usage: ./run.sh [mode]   (mode = loop|status|set|reset ; default loop)
set -eu
cd "$(dirname "$0")"

MODE="${1:-loop}"
BOTTLE_NAME="shim-clean"
CXROOT="$HOME/Applications/CrossOver.app/Contents/SharedSupport/CrossOver"
WL="$CXROOT/CrossOver-Hosted Application/wineloader"
BOTTLE="$HOME/Library/Application Support/CrossOver/Bottles/$BOTTLE_NAME"
DIST="$(pwd)/dist"
HARNESS="$(cd ../harness/build && pwd)"
SHIMDIR="$BOTTLE/drive_c/shim"

[ -d "$BOTTLE" ] || { echo "bottle '$BOTTLE_NAME' missing"; exit 2; }
[ -f "$HARNESS/harness.exe" ] || { echo "harness.exe missing — run 'make' in tools/harness"; exit 2; }

# Native Steam must be running AND online (map trap #1); the harness aborts on
# BLoggedOn=false, and the unix side logs Steam_BConnected for the record.
if ! pgrep -qx steam_osx && ! pgrep -q "Steam.AppBundle"; then
    echo "WARNING: native Steam.app does not appear to be running."
fi

export WINEPREFIX="$BOTTLE"
export CX_ROOT="$CXROOT"
export WINEDLLPATH="$CXROOT/lib/wine/x86_64-windows:$SHIMDIR"
export WINEDEBUG="${WINEDEBUG:-+debugstr}"
export SHIM_UNIX_LOG="/tmp/shim_unix.log"
export SteamAppId=480          # gives the dylib's ISteamUserStats its app context
export SteamGameId=480
# Overlay (#21). SHIM_OVERLAY=1 arms it: the unixlib's constructor dlopens
# Valve's renderer, and STEAM_OVERLAY_LOGGING makes it say whether that landed
# before NSApplication (the gate measured in tools/overlay-probe/).
if [ "${SHIM_OVERLAY:-0}" = 1 ]; then
    unset SteamNoOverlayUIDrawing
    export SteamOverlayGameId="$SteamAppId"
    export STEAM_OVERLAY_LOGGING=1 STEAM_OVERLAY_LOGGING_FLUSH=1
else
    export SteamNoOverlayUIDrawing=1
    export SteamOverlayGameId=0
fi
: > "$SHIM_UNIX_LOG"

# Plant the shim (both halves co-located; the .so is found here via WINEDLLPATH).
mkdir -p "$SHIMDIR"
cp "$DIST/steamclient64.dll" "$SHIMDIR/steamclient64.dll"
cp "$DIST/steamclient64.so"  "$SHIMDIR/steamclient64.so"

# The game exe + Valve's steam_api64.dll + steam_appid.txt.
cp "$HARNESS/harness.exe"      "$BOTTLE/drive_c/harness.exe"
cp "$HARNESS/steam_api64.dll"  "$BOTTLE/drive_c/steam_api64.dll"
cp "$HARNESS/steam_appid.txt"  "$BOTTLE/drive_c/steam_appid.txt"

# The single load-bearing registry value (#13): point steam_api64.dll at our PE.
"$WL" reg add "HKCU\\Software\\Valve\\Steam\\ActiveProcess" \
    /v SteamClientDll64 /t REG_SZ /d "C:\\shim\\steamclient64.dll" /f >/dev/null 2>&1

echo "=== running harness ($MODE) through the shim, no Windows Steam ==="
cd "$BOTTLE/drive_c"
"$WL" c:\\harness.exe "$MODE" 2>/tmp/shim_wine_stderr.log || true
echo "=== unix-side shim log (/tmp/shim_unix.log) ==="
cat "$SHIM_UNIX_LOG" || true
echo "=== PE-side shim debugstr (filtered) ==="
grep -a "shim:" /tmp/shim_wine_stderr.log || echo "(no shim: debugstr lines captured)"
