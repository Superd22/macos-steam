#!/bin/sh
# Deploy the macOS Steam Play shim — vehicle A of ADR 0002.
#
# Assembles the two shipped artifacts from the repo's built outputs:
#
#   1. The EXTERNAL PAYLOAD  ~/Library/Application Support/macos-steam-shim/
#        libcompat-enabler.dylib                    the m_bCompatEnabled injector (#16)
#        compatibilitytools.d/crossover-steam-shim/ the compat tool (#17) + shim dist (#11)
#      Outside every bundle, so a Steam client update cannot touch it.
#
#   2. The LAUNCHER  ~/Applications/Steam (macOS Play).app
#      An unhardened .app whose main executable is a shell script: it exports
#      DYLD_INSERT_LIBRARIES (the injector) and STEAM_EXTRA_COMPAT_TOOLS_PATHS
#      (the payload's tool dir), then execs Valve's inner steam_osx. Touches zero
#      Valve files. Deliberately no LSEnvironment key — see the note below.
#
# Why this script exists: the payload was hand-assembled once and then drifted —
# it still held the pre-#19 shim (old wineloader launch, no CWD fix) long after
# the repo had moved on, which is invisible until a launch mysteriously fails.
# Deployment must be one reproducible command that always ships what was built.
#
#   ./src/installer/install.sh            deploy (builds first if needed)
#   ./src/installer/install.sh --uninstall  remove payload + launcher
set -eu

REPO="$(cd "$(dirname "$0")/../.." && pwd)"

# --- the deploy contract (#32) ------------------------------------------------
# Where every artifact lands is one manifest, src/layout/layout.json, not a set
# of literals restated here and in the launch script and in the two C halves.
# Regenerate first: an installer that deploys against a stale contract is the
# drift this script exists to end.
"$REPO/src/layout/build.sh"
. "$REPO/src/layout/gen/shim_paths.sh"
# The other half of the same manifest (#33): the runtime switches, as shell
# predicates. `SHIM_OVERLAY=0 ./install.sh` is read by the generated function,
# not by a test written here — this script used to hold one of the five.
. "$REPO/src/layout/gen/shim_policy.sh"

PAYLOAD="$HOME/$SHIM_PATH_PAYLOAD_REL"
APP="$HOME/$SHIM_PATH_LAUNCHER_REL"
STEAM_OSX="$HOME/$SHIM_PATH_STEAM_OSX_REL"
TOOLDIR="$HOME/$SHIM_PATH_TOOL_DIR_REL"

log() { printf '[install] %s\n' "$*"; }

# Overlay (#21). ON by default: the flag is baked into the launcher so the compat
# tool sees it — Steam does not forward arbitrary env to a compat tool, so the
# launcher is where it has to live. `SHIM_OVERLAY=0 ./install.sh` opts out.
#
# The launcher always states the value rather than relying on the default. A
# reinstall is how a user turns the overlay off, and "off" has to mean an
# explicit 0 reaching the bottle: every layer below now treats *unset* as ON, so
# an absent variable would silently mean the opposite of what was asked for.
if shim_overlay_enabled; then
    OVERLAY_ENV=1
    log "overlay ON (default) — $SHIM_ENV_OVERLAY=0 ./install.sh to disable"
else
    OVERLAY_ENV=0
    log "$SHIM_ENV_OVERLAY=0 baked into the launcher (overlay disabled)"
fi

if [ "${1:-}" = "--uninstall" ]; then
    rm -rf "$APP" "$PAYLOAD"
    log "removed $APP"
    log "removed $PAYLOAD"
    log "Steam itself was never modified — nothing else to undo."
    exit 0
fi

[ -x "$STEAM_OSX" ] || { log "native Steam not found at $STEAM_OSX"; exit 2; }

# --- build whatever is missing or stale --------------------------------------
# "Missing" is not enough: a present-but-older artifact is exactly the drift this
# script exists to stop, and a present artifact that was never built HERE is
# worse — an opaque binary shipped into the user's bottle that no diff can be
# read for. Nothing under a dist/ dir is version-controlled (see .gitignore), so
# every shipped artifact is built on this machine, from the sources beside it.
#
# stale <output> <source>...  — true when the output is absent or any source is
# newer than it. `find -newer` rather than a timestamp compare: no GNU stat, and
# it is one syscall-cheap test per source.
stale() {
    out="$1"; shift
    [ -f "$out" ] || return 0
    for src in "$@"; do
        [ -e "$src" ] || continue
        [ -n "$(find "$src" -newer "$out" -print -quit 2>/dev/null)" ] && return 0
    done
    return 1
}

