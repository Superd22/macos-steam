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
# Launch vehicle (#19): CrossOver's FRONT DOOR — `bin/wine --bottle` — not a
# raw wineloader exec. The perl launcher is what wires the per-title tweak
# database (CX_HOME -> compatdb.dat -> cxcompatdb.so), the D3DMetal/GPTK dylib
# preload (CX_APPLEGPTK_LIBD3DSHARED_PATH consumed by ntdll.so), GStreamer,
# and whatever CodeWeavers add next; replicating it means maintaining a copy
# of their launch logic against every update. Requiring CrossOver is fine.
#
# Two launch facts the front door does NOT cover, which this script owns:
#   1. CWD must be the game dir. Mars resolves pack files ("Packs/Lua.fpk")
#      relative to the working directory and DELIBERATELY crashes (null write
#      at a fixed address in Mars.exe) when a mount fails — the #18/#19
#      "graphics" crash was this, not D3D12.
#   2. WINEDLLPATH must include the shim dir so ntdll finds steamclient64.so
#      beside the builtin-marked PE (#8/#10). The perl launcher rebuilds
#      WINEDLLPATH, so the shim dir rides in via the bottle's cxbottle.conf
#      "[Wine] DllPath" — planted idempotently below.
#
# The load-bearing wiring, all from #11/#13:
#   - SteamClientDll64 registry value  -> the ONLY hook that makes steam_api64
#     load our PE (#13; the beside-the-EXE path never fires).
#   - cxbottle.conf [Wine] DllPath     -> shim dir on WINEDLLPATH through the
#     front door (#19).
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
CXWINE="$CXROOT/bin/wine"
[ -x "$CXWINE" ] || { log "CrossOver front door not found at $CXWINE"; exit 2; }

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

# Shim dir on WINEDLLPATH through the front door: cxbottle.conf [Wine] DllPath.
# The perl launcher rebuilds WINEDLLPATH from this key (#19); default win64
# path first, shim dir after, trailing lib/wine as the launcher's own default.
CONF="$BOTTLE/cxbottle.conf"
if ! grep -q '^"DllPath"' "$CONF" 2>/dev/null; then
    printf '\n[Wine]\n"DllPath" = "${CX_ROOT}/lib/wine/x86_64-windows:${WINEPREFIX}/drive_c/shim:${CX_ROOT}/lib/wine"\n' >> "$CONF"
    log "planted [Wine] DllPath in cxbottle.conf"
fi

export SteamAppId="$APPID"
export SteamGameId="$APPID"
export SteamNoOverlayUIDrawing=1
export SteamOverlayGameId=0
export CX_BOTTLE="$BOTTLE_NAME"

# The single load-bearing registry value (#13): point steam_api64.dll at our PE.
"$CXWINE" --bottle "$BOTTLE_NAME" reg add "HKCU\\Software\\Valve\\Steam\\ActiveProcess" \
    /v SteamClientDll64 /t REG_SZ /d "C:\\shim\\steamclient64.dll" /f >/dev/null 2>&1

# --- run the title's .exe through CrossOver's front door ----------------------
# CWD must be the game dir (#19): the engine mounts pack files relative to it.
# For "run" (a short-lived helper Steam spins off) we do the same wiring; only
# "waitforexitandrun" is the title.
cd "$(dirname "$EXE")"
log "launching (front door, cwd=$PWD): wine --bottle $BOTTLE_NAME $EXE $*"
exec "$CXWINE" --bottle "$BOTTLE_NAME" "$EXE" "$@"
