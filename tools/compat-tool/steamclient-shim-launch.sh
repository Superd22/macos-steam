#!/bin/sh
# The compat-tool launch script — the Half A -> Half B seam (#12).
#
# Steam (patched so m_bCompatEnabled is on, #16) hands a Windows-only title's
# .exe to this script under Valve's compat-tool contract:
#
#     steamclient-shim-launch.sh <verb> <path-to-exe> [args...]
#
# where <verb> is "waitforexitandrun" (the real launch) or "run" (a helper the
# launch spins off), and the STEAM_COMPAT_* environment carries the app id and
# the library/data paths (see steam-compat-tool-interface.md). Our job is to
# reconstruct the proven Half B shim environment (#11/#13) around that .exe and
# run it through CrossOver, so the game's steam_api64.dll loads our
# steamclient64.dll and answers Steamworks against the native macOS client —
# with zero Windows Steam.
#
# The load-bearing wiring, all from #11/#13:
#   - SteamClientDll64 registry value  -> the ONLY hook that makes steam_api64
#     load our PE (#13; the beside-the-EXE path never fires).
#   - WINEDLLPATH contains the shim dir -> so ntdll finds steamclient64.so
#     beside the builtin-marked steamclient64.dll (#8/#10).
#   - SteamAppId / SteamGameId          -> gives the dylib's ISteamUserStats its
#     app context; taken from Valve's STEAM_COMPAT_APP_ID (#11).
#
# Env overrides (default to production values; the #12 proof harness sets them):
#   SHIM_BOTTLE      CrossOver bottle to launch in         (default: steam-shim)
#   SHIM_DIST        dir holding steamclient64.dll/.so      (default: ../shim/dist beside this script)
#   CX_APP           CrossOver.app path                     (default: ~/Applications/CrossOver.app)
set -eu

log() { printf '[shim-launch] %s\n' "$*" >&2; }

# --- parse Valve's contract: <verb> <exe> [args...] ---------------------------
VERB="${1:-waitforexitandrun}"
[ $# -ge 1 ] && shift || true
EXE="${1:-}"
[ $# -ge 1 ] && shift || true
[ -n "$EXE" ] || { log "no executable given (verb=$VERB)"; exit 2; }
log "verb=$VERB exe=$EXE args=$*"

# --- locate CrossOver + the shim ---------------------------------------------
HERE="$(cd "$(dirname "$0")" && pwd)"
CX_APP="${CX_APP:-$HOME/Applications/CrossOver.app}"
CXROOT="$CX_APP/Contents/SharedSupport/CrossOver"
WL="$CXROOT/CrossOver-Hosted Application/wineloader"
[ -x "$WL" ] || { log "wineloader not found at $WL"; exit 2; }

# Shim location: env override, else the deployed copy beside this script
# (payload layout, ADR 0002), else the repo dev tree.
if [ -z "${SHIM_DIST:-}" ]; then
    for cand in "$HERE/dist" "$HERE/../shim/dist"; do
        [ -f "$cand/steamclient64.dll" ] && { SHIM_DIST="$cand"; break; }
    done
fi
[ -n "${SHIM_DIST:-}" ] && [ -f "$SHIM_DIST/steamclient64.dll" ] && [ -f "$SHIM_DIST/steamclient64.so" ] \
    || { log "shim not found (looked in \$SHIM_DIST, $HERE/dist, $HERE/../shim/dist)"; exit 2; }

BOTTLE_NAME="${SHIM_BOTTLE:-steam-shim}"
BOTTLE="$HOME/Library/Application Support/CrossOver/Bottles/$BOTTLE_NAME"
[ -d "$BOTTLE" ] || { log "bottle '$BOTTLE_NAME' missing"; exit 2; }
SHIMDIR="$BOTTLE/drive_c/shim"

# --- app id from Valve's contract (falls back to whatever Steam exported) -----
APPID="${STEAM_COMPAT_APP_ID:-${SteamAppId:-0}}"
log "appid=$APPID bottle=$BOTTLE_NAME"

# --- native macOS Steam must be running and online (map trap #1) --------------
if ! pgrep -qx steam_osx && ! pgrep -q "Steam.AppBundle"; then
    log "WARNING: native macOS Steam.app does not appear to be running — the shim will fail to connect."
fi

# --- plant Half B (idempotent) ------------------------------------------------
mkdir -p "$SHIMDIR"
cp -f "$SHIM_DIST/steamclient64.dll" "$SHIMDIR/steamclient64.dll"
cp -f "$SHIM_DIST/steamclient64.so"  "$SHIMDIR/steamclient64.so"

export WINEPREFIX="$BOTTLE"
export CX_ROOT="$CXROOT"
export WINEDLLPATH="$CXROOT/lib/wine/x86_64-windows:$SHIMDIR"
export SteamAppId="$APPID"
export SteamGameId="$APPID"
export SteamNoOverlayUIDrawing=1
export SteamOverlayGameId=0

# The single load-bearing registry value (#13): point steam_api64.dll at our PE.
"$WL" reg add "HKCU\\Software\\Valve\\Steam\\ActiveProcess" \
    /v SteamClientDll64 /t REG_SZ /d "C:\\shim\\steamclient64.dll" /f >/dev/null 2>&1

# --- run the title's .exe through CrossOver -----------------------------------
# wineloader accepts a unix path to the .exe. For "run" (a short-lived helper
# Steam spins off) we do the same wiring; only "waitforexitandrun" is the title.
log "launching: wine $EXE $*"
exec "$WL" "$EXE" "$@"