# The generated header is a source like any other: a manifest edit changes a
# path that is compiled in, and gen.py only rewrites it when it really changed,
# so its mtime is exactly "when the contract last moved" (#32).
PATHS_H="$REPO/src/layout/gen/shim_paths.h"
# Same for the switch predicates (#33), which only the shim's two halves compile
# in — the enabler and the injector never ask whether the overlay is on.
POLICY_H="$REPO/src/layout/gen/shim_policy.h"
if stale "$REPO/src/compat-enabler/$SHIM_PATH_ENABLER" \
         "$REPO/src/compat-enabler/enabler.c" \
         "$REPO/src/compat-enabler/build.sh" "$PATHS_H"; then
    log "building injector"; "$REPO/src/compat-enabler/build.sh"
fi
SHIM_DIST_DIR="$REPO/src/shim/$SHIM_PATH_DIST"
INJECT_DIST_DIR="$REPO/src/overlay-inject/$SHIM_PATH_DIST"
if [ ! -f "$SHIM_DIST_DIR/$SHIM_PATH_PE32" ] \
   || stale "$SHIM_DIST_DIR/$SHIM_PATH_PE64" \
            "$REPO/src/shim/shim_pe.c"      "$REPO/src/shim/shim_unix.cpp" \
            "$REPO/src/shim/shim_abi.h"     "$REPO/src/shim/shim_vtables.h" \
            "$REPO/src/shim/steam_ifaces.h" "$REPO/src/shim/build.sh" \
            "$PATHS_H" "$POLICY_H"; then
    log "building shim (both bitnesses)"; "$REPO/src/shim/build.sh"
fi
if [ ! -f "$INJECT_DIST_DIR/$SHIM_PATH_INJECT32" ] \
   || stale "$INJECT_DIST_DIR/$SHIM_PATH_INJECT64" \
            "$REPO/src/overlay-inject/overlayinject.c" \
            "$REPO/src/overlay-inject/build.sh" "$PATHS_H"; then
    log "building overlay injector"; "$REPO/src/overlay-inject/build.sh"
fi

# --- payload ------------------------------------------------------------------
mkdir -p "$TOOLDIR/$SHIM_PATH_DIST"
cp -f "$REPO/src/compat-enabler/$SHIM_PATH_ENABLER" "$PAYLOAD/"
cp -f "$REPO/src/compat-tool/$SHIM_PATH_LAUNCH_SH"  "$TOOLDIR/"
# The contract travels with the launch script: once deployed it is the only copy
# either can see, so the script sources it from beside itself (#32).
cp -f "$REPO/src/layout/gen/$SHIM_PATH_PATHS_SH"    "$TOOLDIR/"
cp -f "$REPO/src/layout/gen/$SHIM_PATH_POLICY_SH"   "$TOOLDIR/"
# The two vdfs are templates. Steam matches the tool key inside
# compatibilitytool.vdf against the directory name, and toolmanifest.vdf names
# the launch script — three facts that must agree, so all three come from the
# manifest and none is typed into a vdf.
render_vdf() {
    sed -e "s|@TOOL_NAME@|$SHIM_PATH_TOOL_NAME|g" \
        -e "s|@LAUNCH_SH@|$SHIM_PATH_LAUNCH_SH|g" "$1" > "$2"
}
render_vdf "$REPO/src/compat-tool/$SHIM_PATH_TOOL_MANIFEST.in" "$TOOLDIR/$SHIM_PATH_TOOL_MANIFEST"
render_vdf "$REPO/src/compat-tool/$SHIM_PATH_TOOL_VDF.in"      "$TOOLDIR/$SHIM_PATH_TOOL_VDF"
cp -f "$SHIM_DIST_DIR/$SHIM_PATH_PE64"    "$TOOLDIR/$SHIM_PATH_DIST/"
cp -f "$SHIM_DIST_DIR/$SHIM_PATH_UNIX64"  "$TOOLDIR/$SHIM_PATH_DIST/"
# Both bitnesses: 32-bit titles load steam_api.dll -> steamclient.dll under the
# SteamClientDll registry value, a different file and a different value from the
# 64-bit pair (#20). Shipping only the 64-bit half is what made Among Us report
# "Could not sign in to your Steam account".
cp -f "$SHIM_DIST_DIR/$SHIM_PATH_PE32"    "$TOOLDIR/$SHIM_PATH_DIST/"
cp -f "$SHIM_DIST_DIR/$SHIM_PATH_UNIX32"  "$TOOLDIR/$SHIM_PATH_DIST/"
# The overlay injector (#25, ADR 0003) — the launch script plants it in the
# bottle and routes through it when SHIM_OVERLAY=1. Without both bitnesses the
# script leaves the overlay off rather than arming an env it cannot deliver.
cp -f "$INJECT_DIST_DIR/$SHIM_PATH_INJECT64" "$TOOLDIR/$SHIM_PATH_DIST/"
cp -f "$INJECT_DIST_DIR/$SHIM_PATH_INJECT32" "$TOOLDIR/$SHIM_PATH_DIST/"
chmod +x "$TOOLDIR/$SHIM_PATH_LAUNCH_SH"
log "payload -> $PAYLOAD"

