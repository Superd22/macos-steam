#!/bin/sh
# Build half of the install (#34): turn this repo into a PAYLOAD — a directory
# tree that is exactly what lands on a machine, with nothing left to compile.
#
#   ./src/installer/build.sh [outdir]      default: src/installer/dist/payload
#
# The split exists because deploying was expressed *as* build orchestration: a
# user installing a beta needed Xcode CLT and mingw-w64, because no path could
# lay down an already-built payload. Now there are two halves and one seam —
# a directory — so the second and third callers (the brew formula, the launcher
# app) build nothing and only ever call deploy.sh against a tree like this one.
#
# The payload is self-describing on purpose. Beside the artifacts it carries the
# contract fragments, its version, and deploy.sh itself, because at deploy time
# the repo may not exist (a release tarball is unpacked, built, and the repo is
# then gone) and at UNINSTALL time it certainly does not.
set -eu

REPO="$(cd "$(dirname "$0")/../.." && pwd)"
STAGE="${1:-$REPO/src/installer/dist/payload}"

# --- the deploy contract (#32) ------------------------------------------------
# Regenerate first: building against a stale contract is the drift the manifest
# exists to end. Sourcing it gives us every basename below as a name.
"$REPO/src/layout/build.sh"
. "$REPO/src/layout/gen/shim_paths.sh"

log() { printf '[build] %s\n' "$*"; }

VERSION="$("$REPO/src/installer/version.sh")"

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
DRM_DIST_DIR="$REPO/src/drm/$SHIM_PATH_DIST"

# The shim's sources are no longer only its C and C++: since #78 the thunks are
# GENERATED, so the generator, its inputs and the checkers that gate it are all
# build inputs. #34 called this out as a gap — editing interface-versions.txt
# alone left a stale dist/, and a stale dist/ is a title that cannot sign in.
if [ ! -f "$SHIM_DIST_DIR/$SHIM_PATH_PE32" ] \
   || stale "$SHIM_DIST_DIR/$SHIM_PATH_PE64" \
            "$REPO/src/shim/shim_pe.c"      "$REPO/src/shim/shim_unix.cpp" \
            "$REPO/src/shim/shim_abi.h"     "$REPO/src/shim/shim_vtables.h" \
            "$REPO/src/shim/steam_ifaces.h" "$REPO/src/shim/build.sh" \
            "$REPO/src/shim/vtables.json"   "$REPO/src/shim/structs.json" \
            "$REPO/src/shim/overrides.json" "$REPO/src/shim/interface-versions.txt" \
            $(find "$REPO/src/shim" -maxdepth 1 -name '*.py') \
            "$PATHS_H" "$POLICY_H"; then
    log "building shim (both bitnesses)"; "$REPO/src/shim/build.sh"
fi

# The DRM route's two shadows (ADR 0014). Their export lists are generated from
# Valve's own libraries and committed, so this build stays offline; `--regen` in
# that module is the step that goes and looks.
if [ ! -f "$DRM_DIST_DIR/$SHIM_PATH_SHADOW_VSTDLIB" ] \
   || stale "$DRM_DIST_DIR/$SHIM_PATH_SHADOW_TIER0" \
            "$REPO/src/drm/shadow_tier0.c" "$REPO/src/drm/shadow_vstdlib.c" \
            "$REPO/src/drm/gen/tier0.def"  "$REPO/src/drm/gen/vstdlib.def" \
            "$REPO/src/drm/build.sh" "$PATHS_H"; then
    log "building DRM shadows"; "$REPO/src/drm/build.sh"
fi

if [ ! -f "$INJECT_DIST_DIR/$SHIM_PATH_INJECT32" ] \
   || stale "$INJECT_DIST_DIR/$SHIM_PATH_INJECT64" \
            "$REPO/src/overlay-inject/overlayinject.c" \
            "$REPO/src/overlay-inject/build.sh" "$PATHS_H"; then
    log "building overlay injector"; "$REPO/src/overlay-inject/build.sh"
fi

# The launcher app (#42). Optional in exactly one direction: a machine without
# a Swift toolchain still produces a deployable payload, and deploy.sh writes
# the shell launcher it always wrote. A machine WITH one always builds it, so a
# release tarball is never quietly missing the app.
if command -v swiftc >/dev/null 2>&1; then
    if stale "$REPO/src/launcher/$SHIM_PATH_DIST/$SHIM_PATH_LAUNCHER_BIN" \
             $(find "$REPO/src/launcher" -maxdepth 1 -name '*.swift') \
             "$REPO/src/launcher/build.sh" "$PATHS_H" "$POLICY_H"; then
        log "building launcher"; "$REPO/src/launcher/build.sh" >/dev/null
    fi
