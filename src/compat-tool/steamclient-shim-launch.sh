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
CX_APP="${CX_APP:-$(shim_installed_app "$SHIM_PATH_CX_APP_REL")}"
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

# --- the DRM route (ADR 0014) -------------------------------------------------
# A DRM-wrapped title does not ask Steamworks anything. Its wrapper resolves
# CreateInterface, traces it back to the file that provided it, and verifies an
# RSA signature over that file against keys built into the title. Our shim
# cannot carry one: a PE is either a Wine builtin or Valve-signed, because
# winebuild's marker and Valve's block are the same bytes at offset 0x40.
#
# So on this route the file at that path is VALVE'S OWN, genuine and untouched,
# fetched from Valve's public client manifest by src/drm/fetch.sh -- and our
# shim is reached through trampolines that our tier0 shadow writes into it
# while it loads. Nothing is forged; the wrapper's question is "is the client
# DLL on disk the real one", and the answer is yes.
#
# Taken ONLY by titles that are actually wrapped. A non-DRM title's launch is
# byte-identical to what it was before this existed, which is the point: the
# normal path is proven across the catalogue and this one is not.
# In the payload every artifact shares one dir; in the dev tree the shadows are
# their own module's, so look there too rather than making the dev build stage a
# payload just to exercise this path.
DRM_DIST="$SHIM_DIST"
[ -f "$DRM_DIST/$SHIM_PATH_SHADOW_TIER0" ] || DRM_DIST="$HERE/../drm/$SHIM_PATH_DIST"

# Which piece is missing decides what the user should do about it, so find out
# rather than assuming the common one.
DRM_MISSING=
for _f in "$SHIM_DIST/$SHIM_PATH_LSTEAM_PE64" "$SHIM_DIST/$SHIM_PATH_LSTEAM_UNIX64" \
          "$DRM_DIST/$SHIM_PATH_SHADOW_TIER0" "$DRM_DIST/$SHIM_PATH_SHADOW_VSTDLIB"; do
    [ -f "$_f" ] || DRM_MISSING="$DRM_MISSING $(basename "$_f")"
done
DRM_UNFETCHED=0
[ -f "$HOME/$SHIM_PATH_CLIENT_DLL_REL" ] || DRM_UNFETCHED=1

HAVE_DRM=0
[ -z "$DRM_MISSING" ] && [ "$DRM_UNFETCHED" = 0 ] && HAVE_DRM=1

# --- is this TITLE wrapped? (#104) --------------------------------------------
# Steam's DRM wrapper lives in a `.bind` section, and it is in the file on disk
# rather than unpacked at runtime -- so the section table answers exactly,
# without running anything (ADR 0014). Any failure to read a file means NO for
# that file: taking the normal path for a wrapped title produces one legible
# error dialog, while taking the DRM path for an unwrapped one changes a launch
# that works.
#
# What changed in #104 is WHICH FILE is asked. This used to ask about $EXE, the
# binary Steam hands the compat tool -- and for Space Marine II that is a 32-bit
# outer launcher with no `.bind` at all, while the wrapped binary sits four
# directories down. Measured there: two of the title's eight EXEs are wrapped,
# neither of them the one Steam names. So the route never armed, and neither
# did the warning that would have explained it, because both asked the same
# question about the same wrong file.
#
# "Is this file wrapped" is a fact about a file. "Does this title ship a wrapped
# binary" is a fact about the title -- and the route is armed per title.

# Two questions that used to be one function. The second is "can our route help
# it": the shadows, the second shim name and the trampolines are all 64-bit, so
# a 32-bit wrapped title would take a route that cannot help it and lose the
# working 32-bit one. Merged into the wrapped test, that gate made a 32-bit
# launcher fronting a wrapped 64-bit title indistinguishable from an ordinary
# unwrapped 32-bit title -- the test returned at the bitness check before it
# ever read a section table, and "wrapped, but we cannot help it" was not
# sayable. The gate now lives at the call site, where it can be said out loud.
PE_MACHINE_AMD64=34404          # IMAGE_FILE_MACHINE_AMD64 (0x8664), decimal

