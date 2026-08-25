#!/bin/sh
# Build the shim, both bitnesses (#11 for 64-bit, #20 for 32-bit):
#
#   dist/steamclient64.dll  — PE half for 64-bit titles (Mars),      mingw x86_64
#   dist/steamclient.dll    — PE half for 32-bit titles (Among Us),  mingw i686
#   dist/steamclient64.so   — unix half, clang -arch x86_64, hosts steamclient.dylib
#   dist/steamclient.so     — the SAME unix half under the 32-bit PE's basename
#
# Why one unix half serves both: CrossOver is on new WoW64 (its lib/wine has
# i386-windows and x86_64-unix but NO i386-unix), so 32-bit PE code runs inside
# a 64-bit unix process. The unix side stays x86_64 and keeps hosting the
# x86_64 steamclient.dylib; only the PE side changes bitness. It is deployed
# under two names because ntdll derives the .so name from the builtin PE's own
# basename, and the two PEs must have different basenames (steam_api.dll looks
# for steamclient.dll, steam_api64.dll for steamclient64.dll).
#
# The seam ABI needs no conversion layer between them: every params struct in
# shim_abi.h stores pointers as explicit uint64_t, laid out widest-first, so all
# 112 fields land at identical offsets under both compilers. That is checked
# below, not assumed.
set -eu
cd "$(dirname "$0")"

# The deploy contract (#32): the four artifact names below all come from
# src/layout/layout.json, as does the injector basename shim_pe.c derives.
../layout/build.sh
. ../layout/gen/shim_paths.sh
DIST="$SHIM_PATH_DIST"

MINGW64=x86_64-w64-mingw32-gcc
MINGW32=i686-w64-mingw32-gcc
mkdir -p "$DIST"

# --regen-vtables: re-fetch Proton's PE-side sources and re-read vtables.json.
#
# Generates EVERY interface version Proton defines (#29), not a curated subset.
# The subset was how Space Marine failed: it asks for SteamClient021, which was
# not in the list, so it got a stub vtable and null-dereferenced 20 frames deep
# with nothing in any log naming the cause. Coverage has to be a property of the
# generator, or "works on all titles" is a claim about which games we happened
# to own. interface-versions.txt is now a REGRESSION GUARD (checked below), not
# the input.
if [ "${1:-}" = "--regen-vtables" ]; then
    PROTON_DIR="${PROTON_DIR:-/tmp/proton-lsteamclient}"
    mkdir -p "$PROTON_DIR"
    echo "--- fetching Proton lsteamclient PE-side sources ---"
    gh api "repos/ValveSoftware/Proton/contents/lsteamclient?ref=proton_11.0" --jq '.[].name' \
        | grep '^winISteam.*\.c$' | while read -r f; do
            [ -f "$PROTON_DIR/$f" ] || gh api \
                "repos/ValveSoftware/Proton/contents/lsteamclient/$f?ref=proton_11.0" \
                --jq '.content' | base64 -d > "$PROTON_DIR/$f"
        done
    # Two more headers since #82: Proton states every struct LAYOUT as well as
    # every signature, and which of them actually differ between the Windows and
    # unix forms. That verdict is what decides whether an aggregate can cross the
    # seam untouched, so it is fetched rather than re-derived.
    for h in steamclient_structs_generated.h steamclient_structs.h; do
        [ -f "$PROTON_DIR/$h" ] || gh api \
            "repos/ValveSoftware/Proton/contents/lsteamclient/$h?ref=proton_11.0" \
            --jq '.content' | base64 -d > "$PROTON_DIR/$h"
    done
    python3 extract_vtables.py "$PROTON_DIR" --all > vtables.json
    python3 extract_structs.py "$PROTON_DIR" > structs.json
fi

# The generated tables and thunks are a pure function of vtables.json, which is
# committed — so regenerate them on EVERY build, not only behind --regen-vtables.
# Drift between the checked-in headers and the data they came from is then not a
# state the tree can be in. --regen-vtables re-fetches the DATA; this re-derives
# the code from it (#20 for the tables, #78 for the thunks).
python3 gen_vtables.py vtables.json shim_vtables.h

# overrides.json's "hand-written" entries are a claim about shim_pe.c. Check it
# BEFORE generating: a stale entry means the generator skips a method nothing
# serves, which is exactly the silent-0 failure #43 cost a session to find.
python3 check_overrides.py vtables.json overrides.json shim_pe.c

# The generated thunks dispatch on the native side by INDEXING the dylib's
# vtable with a slot resolved from Proton's MSVC table. That is only correct
# where the MSVC and Itanium orders agree. Check it against the classes a human
# transcribed from the SDK, before generating a thousand callers of the claim.
python3 check_slot_transfer.py vtables.json steam_ifaces.h
python3 gen_thunks.py vtables.json structs.json overrides.json gen