# --- launcher .app ------------------------------------------------------------
mkdir -p "$APP/Contents/MacOS"
cat > "$APP/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleExecutable</key><string>launcher</string>
  <key>CFBundleIdentifier</key><string>com.macos-steam-shim.launcher</string>
  <key>CFBundleName</key><string>Steam (macOS Play)</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleShortVersionString</key><string>0.1</string>
  <!-- steam_osx is universal, and the arch LaunchServices picks is inherited
       across the exec below. Without this key a Finder/open/Raycast launch
       lands on translated x86_64, where the injector's arm64 gate pattern
       matches nothing and the log reads "patched 0 site(s)". A shell exec from
       an arm64 terminal happened to pick arm64, which is why the direct path
       always looked fine. State the preference so every launch path agrees. -->
  <key>LSArchitecturePriority</key>
  <array><string>arm64</string></array>
</dict>
</plist>
PLIST
# The body is a QUOTED heredoc: nothing in it is expanded at install time, so
# the launcher resolves its own paths from $HOME when it runs. The unquoted
# version interpolated $PAYLOAD and $STEAM_OSX — both $HOME-derived — straight
# into a script that runs on every Steam launch, which turns a quote, a backtick
# or a $(...) anywhere in $HOME into code executed by the launcher.
#
# The three contract lines are printf'd rather than heredoc'd, so `$HOME` still
# reaches the launcher unexpanded while the path BELOW $HOME comes from the
# manifest (#32) — that half used to be hand-synced with the constants at the
# top of this script, which the comment there admitted. What is substituted is a
# repo constant, never anything derived from the user's environment, so the
# injection the quoted heredoc exists to prevent stays prevented.
#
# SHIM_OVERLAY is the other baked-in value, written from a variable this script
# has already constrained to the literal 0 or 1.
{
printf '%s\n' '#!/bin/sh'
printf '%s\n' '# Generated by src/installer/install.sh — do not edit; reinstall instead.'
printf '%s=%s\n' "$SHIM_ENV_OVERLAY" "$OVERLAY_ENV"
cat <<'LAUNCHER'
# These exports are the only thing that delivers the three variables. An
# LSEnvironment dict in Info.plist would be the tidier way, but LaunchServices
# refuses to launch a bundle that has BOTH an LSEnvironment key and a #! script
# as CFBundleExecutable — open/Finder/Raycast fail with -54, surfaced as
# 'does not have permission to open "(null)"'. Exporting here works from every
# launch path, so the key is gone. SHIM_OVERLAY is always stated, 1 or 0, never
# omitted — unset means ON below this point, so omitting it cannot express
# "off". Then hand off to Valve's own binary, unmodified.
: "${HOME:?launcher: HOME is unset — cannot locate the payload or Steam}"
LAUNCHER
printf 'export DYLD_INSERT_LIBRARIES="$HOME/%s"\n'          "$SHIM_PATH_ENABLER_REL"
printf 'export STEAM_EXTRA_COMPAT_TOOLS_PATHS="$HOME/%s"\n' "$SHIM_PATH_COMPAT_TOOLS_REL"
printf 'export %s\n' "$SHIM_ENV_OVERLAY"
printf 'exec "$HOME/%s" "$@"\n'                             "$SHIM_PATH_STEAM_OSX_REL"
} > "$APP/Contents/MacOS/launcher"
chmod +x "$APP/Contents/MacOS/launcher"
log "launcher -> $APP"

log "done. Quit Steam, then launch 'Steam (macOS Play)' so the injector and the"
log "tool path are inherited. Confirm ~/$SHIM_PATH_LOG_ENABLER_REL"
log "says 'patched 1 site(s)'."