# One pass over the headers answers both, because every od and dd is a process
# spawn and the sweep below asks this of every EXE the title ships. Sets
# EXE_MACHINE (COFF machine word) and EXE_WRAPPED (1 = carries `.bind`);
# non-zero return means the file is not a PE we could read, which is a NO.
EXE_MACHINE=
EXE_WRAPPED=0
exe_read_pe() {
    _exe="$1" _lfanew= _nsec= _szopt= _start=
    EXE_MACHINE=
    EXE_WRAPPED=0
    [ -f "$_exe" ] || return 1
    _lfanew=$(od -An -tu4 -j 60 -N 4 "$_exe" 2>/dev/null | tr -d ' ') || return 1
    [ -n "$_lfanew" ] && [ "$_lfanew" -gt 0 ] 2>/dev/null || return 1
    EXE_MACHINE=$(od -An -tu2 -j $((_lfanew + 4)) -N 2 "$_exe" 2>/dev/null | tr -d ' ') || return 1
    [ -n "$EXE_MACHINE" ] || return 1
    _nsec=$(od -An -tu2 -j $((_lfanew + 6)) -N 2 "$_exe" 2>/dev/null | tr -d ' ')
    _szopt=$(od -An -tu2 -j $((_lfanew + 20)) -N 2 "$_exe" 2>/dev/null | tr -d ' ')
    [ -n "$_nsec" ] && [ -n "$_szopt" ] && [ "$_nsec" -gt 0 ] 2>/dev/null || return 1
    [ "$_nsec" -le 96 ] 2>/dev/null || return 1
    _start=$((_lfanew + 24 + _szopt))
    # LC_ALL=C is load-bearing, not hygiene: a section table is bytes, not text,
    # and under a UTF-8 locale macOS grep rejects it as invalid multibyte and
    # matches nothing. Without this the route silently never engages -- and the
    # warning below never fires either, because it asks the same question.
    if dd if="$_exe" bs=1 skip="$_start" count=$((_nsec * 40)) 2>/dev/null \
        | LC_ALL=C tr -d '\000' | LC_ALL=C grep -q '\.bind'; then
        EXE_WRAPPED=1
    fi
    return 0
}

