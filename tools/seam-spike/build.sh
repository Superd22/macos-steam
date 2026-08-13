#!/bin/sh
# Build the seam spike (#8 stage 1): PE halves with mingw-w64, unix half with
# clang -arch x86_64. Output goes into a single dist/ dir because CrossOver's
# Wine 10 loader derives the unixlib .so as <WINEDLLPATH-entry>/<name>.so — i.e.
# it expects the .so RIGHT BESIDE the builtin PE on the DllPath, NOT in a
# sibling x86_64-unix/ directory (that upstream-wine detail does not hold here).
# See FINDINGS.md.
set -eu
cd "$(dirname "$0")"

MINGW=x86_64-w64-mingw32-gcc
mkdir -p dist

# Unix half: thin x86_64 Mach-O dylib, exporting the two unixlib symbol arrays.
# C++ now (stage 2 calls through steamclient.dylib's C++ vtables); the exported
# unixlib arrays stay extern "C" so ntdll.so resolves them by their plain names.
clang++ -std=c++17 -arch x86_64 -dynamiclib -O2 -Wall \
    -o dist/bridgetest.so bridge_unix.cpp \
    -Wl,-install_name,@rpath/bridgetest.so

# PE half: mingw builtin DLL, then stamp the Wine-builtin marker at file 0x40.
$MINGW -shared -O2 -Wall -o dist/bridgetest.dll bridge_pe.c
python3 patch_marker.py dist/bridgetest.dll

# The test driver exe.
$MINGW -O2 -Wall -o dist/spike.exe spike_main.c

echo "---"
file dist/bridgetest.so dist/bridgetest.dll dist/spike.exe
nm -gU dist/bridgetest.so
