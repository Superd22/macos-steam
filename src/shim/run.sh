#!/bin/sh
# Deploy the shim into a clean bottle and run the #7 achievement harness through
# it, with NO Windows Steam — the Level B deliverable of #11.
#
# Production hook (#13): the game's steam_api64.dll loads our steamclient64.dll
# via HKCU\...\ActiveProcess\SteamClientDll64. The unix .so is discovered beside
# it on WINEDLLPATH (#8/#10). Provenance: delete the .dll and the run must flip
# back to SteamAPI_Init()=0 FATAL (the negative control from #13).
#
# Usage: ./run.sh [mode] [arg]
#   mode = loop|status|set|reset|overlay   (default loop)
#   arg  = achievement name for set; for `overlay`, which slot to fire
#          (all|store|web|friends|user|invite|remoteplay|connect|protocol).
#
# ./run.sh overlay and SHIM_OVERLAY=0 ./run.sh overlay are the #23 pair: armed,
# IsOverlayEnabled answers true only if injection actually landed; disabled, it
# must answer false.
set -eu
cd "$(dirname "$0")"

# The deploy contract (#32). This is a test-time script, but it plants the same
# payload under the same names as the real launch — sharing the manifest is what
# keeps the acceptance run testing what ships.
../layout/build.sh
. ../layout/gen/shim_paths.sh
# And the switch predicates (#33): the negative control below asks the same
# question the shipped launch script asks, in the same words.
. ../layout/gen/shim_policy.sh

MODE="${1:-loop}"
ARG="${2:-}"
BOTTLE_NAME="shim-clean"
CXROOT="$(shim_installed_app "$SHIM_PATH_CX_APP_REL")/Contents/SharedSupport/CrossOver"
WL="$CXROOT/CrossOver-Hosted Application/wineloader"
BOTTLE="$HOME/$SHIM_PATH_CX_BOTTLES_REL/$BOTTLE_NAME"
DIST="$(pwd)/$SHIM_PATH_DIST"
HARNESS="$(cd ../../instruments/harness/build && pwd)"
SHIMDIR="$BOTTLE/$SHIM_PATH_SHIM_SUBDIR"

[ -d "$BOTTLE" ] || { echo "bottle '$BOTTLE_NAME' missing"; exit 2; }
[ -f "$HARNESS/harness.exe" ] || { echo "harness.exe missing — run 'make' in instruments/harness"; exit 2; }

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
# Which app the harness initialises AS. 480 (Spacewar) is the default because
# the achievement modes need an app whose achievements are safe to burn — but a
# per-title bug is not reproducible against a different title, so this is a knob:
#   APPID=1466860 ./run.sh ticket
# runs the harness as AoE IV, which is how #90's ticket path was measured.
APPID="${APPID:-480}"
export SteamAppId="$APPID"     # gives the dylib's ISteamUserStats its app context
export SteamGameId="$APPID"
# Overlay (#21). ON by default, matching what ships; `SHIM_OVERLAY=0 ./run.sh` is
# the negative control. The unixlib's constructor dlopens Valve's renderer and
# STEAM_OVERLAY_LOGGING makes it say whether that landed before NSApplication
# (the gate measured in attic/overlay-probe/).
#
# The harness is a console exe with no swapchain, so the renderer LOADS here and
# never ARMS — IsOverlayEnabled() stays 0. That is the correct answer, and the
# discriminator #23 turns on: a real title returns 1 from the same code.
if shim_overlay_enabled; then
    shim_overlay_export 1
    unset SteamNoOverlayUIDrawing
    export SteamOverlayGameId="$SteamAppId"
    export STEAM_OVERLAY_LOGGING=1 STEAM_OVERLAY_LOGGING_FLUSH=1
else
    # Explicit 0, not merely unset: unset means ON to the unixlib now.
    shim_overlay_export 0
    export SteamNoOverlayUIDrawing=1
    export SteamOverlayGameId=0
fi
: > "$SHIM_UNIX_LOG"

# Plant the shim (both halves co-located; the .so is found here via WINEDLLPATH).
mkdir -p "$SHIMDIR"
cp "$DIST/$SHIM_PATH_PE64"   "$SHIMDIR/$SHIM_PATH_PE64"
cp "$DIST/$SHIM_PATH_UNIX64" "$SHIMDIR/$SHIM_PATH_UNIX64"

# The game exe + Valve's steam_api64.dll + steam_appid.txt.
cp "$HARNESS/harness.exe"      "$BOTTLE/drive_c/harness.exe"
cp "$HARNESS/steam_api64.dll"  "$BOTTLE/drive_c/steam_api64.dll"
printf '%s' "$APPID" > "$BOTTLE/drive_c/steam_appid.txt"

# The single load-bearing registry value (#13): point steam_api64.dll at our PE.
"$WL" reg add "HKCU\\Software\\Valve\\Steam\\ActiveProcess" \
    /v SteamClientDll64 /t REG_SZ /d "$SHIM_PATH_PE64_WIN" /f >/dev/null 2>&1

echo "=== running harness ($MODE${ARG:+ $ARG}) as appid $APPID through the shim, no Windows Steam ==="
cd "$BOTTLE/drive_c"
"$WL" c:\\harness.exe "$MODE" ${ARG:+"$ARG"} 2>/tmp/shim_wine_stderr.log || true
echo "=== unix-side shim log (/tmp/shim_unix.log) ==="
cat "$SHIM_UNIX_LOG" || true
echo "=== PE-side shim debugstr (filtered) ==="
grep -a "shim:" /tmp/shim_wine_stderr.log || echo "(no shim: debugstr lines captured)"
