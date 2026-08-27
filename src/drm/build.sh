#!/bin/sh
# Build the DRM route's two shadow libraries (ADR 0014).
#
#   dist/tier0_s64.dll    the hook: installs the trampolines and neuters
#                         Valve's steamclient64.dll entry point
#   dist/vstdlib_s64.dll  inert; exists only because Valve's real one cannot
#                         initialise against a shadowed tier0
#
# Both are ordinary native PEs -- NOT Wine builtins. They have no unix half and
# nothing checks their signature, so the marker that makes our shim a builtin
# would only confuse the loader here.
#
# The export lists are GENERATED from Valve's own libraries and committed, the
# way vtables.json is: the build stays offline, and `--regen` is the explicit
# step that goes and looks. Regenerating needs the reference DLLs, which
# fetch.sh puts in the cache.
#
#   ./build.sh [--regen]
set -eu
cd "$(dirname "$0")"

../layout/build.sh
. ../layout/gen/shim_paths.sh
DIST="$SHIM_PATH_DIST"
MINGW64=x86_64-w64-mingw32-gcc
mkdir -p "$DIST" gen

CACHE="$HOME/$SHIM_PATH_CLIENT_CACHE_REL"

if [ "${1:-}" = "--regen" ]; then
    [ -f "$CACHE/$SHIM_PATH_SHADOW_TIER0" ] || ./fetch.sh
    echo "--- regenerating shadow export lists from Valve's libraries ---"
    python3 gen_shadow.py "$CACHE/$SHIM_PATH_SHADOW_TIER0" \
        gen/tier0_stubs.c gen/tier0.def t0s
    python3 gen_shadow.py "$CACHE/$SHIM_PATH_SHADOW_VSTDLIB" \
        gen/vstdlib_stubs.c gen/vstdlib.def vls
fi

for f in gen/tier0_stubs.c gen/tier0.def gen/vstdlib_stubs.c gen/vstdlib.def; do
    [ -f "$f" ] || { echo "missing $f -- run ./build.sh --regen"; exit 2; }
done

# The generated set is a SUPERSET of what any one client build imports (it comes
# from the reference library's exports, not steamclient64's imports). Check it
# anyway when the reference is present: a Valve build that dropped an export we
# still claim is harmless, but one that ADDED an import we do not export makes
# steamclient64.dll fail to bind at load, with nothing in any log naming the
# reason -- and this project's whole argument about #45 is that such a failure
# costs a session.
if [ -f "$CACHE/$SHIM_PATH_PE64" ]; then
    python3 check_shadow.py "$CACHE/$SHIM_PATH_PE64" \
        "$SHIM_PATH_SHADOW_TIER0"   gen/tier0.def \
        "$SHIM_PATH_SHADOW_VSTDLIB" gen/vstdlib.def
else
    echo "drm: no fetched $SHIM_PATH_PE64 to check the shadows against (run ./fetch.sh)"
fi

$MINGW64 -shared -O2 -Wall -static -static-libgcc -I../layout/gen \
    -o "$DIST/$SHIM_PATH_SHADOW_TIER0" shadow_tier0.c gen/tier0_stubs.c gen/tier0.def
$MINGW64 -shared -O2 -Wall -static -static-libgcc -I../layout/gen \
    -o "$DIST/$SHIM_PATH_SHADOW_VSTDLIB" shadow_vstdlib.c gen/vstdlib_stubs.c gen/vstdlib.def

# The bottle has no mingw runtime DLLs; an accidental dependency here fails at
# load time as err=126 with the title reporting a Steam sign-in problem (#20).
for f in "$SHIM_PATH_SHADOW_TIER0" "$SHIM_PATH_SHADOW_VSTDLIB"; do
    if x86_64-w64-mingw32-objdump -p "$DIST/$f" | grep -qE 'DLL Name: (libgcc|libwinpthread|libstdc)'; then
        echo "$f pulled in a mingw runtime dependency"; exit 2
    fi
done

echo "--- built ---"
for f in "$SHIM_PATH_SHADOW_TIER0" "$SHIM_PATH_SHADOW_VSTDLIB"; do
    printf '%-18s %s\n' "$f" "$(file -b "$DIST/$f")"
done
