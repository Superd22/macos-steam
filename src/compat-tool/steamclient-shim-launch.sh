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

# --- the deploy contract (#32) ------------------------------------------------
# Every payload path and basename below is a name from src/layout/layout.json,
# not a literal typed here. The fragment sits beside us in the deployed payload
# (install.sh copies it into the tool dir) and two levels up in the dev tree.
# It is sourced before anything else, including the log setup, because the log
# path is part of the contract too.
HERE="$(cd "$(dirname "$0")" && pwd)"
for _f in shim_paths.sh shim_policy.sh; do
    for _p in "$HERE/$_f" "$HERE/../layout/gen/$_f"; do
        [ -f "$_p" ] && { . "$_p"; break; }
    done
done
unset _f _p
# shim_policy.sh (#33) carries the switch predicates — the overlay question below
# is asked by a generated function, not by a test written here. Both fragments
# are required: a payload with one and not the other is a partial install.
if [ -z "${SHIM_PATH_PE64:-}" ] || ! command -v shim_overlay_enabled >/dev/null 2>&1; then
    printf 'shim-launch: shim_paths.sh / shim_policy.sh not found beside %s or in ../layout/gen — reinstall\n' "$HERE" >&2
    exit 2
fi

# Steam invokes us with stderr detached, so a stderr-only log is invisible when
# a launch fails from the Play button. Tee to a file as well (#12).
#
# Not /tmp: it is world-writable, so the fixed path was one another local account
# could pre-plant a symlink at, making us append to a file of its choosing as the
# user — and the log names the whole Steam library, app ids included, while
# landing world-readable. ~/Library/Logs is writable only by its owner, and the
# umask keeps what we create there to 0600. The same directory is where the
# injector and the shim's unix half now write.
#
# The umask is set and restored around our own creation only. Leaving it at 077
# would be inherited by the title we exec, quietly changing the mode of every
# save file and config it writes from then on — a hardening change has no
# business doing that.
if [ -z "${SHIM_LAUNCH_LOG:-}" ]; then
    SHIM_LAUNCH_LOG="$HOME/$SHIM_PATH_LOG_LAUNCH_REL"
    _oldumask="$(umask)"
    umask 077
    mkdir -p "$(dirname "$SHIM_LAUNCH_LOG")" 2>/dev/null || true
    : >>"$SHIM_LAUNCH_LOG" 2>/dev/null || true
    umask "$_oldumask"
    unset _oldumask
fi
log() {
    printf '[shim-launch %s] %s\n' "$(date +%H:%M:%S)" "$*" >&2
    printf '[shim-launch %s] %s\n' "$(date +%H:%M:%S)" "$*" >>"$SHIM_LAUNCH_LOG" 2>/dev/null || true
}

# --- parse Valve's contract: [verb] <exe> [args...] ---------------------------
# The verb reaches us only because toolmanifest.vdf's commandline ends in
# "%verb%". It is NOT guaranteed: a manifest that omits %verb% (ours did, until
# the #12 Play-button run exposed it) makes Steam pass the .exe as argv[1]. A
# script that blindly shifts then dies with "no executable given" — two seconds
# after Play, with nothing in any log but a vanished process. So sniff argv[1]
# rather than assuming it: consume it only when it really is a verb.
VERB=waitforexitandrun
case "${1:-}" in
    run|waitforexitandrun)
        VERB="$1"; shift ;;
    *)
        log "argv[1] is not a verb (manifest missing %verb%?) — assuming $VERB" ;;
