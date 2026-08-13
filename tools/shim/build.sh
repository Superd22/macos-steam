#!/bin/sh
# Build the achievement shim (#11):
#   steamclient64.dll  — PE half, mingw-w64, stamped as a Wine builtin.
#   steamclient64.so   — unix half, clang -arch x86_64, hosts steamclient.dylib.
# Matching basenames so CrossOver's ntdll derives the .so from the builtin PE
# and finds it on WINEDLLPATH (the #8/#10 loader mechanism).
set -eu
cd "$(dirname "$0")"

MINGW=x86_64-w64-mingw32-gcc
mkdir -p dist

clang++ -std=c++17 -arch x86_64 -dynamiclib -O2 -Wall -Wextra \
    -o dist/steamclient64.so shim_unix.cpp \
    -Wl,-install_name,@rpath/steamclient64.so

$MINGW -shared -O2 -Wall -o dist/steamclient64.dll shim_pe.c
python3 patch_marker.py dist/steamclient64.dll

echo "---"
file dist/steamclient64.so dist/steamclient64.dll
echo "--- unix exports (must include the two unixlib arrays) ---"
nm -gU dist/steamclient64.so | grep -E "wine_unix_call_funcs|wine_unix_lib_init" || true
echo "--- PE exports (must include the flat steamclient entry points) ---"
$MINGW -Wl,--version >/dev/null 2>&1 || true
x86_64-w64-mingw32-objdump -p dist/steamclient64.dll | grep -A40 "Export Address Table" | grep -E "CreateInterface|Steam_" || true
