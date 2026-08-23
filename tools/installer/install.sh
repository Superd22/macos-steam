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
#      An unhardened .app whose LSEnvironment carries DYLD_INSERT_LIBRARIES (the
#      injector) and STEAM_EXTRA_COMPAT_TOOLS_PATHS (the payload's tool dir),
#      then execs Valve's inner steam_osx. Touches zero Valve files.
#
# Why this script exists: the payload was hand-assembled once and then drifted —
# it still held the pre-#19 shim (old wineloader launch, no CWD fix) long after
# the repo had moved on, which is invisible until a launch mysteriously fails.
# Deployment must be one reproducible command that always ships what was built.
#
#   ./tools/installer/install.sh            deploy (builds first if needed)
#   ./tools/installer/install.sh --uninstall  remove payload + launcher
set -eu

REPO="$(cd "$(dirname "$0")/../.." && pwd)"
PAYLOAD="$HOME/Library/Application Support/macos-steam-shim"
APP="$HOME/Applications/Steam (macOS Play).app"
STEAM_OSX="$HOME/Library/Application Support/Steam/Steam.AppBundle/Steam/Contents/MacOS/steam_osx"
TOOLDIR="$PAYLOAD/compatibilitytools.d/crossover-steam-shim"

log() { printf '[install] %s\n' "$*"; }

# Overlay spike (#21). SHIM_OVERLAY=1 ./install.sh bakes the flag into the
# launcher so the compat tool sees it — Steam does not forward arbitrary env to
# a compat tool, so the launcher is where it has to live. Default off.
OVERLAY_ENV=""
if [ "${SHIM_OVERLAY:-0}" = 1 ]; then
    OVERLAY_ENV=1
    log "SHIM_OVERLAY=1 baked into the launcher (overlay spike)"
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
[ -f "$REPO/tools/compat-enabler/libcompat-enabler.dylib" ] \
    || { log "building injector"; "$REPO/tools/compat-enabler/build.sh"; }
[ -f "$REPO/tools/shim/dist/steamclient64.dll" ] && [ -f "$REPO/tools/shim/dist/steamclient.dll" ] \
    || { log "building shim (both bitnesses)"; "$REPO/tools/shim/build.sh"; }
[ -f "$REPO/tools/overlay-inject/dist/overlayinject64.exe" ] \
    && [ -f "$REPO/tools/overlay-inject/dist/overlayinject32.exe" ] \
    || { log "building overlay injector"; "$REPO/tools/overlay-inject/build.sh"; }

# --- payload ------------------------------------------------------------------
mkdir -p "$TOOLDIR/dist"
cp -f "$REPO/tools/compat-enabler/libcompat-enabler.dylib" "$PAYLOAD/"
cp -f "$REPO/tools/compat-tool/steamclient-shim-launch.sh" "$TOOLDIR/"
cp -f "$REPO/tools/compat-tool/toolmanifest.vdf"           "$TOOLDIR/"
cp -f "$REPO/tools/compat-tool/compatibilitytool.vdf"      "$TOOLDIR/"
cp -f "$REPO/tools/shim/dist/steamclient64.dll"            "$TOOLDIR/dist/"
cp -f "$REPO/tools/shim/dist/steamclient64.so"             "$TOOLDIR/dist/"
# Both bitnesses: 32-bit titles load steam_api.dll -> steamclient.dll under the
# SteamClientDll registry value, a different file and a different value from the
# 64-bit pair (#20). Shipping only the 64-bit half is what made Among Us report
# "Could not sign in to your Steam account".
cp -f "$REPO/tools/shim/dist/steamclient.dll"              "$TOOLDIR/dist/"
cp -f "$REPO/tools/shim/dist/steamclient.so"               "$TOOLDIR/dist/"
# The overlay injector (#25, ADR 0003) — the launch script plants it in the
# bottle and routes through it when SHIM_OVERLAY=1. Without both bitnesses the
# script leaves the overlay off rather than arming an env it cannot deliver.
cp -f "$REPO/tools/overlay-inject/dist/overlayinject64.exe" "$TOOLDIR/dist/"
cp -f "$REPO/tools/overlay-inject/dist/overlayinject32.exe" "$TOOLDIR/dist/"
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
  <key>LSEnvironment</key>
  <dict>
    <key>DYLD_INSERT_LIBRARIES</key><string>$PAYLOAD/libcompat-enabler.dylib</string>
    <key>STEAM_EXTRA_COMPAT_TOOLS_PATHS</key><string>$PAYLOAD/compatibilitytools.d</string>${OVERLAY_ENV:+
    <key>SHIM_OVERLAY</key><string>1</string>}
  </dict>
</dict>
</plist>
PLIST
cat > "$APP/Contents/MacOS/launcher" <<LAUNCHER
#!/bin/sh
# LSEnvironment above is what actually delivers the two variables; exporting
# them here too keeps a direct invocation of this script equivalent to a Finder
# launch. Then hand off to Valve's own binary, unmodified.
export DYLD_INSERT_LIBRARIES="$PAYLOAD/libcompat-enabler.dylib"
export STEAM_EXTRA_COMPAT_TOOLS_PATHS="$PAYLOAD/compatibilitytools.d"${OVERLAY_ENV:+
export SHIM_OVERLAY=1}
exec "$STEAM_OSX" "\$@"
LAUNCHER
chmod +x "$APP/Contents/MacOS/launcher"
log "launcher -> $APP"

log "done. Quit Steam, then launch 'Steam (macOS Play)' so the injector and the"
log "tool path are inherited. Confirm /tmp/compat-enabler.log says 'patched 1 site(s)'."