else
    log "no swiftc — payload will carry no launcher app; deploy writes the shell one"
fi

# --- stage the payload --------------------------------------------------------
# From here down nothing compiles: this is the shape the tree has on the user's
# machine, one directory below $HOME's payload root, so deploy.sh can be a copy
# and a symlink swap rather than a second assembly.
rm -rf "$STAGE"
TOOLDIR="$STAGE/$SHIM_PATH_COMPAT_TOOLS/$SHIM_PATH_TOOL_NAME"
mkdir -p "$TOOLDIR/$SHIM_PATH_DIST"

cp -f "$REPO/src/compat-enabler/$SHIM_PATH_ENABLER" "$STAGE/"
cp -f "$REPO/src/compat-tool/$SHIM_PATH_LAUNCH_SH"  "$TOOLDIR/"
# The contract travels with the launch script: once deployed it is the only copy
# either can see, so the script sources it from beside itself (#32). It travels
# at the payload ROOT too, because deploy.sh reads the same names to decide
# where any of this goes, and it has no repo to read them from.
for f in "$SHIM_PATH_PATHS_SH" "$SHIM_PATH_POLICY_SH"; do
    cp -f "$REPO/src/layout/gen/$f" "$TOOLDIR/"
    cp -f "$REPO/src/layout/gen/$f" "$STAGE/"
done
cp -f "$REPO/src/installer/deploy.sh" "$STAGE/"
cp -f "$REPO/src/layout/layout.json"  "$STAGE/"

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
# bottle and routes through it when the overlay switch is on. Without both
# bitnesses the script leaves the overlay off rather than arming an env it
# cannot deliver.
cp -f "$INJECT_DIST_DIR/$SHIM_PATH_INJECT64" "$TOOLDIR/$SHIM_PATH_DIST/"
cp -f "$INJECT_DIST_DIR/$SHIM_PATH_INJECT32" "$TOOLDIR/$SHIM_PATH_DIST/"
# The DRM route (ADR 0014): the shim under its second name, and the two shadows
# that trampoline Valve's signed DLL into it. Valve's DLL itself is NOT shipped —
# it is fetched from Valve at provision time, per machine. A payload missing
# these deploys fine and simply leaves DRM-wrapped titles failing as they did
# before, which is the same interlock the overlay uses.
cp -f "$SHIM_DIST_DIR/$SHIM_PATH_LSTEAM_PE64"     "$TOOLDIR/$SHIM_PATH_DIST/"
cp -f "$SHIM_DIST_DIR/$SHIM_PATH_LSTEAM_UNIX64"   "$TOOLDIR/$SHIM_PATH_DIST/"
cp -f "$DRM_DIST_DIR/$SHIM_PATH_SHADOW_TIER0"     "$TOOLDIR/$SHIM_PATH_DIST/"
cp -f "$DRM_DIST_DIR/$SHIM_PATH_SHADOW_VSTDLIB"   "$TOOLDIR/$SHIM_PATH_DIST/"
cp -f "$REPO/src/drm/$SHIM_PATH_FETCH_SH"         "$TOOLDIR/"
cp -f "$REPO/src/drm/check_shadow.py"             "$TOOLDIR/"
chmod +x "$TOOLDIR/$SHIM_PATH_LAUNCH_SH" "$STAGE/deploy.sh"

# The launcher, if it has been built. It is a separate module with its own
# toolchain (Swift), and a payload without it still deploys — deploy.sh falls
# back to the shell launcher it has always emitted.
if [ -x "$REPO/src/launcher/$SHIM_PATH_DIST/$SHIM_PATH_LAUNCHER_BIN" ]; then
    cp -f "$REPO/src/launcher/$SHIM_PATH_DIST/$SHIM_PATH_LAUNCHER_BIN" "$STAGE/"
fi

# Version and compatibility statement: the payload states what it is and what it
# was exercised against, so the receipt, the launcher's About and a release page
# all read one file rather than three restatements.
printf '%s\n' "$VERSION" > "$STAGE/VERSION"
cp -f "$REPO/src/installer/compatibility.env" "$STAGE/"

log "payload $VERSION staged -> $STAGE"
printf '%s\n' "$STAGE"
