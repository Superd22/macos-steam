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
PAYLOAD="$HOME/Library/Application Support/macos-steam-shim"
APP="$HOME/Applications/Steam (macOS Play).app"
# Kept in sync by hand with the same path inside the generated launcher below,
# which resolves it from its own $HOME rather than having it substituted in.
STEAM_OSX="$HOME/Library/Application Support/Steam/Steam.AppBundle/Steam/Contents/MacOS/steam_osx"
TOOLDIR="$PAYLOAD/compatibilitytools.d/crossover-steam-shim"

log() { printf '[install] %s\n' "$*"; }

# Overlay (#21). ON by default: the flag is baked into the launcher so the compat
# tool sees it — Steam does not forward arbitrary env to a compat tool, so the
# launcher is where it has to live. `SHIM_OVERLAY=0 ./install.sh` opts out.
#
# The launcher always states the value rather than relying on the default. A
# reinstall is how a user turns the overlay off, and "off" has to mean an
# explicit 0 reaching the bottle: every layer below now treats *unset* as ON, so
# an absent variable would silently mean the opposite of what was asked for.
OVERLAY_ENV=1
if [ "${SHIM_OVERLAY:-1}" = 0 ]; then
    OVERLAY_ENV=0
    log "SHIM_OVERLAY=0 baked into the launcher (overlay disabled)"
else
    log "overlay ON (default) — SHIM_OVERLAY=0 ./install.sh to disable"
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

if stale "$REPO/src/compat-enabler/libcompat-enabler.dylib" \
         "$REPO/src/compat-enabler/enabler.c" \
         "$REPO/src/compat-enabler/build.sh"; then
    log "building injector"; "$REPO/src/compat-enabler/build.sh"
fi
if [ ! -f "$REPO/src/shim/dist/steamclient.dll" ] \
   || stale "$REPO/src/shim/dist/steamclient64.dll" \
            "$REPO/src/shim/shim_pe.c"      "$REPO/src/shim/shim_unix.cpp" \
            "$REPO/src/shim/shim_abi.h"     "$REPO/src/shim/shim_vtables.h" \
            "$REPO/src/shim/steam_ifaces.h" "$REPO/src/shim/build.sh"; then
    log "building shim (both bitnesses)"; "$REPO/src/shim/build.sh"
fi
if [ ! -f "$REPO/src/overlay-inject/dist/overlayinject32.exe" ] \
   || stale "$REPO/src/overlay-inject/dist/overlayinject64.exe" \
            "$REPO/src/overlay-inject/overlayinject.c" \
            "$REPO/src/overlay-inject/build.sh"; then
    log "building overlay injector"; "$REPO/src/overlay-inject/build.sh"
fi

# --- payload ------------------------------------------------------------------
mkdir -p "$TOOLDIR/dist"
cp -f "$REPO/src/compat-enabler/libcompat-enabler.dylib" "$PAYLOAD/"
cp -f "$REPO/src/compat-tool/steamclient-shim-launch.sh" "$TOOLDIR/"
cp -f "$REPO/src/compat-tool/toolmanifest.vdf"           "$TOOLDIR/"
cp -f "$REPO/src/compat-tool/compatibilitytool.vdf"      "$TOOLDIR/"
cp -f "$REPO/src/shim/dist/steamclient64.dll"            "$TOOLDIR/dist/"
cp -f "$REPO/src/shim/dist/steamclient64.so"             "$TOOLDIR/dist/"
# Both bitnesses: 32-bit titles load steam_api.dll -> steamclient.dll under the
# SteamClientDll registry value, a different file and a different value from the
# 64-bit pair (#20). Shipping only the 64-bit half is what made Among Us report
# "Could not sign in to your Steam account".
cp -f "$REPO/src/shim/dist/steamclient.dll"              "$TOOLDIR/dist/"
cp -f "$REPO/src/shim/dist/steamclient.so"               "$TOOLDIR/dist/"
# The overlay injector (#25, ADR 0003) — the launch script plants it in the
# bottle and routes through it when SHIM_OVERLAY=1. Without both bitnesses the
# script leaves the overlay off rather than arming an env it cannot deliver.
cp -f "$REPO/src/overlay-inject/dist/overlayinject64.exe" "$TOOLDIR/dist/"
cp -f "$REPO/src/overlay-inject/dist/overlayinject32.exe" "$TOOLDIR/dist/"
chmod +x "$TOOLDIR/steamclient-shim-launch.sh"
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
# or a $(...) anywhere in $HOME into code executed by the launcher. Same paths,
# same values, no substitution step to get wrong.
#
# SHIM_OVERLAY is the one baked-in value, so it is written separately, from a
# variable this script has already constrained to the literal 0 or 1.
{
printf '%s\n' '#!/bin/sh'
printf '%s\n' '# Generated by src/installer/install.sh — do not edit; reinstall instead.'
printf 'SHIM_OVERLAY=%s\n' "$OVERLAY_ENV"
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
PAYLOAD="$HOME/Library/Application Support/macos-steam-shim"
export DYLD_INSERT_LIBRARIES="$PAYLOAD/libcompat-enabler.dylib"
export STEAM_EXTRA_COMPAT_TOOLS_PATHS="$PAYLOAD/compatibilitytools.d"
export SHIM_OVERLAY
exec "$HOME/Library/Application Support/Steam/Steam.AppBundle/Steam/Contents/MacOS/steam_osx" "$@"
LAUNCHER
} > "$APP/Contents/MacOS/launcher"
chmod +x "$APP/Contents/MacOS/launcher"
log "launcher -> $APP"

log "done. Quit Steam, then launch 'Steam (macOS Play)' so the injector and the"
log "tool path are inherited. Confirm ~/Library/Logs/macos-steam-shim/compat-enabler.log"
log "says 'patched 1 site(s)'."