esac
EXE="${1:-}"
[ $# -ge 1 ] && shift || true
[ -n "$EXE" ] || { log "no executable given (verb=$VERB)"; exit 2; }
log "verb=$VERB exe=$EXE args=$*"

# --- locate CrossOver + the shim ---------------------------------------------
CX_APP="${CX_APP:-$HOME/$SHIM_PATH_CX_APP_REL}"
CXROOT="$CX_APP/Contents/SharedSupport/CrossOver"
# Overlay (#24) needs an entitled wineloader, and CX_ROOT is derived by bin/wine
# from its OWN path (bin/wine:69) — env CX_ROOT is overwritten, not read. So the
# only way to steer it is to invoke a wine from a mirror root. SHIM_CXROOT points
# at one: bin/wineloader and lib/wine/<arch>-unix/wine are re-signed copies, the
# rest symlinks into CrossOver.app. Unset = stock CrossOver, unchanged behaviour.
CXROOT="${SHIM_CXROOT:-$CXROOT}"
CXWINE="$CXROOT/bin/wine"
[ -x "$CXWINE" ] || { log "CrossOver front door not found at $CXWINE"; exit 2; }

# Shim location: env override, else the deployed copy beside this script
# (payload layout, ADR 0002), else the repo dev tree.
if [ -z "${SHIM_DIST:-}" ]; then
    for cand in "$HERE/$SHIM_PATH_DIST" "$HERE/../shim/$SHIM_PATH_DIST"; do
        [ -f "$cand/$SHIM_PATH_PE64" ] && { SHIM_DIST="$cand"; break; }
    done
fi
[ -n "${SHIM_DIST:-}" ] && [ -f "$SHIM_DIST/$SHIM_PATH_PE64" ] && [ -f "$SHIM_DIST/$SHIM_PATH_UNIX64" ] \
    || { log "shim not found (looked in \$SHIM_DIST, $HERE/$SHIM_PATH_DIST, $HERE/../shim/$SHIM_PATH_DIST)"; exit 2; }

# The 32-bit half is optional so an old payload still launches 64-bit titles,
# but a 32-bit title without it fails as "Could not sign in to your Steam
# account" — steam_api.dll finds no steamclient.dll and SteamAPI_Init returns 0,
# which the game reports as a login problem (#20). Say so plainly here.
HAVE32=0
[ -f "$SHIM_DIST/$SHIM_PATH_PE32" ] && [ -f "$SHIM_DIST/$SHIM_PATH_UNIX32" ] && HAVE32=1

BOTTLE_NAME="${SHIM_BOTTLE:-$SHIM_PATH_BOTTLE_DEFAULT}"
BOTTLE="$HOME/$SHIM_PATH_CX_BOTTLES_REL/$BOTTLE_NAME"
[ -d "$BOTTLE" ] || { log "bottle '$BOTTLE_NAME' missing"; exit 2; }
SHIMDIR="$BOTTLE/$SHIM_PATH_SHIM_SUBDIR"

# --- app id from Valve's contract (falls back to whatever Steam exported) -----
APPID="${STEAM_COMPAT_APP_ID:-${SteamAppId:-0}}"
log "appid=$APPID bottle=$BOTTLE_NAME"

# --- native macOS Steam must be running and online (map trap #1) --------------
# Absolute path: Steam hands the tool a scrubbed PATH, so a bare `pgrep` is not
# resolvable and the check used to warn on every launch even with Steam running.
if ! /usr/bin/pgrep -qx steam_osx && ! /usr/bin/pgrep -q "Steam.AppBundle"; then
    log "WARNING: native macOS Steam.app does not appear to be running — the shim will fail to connect."
fi

# --- plant Half B (idempotent) ------------------------------------------------
mkdir -p "$SHIMDIR"
cp -f "$SHIM_DIST/$SHIM_PATH_PE64"   "$SHIMDIR/$SHIM_PATH_PE64"
cp -f "$SHIM_DIST/$SHIM_PATH_UNIX64" "$SHIMDIR/$SHIM_PATH_UNIX64"
if [ "$HAVE32" = 1 ]; then
    # 32-bit titles (Among Us and most of the older Unity catalogue) load
    # steam_api.dll, which looks for steamclient.dll and the SteamClientDll
    # registry value — different file, different value, same shim (#20). The
    # .so is the same x86_64 unix half under the 32-bit PE's basename, because
    # ntdll derives the .so name from the builtin PE it just loaded.
    cp -f "$SHIM_DIST/$SHIM_PATH_PE32"   "$SHIMDIR/$SHIM_PATH_PE32"
    cp -f "$SHIM_DIST/$SHIM_PATH_UNIX32" "$SHIMDIR/$SHIM_PATH_UNIX32"
else
    log "WARNING: no 32-bit shim in $SHIM_DIST — a 32-bit title will fail with"
    log "         \"Could not sign in to your Steam account\". Run src/shim/build.sh."
fi

# Shim dir on WINEDLLPATH through the front door: cxbottle.conf [Wine] DllPath.
# The perl launcher rebuilds WINEDLLPATH from this key (#19); default win64
# path first, shim dir after, trailing lib/wine as the launcher's own default.
# i386-windows is on the path too: a 32-bit title resolves its builtins from
# there, not from x86_64-windows, and overriding DllPath replaces CrossOver's
# own default rather than adding to it (#20).
CONF="$BOTTLE/cxbottle.conf"
# ${CX_ROOT}/${WINEPREFIX} stay literal — the perl launcher expands them, we do
# not — while the shim subdir comes from the contract.
DLLPATH='${CX_ROOT}/lib/wine/x86_64-windows:${CX_ROOT}/lib/wine/i386-windows:${WINEPREFIX}/'"$SHIM_PATH_SHIM_SUBDIR"':${CX_ROOT}/lib/wine'
if ! grep -q '^"DllPath"' "$CONF" 2>/dev/null; then
    printf '\n[Wine]\n"DllPath" = "%s"\n' "$DLLPATH" >> "$CONF"
    log "planted [Wine] DllPath in cxbottle.conf"
else
    # A bottle provisioned before #20 has a DllPath without i386-windows. Leave
    # the operator's line alone, but do not let a 32-bit launch fail silently.
    grep -q "$SHIM_PATH_SHIM_SUBDIR" "$CONF" || log "WARNING: cxbottle.conf [Wine] DllPath does not include $SHIM_PATH_SHIM_SUBDIR — the shim will not be found"
    if [ "$HAVE32" = 1 ] && ! grep -q 'i386-windows' "$CONF"; then
        log "NOTE: cxbottle.conf [Wine] DllPath has no i386-windows entry; if a 32-bit"
        log "      title fails to start, set it to: $DLLPATH"
    fi
fi

# Overlay injector (#25, ADR 0003). Only the 64-bit one is ever invoked: it reads
# the title's PE header and hands over to its 32-bit sibling when they disagree,
# because CreateRemoteThread cannot cross a bitness boundary.
HAVE_INJECT=0
if [ -f "$SHIM_DIST/$SHIM_PATH_INJECT64" ] && [ -f "$SHIM_DIST/$SHIM_PATH_INJECT32" ]; then
    cp -f "$SHIM_DIST/$SHIM_PATH_INJECT64" "$SHIMDIR/$SHIM_PATH_INJECT64"
    cp -f "$SHIM_DIST/$SHIM_PATH_INJECT32" "$SHIMDIR/$SHIM_PATH_INJECT32"
    HAVE_INJECT=1
fi

export SteamAppId="$APPID"
export SteamGameId="$APPID"
# Overlay (#21). ON by default since #23 closed the API surface and the route was
# proven across titles. `SHIM_OVERLAY=0` opts out. The renderer checks BOTH of
# these and bails on SteamNoOverlayUIDrawing, so it must be unset, not just empty.
#
# The default flipped here, at the policy layer, but the INTERLOCK below did not:
# no injector means no compositor, and a title that believes an overlay exists
# can wait on one forever. So "on by default" is still conditional on being able
# to deliver, and the off branch says so out loud rather than defaulting quietly.
if shim_overlay_enabled && [ "$HAVE_INJECT" = 1 ]; then
    unset SteamNoOverlayUIDrawing
    export SteamOverlayGameId="$APPID"
    # Export, not just read: the unixlib's constructor asks the same predicate
    # to decide whether to dlopen the renderer, and it runs in the process we
    # are about to launch. Reading it here without exporting arms the env and
    # nothing else — which is why setting it goes through the fragment's
    # exporting helper rather than a bare assignment.
    shim_overlay_export 1
    # The renderer's own log is opt-in, and without it a failure is mute — which
    # is what made #22 misread (a2) as dead. It names the stage reached:
    # "Hooking ..." lines mean our unixlib's constructor beat NSApplication.
    export STEAM_OVERLAY_LOGGING=1 STEAM_OVERLAY_LOGGING_FLUSH=1
    log "overlay ENABLED: SteamOverlayGameId=$APPID, SteamNoOverlayUIDrawing unset"
    log "      renderer log: /tmp/gameoverlayrenderer.<pid>.log"
else
    # Off, and off HARD: a title told an overlay exists can wait on one forever,
    # so never arm the env without an injector to deliver the renderer.
    #
    # Exporting an explicit 0 is load-bearing now that the manifest's default is
    # ON. Leaving it merely unset used to mean "off" everywhere; since the flip
    # it means "on", so this branch would have dlopened the renderer into a
    # process with no injector to place it — the exact thing the interlock above
    # exists to prevent. Say 0, do not imply it.
    if shim_overlay_enabled; then
        log "overlay ON by default but no injector in $SHIM_DIST — staying off"
    fi
    shim_overlay_export 0
    export SteamNoOverlayUIDrawing=1
    export SteamOverlayGameId=0
fi
export CX_BOTTLE="$BOTTLE_NAME"

# The load-bearing registry values (#13): point steam_api at our PE. There are
# TWO, one per bitness, and a title reads only its own — steam_api64.dll reads
# SteamClientDll64, steam_api.dll reads SteamClientDll. Writing just the 64-bit
# one is exactly why Among Us could not sign in (#20). Both are written on every
# launch regardless of the title's bitness: it is one cheap reg add, and it
# means the bottle is correct for whatever gets launched in it next.
"$CXWINE" --bottle "$BOTTLE_NAME" reg add "HKCU\\Software\\Valve\\Steam\\ActiveProcess" \
    /v SteamClientDll64 /t REG_SZ /d "$SHIM_PATH_PE64_WIN" /f >/dev/null 2>&1
if [ "$HAVE32" = 1 ]; then
    "$CXWINE" --bottle "$BOTTLE_NAME" reg add "HKCU\\Software\\Valve\\Steam\\ActiveProcess" \
        /v SteamClientDll /t REG_SZ /d "$SHIM_PATH_PE32_WIN" /f >/dev/null 2>&1
fi

# --- run the title's .exe through CrossOver's front door ----------------------
# CWD must be the game dir (#19): the engine mounts pack files relative to it.
# For "run" (a short-lived helper Steam spins off) we do the same wiring; only
# "waitforexitandrun" is the title.
cd "$(dirname "$EXE")"
# The injector is a PE and calls CreateProcess, which cannot open a UNIX path —
# and Steam hands us one for its own helpers (iscriptevaluator.exe), which died
# with "CreateProcess failed 2" when the first version of this routed them too.
# The verb does not discriminate: our manifest omits %verb%, so those helper
# invocations are ALSO seen as waitforexitandrun. The path shape is what tells
# them apart, so convert it the way Wine does and let anything unconvertible go
# down the direct path.
EXE_WIN="$EXE"
case "$EXE" in
    /*) EXE_WIN="Z:$(printf '%s' "$EXE" | tr '/' '\\')" ;;
esac
if shim_overlay_enabled && [ "$HAVE_INJECT" = 1 ] && [ -f "$EXE" ]; then
    # The injector creates the title suspended, puts the shim PE in before any of
    # the title's own code runs, and resumes — that ordering is the whole overlay
    # (ADR 0003). It stays for the title's lifetime and exits with the title's
    # own code, which is what Steam reads back through this tool.
    log "launching (front door, injected): wine --bottle $BOTTLE_NAME $SHIM_PATH_INJECT64 $EXE $*"
    exec "$CXWINE" --bottle "$BOTTLE_NAME" "$SHIMDIR/$SHIM_PATH_INJECT64" "$EXE_WIN" "$@"
fi
log "launching (front door, cwd=$PWD): wine --bottle $BOTTLE_NAME $EXE $*"
exec "$CXWINE" --bottle "$BOTTLE_NAME" "$EXE" "$@"