# The launcher chain: a REFINEMENT, never the mechanism.
# A launcher that declares its target does it in a <basename>.ini beside itself
# with ApplicationPath=. That is a FORMAT rule rather than a per-title one:
# ADR 0003:206-208 recorded the identical shape on an unrelated title
# (SpaceMarineBootstrapper.exe -> SpaceMarine.exe, ApplicationPath=,
# WaitForExit=0) before this issue existed. Following it names the exact binary,
# which cuts false positives and lets the log say something specific.
#
# NOTHING may depend on it. When it does not resolve -- an unknown launcher, an
# ini the user has edited, a chain that goes through an anti-cheat stub instead
# -- the sweep below still gives the right answer, so an unfamiliar launcher
# costs precision, not function. Exercised both ways on Space Marine II: with
# its ini pointing at the wrapped binary the chain names it exactly, and with
# the ini pointing at the anti-cheat stub instead the sweep still arms.
#
# Hops are capped: an ini naming itself, or a pair naming each other, is a loop,
# and this runs before the user has seen anything at all.
DRM_CHAIN_HOPS=4
drm_chain_next() {
    _from="$1" _ini= _val=
    _ini="${_from%.*}.ini"
    [ -f "$_ini" ] || return 1
    # key=value, CRLF, no section header. Neither the case of the key nor the
    # spacing around the = is guaranteed by anything, so do not assume either.
    # LC_ALL=C for the same reason as above: an ini beside a game EXE is not
    # promised to be valid UTF-8, and a sed that rejects it matches nothing.
    _val=$(LC_ALL=C sed -n 's/^[Aa]pplication[Pp]ath[[:space:]]*=[[:space:]]*//p' "$_ini" 2>/dev/null \
            | head -n 1 | tr -d '\r' | LC_ALL=C sed 's/[[:space:]]*$//')
    [ -n "$_val" ] || return 1
    # Windows separators, and relative to the ini's own directory.
    _val=$(printf '%s' "$_val" | tr '\\' '/')
    case "$_val" in
        /*) : ;;
        *)  _val="$(dirname "$_from")/$_val" ;;
    esac
    [ -f "$_val" ] || return 1
    printf '%s' "$_val"
}

# The sweep, which is the arming signal.
# Launcher-agnostic (any matryoshka depth, any convention, including shapes
# nobody has seen) and packer-agnostic: it asks about Steam's own published
# format, the same fact ADR 0014 already stakes the route on.
#
# Capped on BOTH count and depth, because each EXE costs a few process spawns in
# POSIX shell and a title shipping hundreds of them would make this expensive.
# Measured on Space Marine II: 8 EXEs, ~320 bytes of section table each, 150 ms
# wall for find plus all eight -- and ~2.7 ms per EXE on a 300-file fixture, so
# the count cap is about 0.7 s of worst case, once, before a launch that is
# already starting a Wine. When a cap bites we SAY SO at the call site rather
# than silently answering "no" -- an unfinished sweep is uncertainty, and the
# branch that tells the user we cannot help must be reachable from there too.
DRM_SWEEP_MAX_FILES=256
DRM_SWEEP_MAX_DEPTH=8
DRM_SWEEP_CAPPED=0

# Sets DRM_WRAPPED_EXE / DRM_WRAPPED_MACHINE / DRM_WRAPPED_HOW when the title
# ships a wrapped binary, and DRM_SWEEP_CAPPED when it ran out of budget before
# it could answer. Globals rather than stdout: a command substitution is a
# subshell, and the "I could not finish" flag has to survive.
DRM_WRAPPED_EXE=
DRM_WRAPPED_MACHINE=
DRM_WRAPPED_HOW=
drm_locate_wrapped() {
    _root="$1" _cand= _prev= _hop=0 _n=0 _list= _f=
    DRM_WRAPPED_EXE=
    DRM_WRAPPED_MACHINE=
    DRM_WRAPPED_HOW=
    DRM_SWEEP_CAPPED=0

    # First the binary Steam named, then whatever it declares. Both are exact,
    # both are two files' worth of work, and between them they answer for every
    # title shape we understand -- the sweep is what covers the rest.
    _cand="$EXE"
    while [ -n "$_cand" ]; do
        if exe_read_pe "$_cand" && [ "$EXE_WRAPPED" = 1 ]; then
            DRM_WRAPPED_EXE="$_cand"
            DRM_WRAPPED_MACHINE="$EXE_MACHINE"
            if [ "$_hop" = 0 ]; then
                DRM_WRAPPED_HOW="the binary Steam handed us"
            else
                DRM_WRAPPED_HOW="named by $(basename "${_prev%.*}.ini") (ApplicationPath=)"
            fi
            return 0
        fi
        [ "$_hop" -lt "$DRM_CHAIN_HOPS" ] || break
        _prev="$_cand"
        _cand=$(drm_chain_next "$_cand") || _cand=
        _hop=$((_hop + 1))
    done

    # Then every EXE the title ships, scoped to its INSTALL dir -- not to
    # wherever $EXE happens to live, which for a launcher one directory down is
    # a subtree that does not contain the game.
    _list=$(find "$_root" -maxdepth "$DRM_SWEEP_MAX_DEPTH" -type f -iname '*.exe' 2>/dev/null) || true
    if [ -n "$_list" ]; then
        while IFS= read -r _f; do
            [ -n "$_f" ] || continue
            if [ "$_n" -ge "$DRM_SWEEP_MAX_FILES" ]; then
                DRM_SWEEP_CAPPED=1
                break
            fi
            _n=$((_n + 1))
            [ "$_f" = "$EXE" ] && continue
            if exe_read_pe "$_f" && [ "$EXE_WRAPPED" = 1 ]; then
                DRM_WRAPPED_EXE="$_f"
                DRM_WRAPPED_MACHINE="$EXE_MACHINE"
                DRM_WRAPPED_HOW="found by sweeping the title's EXEs ($_n read)"
                return 0
            fi
        done <<EOF
$_list
EOF
    fi

    # A "no" from a sweep that stopped at the depth cap is not a no. Directories
    # sitting exactly AT the cap mean there are files below it we never read, so
    # ask -- one extra find, only on the path where the answer was negative.
    if [ "$DRM_SWEEP_CAPPED" = 0 ] \
       && [ -n "$(find "$_root" -mindepth "$DRM_SWEEP_MAX_DEPTH" -maxdepth "$DRM_SWEEP_MAX_DEPTH" \
                       -type d 2>/dev/null | head -n 1)" ]; then
        DRM_SWEEP_CAPPED=1
    fi
    return 1
}

# What a user should do about a route that cannot run. Said from TWO places now
# (#104): the title we know is wrapped, and the title we could not finish
# checking. It used to be reachable only from the first, which meant a title we
# failed to recognise got no diagnostic at all -- the user saw Valve's dialog
# and a launch log that mentioned DRM nowhere. That was the worse half of #104.
drm_route_unavailable() {
    if [ "$DRM_UNFETCHED" = 1 ]; then
        # Beside us in the deployed payload; in the dev tree it is its module's.
        _fetch="$HERE/$SHIM_PATH_FETCH_SH"
        [ -f "$_fetch" ] || _fetch="$HERE/../drm/$SHIM_PATH_FETCH_SH"
        log "         Valve's signed client DLL has not been fetched."
        log "         Run: $_fetch    (downloads ~60 MB from Valve, once)"
    fi
    [ -n "$DRM_MISSING" ] && \
        log "         missing from the payload:$DRM_MISSING — reinstall"
    # Explicit, and load-bearing under `set -e`: this is called as the last
    # command of an if-branch, and set -e is NOT ignored there the way it is in
    # the condition. Falling out of the function on that failing `[` would take
    # the whole launch down at the point where we were only trying to explain
    # ourselves -- the diagnostic killing the launch it was diagnosing.
    return 0
}

# The scope for the sweep: the TITLE's install directory, which is Valve's to
# tell us -- STEAM_COMPAT_INSTALL_PATH, from the same contract that carries the
# app id -- rather than the directory $EXE happens to sit in. They are the same
# place for a title Steam launches from its root, and they are not for one it
# does not; assuming the second is a milder version of the bug this fixes.
TITLEDIR="${STEAM_COMPAT_INSTALL_PATH:-}"
if [ -z "$TITLEDIR" ] || [ ! -d "$TITLEDIR" ]; then
    TITLEDIR="$(dirname "$EXE")"
fi

USE_DRM=0
if shim_drm_enabled; then
    drm_locate_wrapped "$TITLEDIR" || true
fi

# Over-arming still has a cost, though a smaller one than when this was written.
# It USED to be the overlay: the route and the injector are mutually exclusive
# (ADR 0003, ADR 0014), so a false positive silently cost a working title its
# overlay. Since #112 a wrapped title gets the overlay by the late path instead,
# so what a false positive costs now is a title running through Valve's signed
# DLL that did not need to -- a heavier startup and one more moving part in the
# way of a launch that worked. State the finding and how it was reached, every
# time, so that cost stays visible in the log rather than inferred from a
# behaviour change nobody asked for.
if [ -n "$DRM_WRAPPED_EXE" ]; then
    log "DRM: this title is wrapped — $DRM_WRAPPED_EXE"
    log "     carries Steam's .bind section; $DRM_WRAPPED_HOW"
fi

if [ -n "$DRM_WRAPPED_EXE" ] && [ "$DRM_WRAPPED_MACHINE" != "$PE_MACHINE_AMD64" ]; then
    # Sayable at all only because the bitness gate moved out of the predicate.
    log "WARNING: this title is DRM-wrapped, but the wrapped binary is machine"
    log "         0x$(printf '%04x' "$DRM_WRAPPED_MACHINE") and the DRM route is 64-bit only (ADR 0014), so it"
    log "         will fail with 'Application load error 3:0000065432'."
elif [ -n "$DRM_WRAPPED_EXE" ] && [ "$HAVE_DRM" = 1 ]; then
    USE_DRM=1
elif [ -n "$DRM_WRAPPED_EXE" ]; then
    log "WARNING: this title is DRM-wrapped and the DRM route is unavailable, so it"
    log "         will fail with 'Application load error 3:0000065432'."
    drm_route_unavailable
elif [ "$DRM_SWEEP_CAPPED" = 1 ]; then
    log "NOTE: could not finish checking whether this title is DRM-wrapped — the"
    log "      sweep of $TITLEDIR stopped at its cap"
    log "      ($DRM_SWEEP_MAX_FILES EXEs, $DRM_SWEEP_MAX_DEPTH directories deep). Treat 'not wrapped' as unknown here."
    if [ "$HAVE_DRM" = 1 ]; then
        log "      If it fails with 'Application load error 3:0000065432' it IS wrapped and"
        log "      the sweep missed it — that is worth reporting (#104)."
    else
        log "      If it fails with 'Application load error 3:0000065432' it IS wrapped, and"
        log "      the DRM route is unavailable:"
        drm_route_unavailable
    fi
fi

if [ "$USE_DRM" = 1 ]; then
    VSTEAMDIR="$BOTTLE/$SHIM_PATH_VSTEAM_SUBDIR"
    mkdir -p "$VSTEAMDIR"
    # Valve's bytes, unmodified -- this is the file the wrapper reads and checks
    # the signature of. Under a name of OURS: Wine resolves a builtin by basename
    # off the DLL path and our shim sits on that path, so under its own basename
    # ours would load in its place, silently, and the wrapper would reject the
    # unsigned file it found. The wrapper never looks at the name -- it resolves
    # whatever provided CreateInterface and reads that file.
    _src="$HOME/$SHIM_PATH_CLIENT_DLL_REL"
    _dst="$VSTEAMDIR/$SHIM_PATH_VALVE_PE64"
    # Not cp onto the destination: it truncates and rewrites in place, and a
    # concurrently-running wrapped title has that inode mapped. Copy beside it
    # and rename, which is atomic and leaves the old inode alone -- and skip
    # even that when the bytes are already there (the cache is SHA-verified
    # against Valve's manifest, so equal size is equal file).
    if [ ! -f "$_dst" ] || [ "$(wc -c <"$_src")" != "$(wc -c <"$_dst")" ]; then
        cp -f "$_src" "$_dst.new.$$" && mv -f "$_dst.new.$$" "$_dst"
    fi
    # Ours. They must sit beside it: the wrapper loads it with
    # LOAD_WITH_ALTERED_SEARCH_PATH, so its own directory is searched first and
    # that is the whole reason our code gets to run before its entry point.
    cp -f "$DRM_DIST/$SHIM_PATH_SHADOW_TIER0"   "$VSTEAMDIR/$SHIM_PATH_SHADOW_TIER0"
    cp -f "$DRM_DIST/$SHIM_PATH_SHADOW_VSTDLIB" "$VSTEAMDIR/$SHIM_PATH_SHADOW_VSTDLIB"
    # The shim under its second name, where the trampolines send the calls.
    cp -f "$SHIM_DIST/$SHIM_PATH_LSTEAM_PE64"   "$SHIMDIR/$SHIM_PATH_LSTEAM_PE64"
    cp -f "$SHIM_DIST/$SHIM_PATH_LSTEAM_UNIX64" "$SHIMDIR/$SHIM_PATH_LSTEAM_UNIX64"

    log "DRM route: $SHIM_PATH_VALVE_PE64_WIN (Valve's signed DLL), trampolined into $SHIM_PATH_LSTEAM_PE64"
fi

export SteamAppId="$APPID"
export SteamGameId="$APPID"
# Overlay (#21). ON by default since #23 closed the API surface and the route was
# proven across titles. `SHIM_OVERLAY=0` opts out. The renderer checks BOTH of
# these and bails on SteamNoOverlayUIDrawing, so it must be unset, not just empty.
#
# Since #92 the predicate has a second input: Steam's own "Enable the Steam
# Overlay while in-game", which the client hands us per launch (ADR 0012). That
# makes "don't inject into THIS title" a per-game decision the user expresses in
# Steam's UI — the only way to say it before #92 was to turn the overlay off for
# the whole library. Anti-tamper titles are the motivating case: AoE IV's Aegis
# rejects the injector's import rewrite and the title cannot start at all.
#
# The default flipped here, at the policy layer, but the INTERLOCK below did not:
# no way to DELIVER a renderer means no compositor, and a title that believes an
# overlay exists can wait on one forever. So "on by default" is still conditional
# on being able to deliver, and the off branch says so out loud rather than
# defaulting quietly.
#
# What changed with #112 is that there are now TWO ways to deliver, so the
# interlock asks about delivery rather than about the injector:
#
#   - not wrapped: the injector, which makes our PE the title's first static
#     import so the renderer is in before NSApplication (ADR 0003);
#   - wrapped: no injector -- the two are still exclusive and #107 measured both
#     ways it fails -- but the DRM route already has our unixlib in the process,
#     and it arms Valve's Metal-hook installer by hand once NSApplication is up
#     (overlay_arm_late in shim_unix.cpp, drm-overlay-late-arming.md §2).
#
# So a wrapped title no longer loses its overlay, and USE_DRM stops being a
# reason to say no here. It is still a reason to say no at the *injector* call
# site at the bottom of this file, which is where the exclusivity actually lives.
if shim_overlay_enabled && { [ "$USE_DRM" = 1 ] || [ "$HAVE_INJECT" = 1 ]; }; then
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
    # "Hooking ..." lines mean the hooks went in — because the constructor beat
    # NSApplication on the injector path, or because it armed the installer by
    # hand on the DRM one.
    export STEAM_OVERLAY_LOGGING=1 STEAM_OVERLAY_LOGGING_FLUSH=1
    log "overlay ENABLED: SteamOverlayGameId=$APPID, SteamNoOverlayUIDrawing unset"
    if [ "$USE_DRM" = 1 ]; then
        log "      delivered by the shim's own unixlib, which arms Valve's hook installer"
        log "      after NSApplication is up (#112) — this route does not inject"
    fi
    log "      renderer log: /tmp/gameoverlayrenderer.<pid>.log"
else
    # Off, and off HARD: a title told an overlay exists can wait on one forever,
    # so never arm the env without one of the two delivery paths above.
    #
    # Exporting an explicit 0 is load-bearing now that the manifest's default is
    # ON. Leaving it merely unset used to mean "off" everywhere; since the flip
    # it means "on", so this branch would have dlopened the renderer into a
    # process with nothing to place it — the exact thing the interlock above
    # exists to prevent. Say 0, do not imply it.
    # Which input said no. A user who just unticked Steam's box needs to see
    # that it took effect, and a support log that says only "overlay off" makes
    # the three reasons indistinguishable. The reason is ASKED of the fragment
    # (shim_overlay_vetoed), never re-derived here.
    if shim_overlay_vetoed; then
        log "overlay OFF for appid $APPID: Steam's own setting says so ($SHIM_VETO_OVERLAY=1)"
        log "      turn it back on in Steam: Properties -> General -> Enable the Steam Overlay while in-game"
    elif ! shim_overlay_enabled; then
        log "overlay OFF: $SHIM_ENV_OVERLAY=0"
    else
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
# On the DRM route this names VALVE's signed DLL rather than ours -- steam_api64
# and the DRM wrapper both load whatever it points at, and the wrapper checks
# the signature of the file it finds. The 32-bit value below is always ours:
# there is no 32-bit DRM route yet (ADR 0014).
#
# The value is bottle-global but the choice is now per-title, so a "run" helper
# (iscriptevaluator and friends, which Steam spins off through this same tool)
# must NOT rewrite it: doing so would point a wrapped title at our unsigned PE
# midway through its own startup and fail it, for a helper that does not use
# Steamworks at all. Only the title launch decides. A helper that DOES use it
# still works either way -- both DLLs lead to the shim.
if [ "$VERB" = run ]; then
    log "verb=run: leaving SteamClientDll64 as it is (a helper does not own that choice)"
else
    if [ "$USE_DRM" = 1 ]; then
        CLIENT_DLL64="$SHIM_PATH_VALVE_PE64_WIN"
    else
        CLIENT_DLL64="$SHIM_PATH_PE64_WIN"
    fi
    "$CXWINE" --bottle "$BOTTLE_NAME" reg add "HKCU\\Software\\Valve\\Steam\\ActiveProcess" \
        /v SteamClientDll64 /t REG_SZ /d "$CLIENT_DLL64" /f >/dev/null 2>&1
fi
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
if shim_overlay_enabled && [ "$HAVE_INJECT" = 1 ] && [ "$USE_DRM" = 0 ] && [ -f "$EXE" ]; then
    # The injector creates the title suspended, puts the shim PE in before any of
    # the title's own code runs, and resumes — that ordering is the whole overlay
    # (ADR 0003). It stays for the title's lifetime and exits with the title's
    # own code, which is what Steam reads back through this tool.
    log "launching (front door, injected): wine --bottle $BOTTLE_NAME $SHIM_PATH_INJECT64 $EXE $*"
    exec "$CXWINE" --bottle "$BOTTLE_NAME" "$SHIMDIR/$SHIM_PATH_INJECT64" "$EXE_WIN" "$@"
fi
log "launching (front door, cwd=$PWD): wine --bottle $BOTTLE_NAME $EXE $*"
exec "$CXWINE" --bottle "$BOTTLE_NAME" "$EXE" "$@"