# Every version a real title has been observed to ask for must have a real table.
# This is the guard that would have caught Space Marine before it crashed: the
# list is what titles NEED, vtables.json is what we HAVE, and a version in the
# first but not the second is a title that will fail on a stub.
python3 - <<'PY'
import json, sys
need = [l.strip() for l in open('interface-versions.txt')
        if l.strip() and not l.startswith('#')]
have = set(json.load(open('vtables.json'))['tables'])
missing = [v for v in need if v not in have]
if missing:
    print('ERROR: no slot-exact vtable for versions real titles ask for:', file=sys.stderr)
    for v in missing:
        print('   ' + v, file=sys.stderr)
    print('Run ./build.sh --regen-vtables', file=sys.stderr)
    sys.exit(1)
print('vtables: %d generated, all %d required versions covered' % (len(have), len(need)))
PY

# --- seam ABI is bitness-neutral: prove it, do not assume it ------------------
python3 check_abi_layout.py

# --- unix half (one x86_64 build, deployed under both PE basenames) -----------
clang++ -std=c++17 -arch x86_64 -dynamiclib -O2 -Wall -Wextra \
    -I../layout/gen \
    -o "$DIST/$SHIM_PATH_UNIX64" shim_unix.cpp \
    -Wl,-install_name,"@rpath/$SHIM_PATH_UNIX64"
cp -f "$DIST/$SHIM_PATH_UNIX64" "$DIST/$SHIM_PATH_UNIX32"

# --- PE halves ----------------------------------------------------------------
# -static-libgcc is load-bearing on i686: that toolchain's default is the SJLJ
# unwinder, so without it the DLL imports libgcc_s_sjlj-1.dll, which is not in
# the bottle. The loader then fails the whole DLL with err=126 (MOD_NOT_FOUND)
# and steam_api reports it as a failed sign-in, three layers away (#20). The
# 64-bit build never needed it, which is exactly why it was easy to miss.
$MINGW64 -shared -O2 -Wall -static -static-libgcc -I../layout/gen \
    -o "$DIST/$SHIM_PATH_PE64" shim_pe.c
python3 patch_marker.py "$DIST/$SHIM_PATH_PE64"

$MINGW32 -shared -O2 -Wall -static -static-libgcc -I../layout/gen \
    -o "$DIST/$SHIM_PATH_PE32" shim_pe.c
python3 patch_marker.py "$DIST/$SHIM_PATH_PE32"

# --- i386 thiscall is CALLEE-cleanup: every slot must pop the right bytes -----
$MINGW32 -c -O2 -I../layout/gen -o "$DIST/.shim_pe32.o" shim_pe.c
python3 verify_abi.py vtables.json shim_pe.c "$DIST/.shim_pe32.o" gen/shim_gen_arity.json
rm -f "$DIST/.shim_pe32.o"

# The bottle has no mingw runtime DLLs. Catch a reintroduced dependency here
# rather than as err=126 at load time.
for d in "$DIST/$SHIM_PATH_PE64" "$DIST/$SHIM_PATH_PE32"; do
    case "$d" in *64.dll) OD=x86_64-w64-mingw32-objdump ;; *) OD=i686-w64-mingw32-objdump ;; esac
    if $OD -p "$d" | grep "DLL Name" | grep -qiE "libgcc|libstdc|libwinpthread"; then
        echo "ERROR: $d imports a mingw runtime DLL that is not in the bottle:" >&2
        $OD -p "$d" | grep "DLL Name" | grep -iE "libgcc|libstdc|libwinpthread" >&2
        exit 1
    fi
done

echo "---"
file "$DIST/$SHIM_PATH_UNIX64" "$DIST/$SHIM_PATH_PE64" "$DIST/$SHIM_PATH_PE32"
echo "--- unix exports (must include the two unixlib arrays) ---"
nm -gU "$DIST/$SHIM_PATH_UNIX64" | grep -E "wine_unix_call_funcs|wine_unix_call_wow64_funcs|wine_unix_lib_init" || true
echo "--- PE exports, 64-bit ---"
x86_64-w64-mingw32-objdump -p "$DIST/$SHIM_PATH_PE64" | grep -A40 "Export Address Table" | grep -E "CreateInterface|Steam_" || true
echo "--- PE exports, 32-bit (must be undecorated: steam_api.dll does GetProcAddress by these names) ---"
i686-w64-mingw32-objdump -p "$DIST/$SHIM_PATH_PE32" | grep -A40 "Export Address Table" | grep -E "CreateInterface|Steam_" || true
