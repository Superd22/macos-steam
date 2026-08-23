#!/bin/sh
# Build the overlay injector, both bitnesses (#25, ADR 0003).
#
#   dist/overlayinject64.exe   for 64-bit titles
#   dist/overlayinject32.exe   for 32-bit titles
#
# Both are built from one source. Which one runs is decided at runtime: the
# injector reads the title's PE header and hands over to its sibling if the
# bitness disagrees, because CreateRemoteThread cannot cross that boundary.
# The compat tool therefore only ever needs to call the 64-bit one.
#
# Console subsystem is deliberate here (unlike the probes): the injector is not
# the process that races winemac.so — the TITLE is — and stdio must pass through
# to Steam.
set -eu
cd "$(dirname "$0")"

MINGW64=x86_64-w64-mingw32-gcc
MINGW32=i686-w64-mingw32-gcc
mkdir -p dist

for cc in "$MINGW64" "$MINGW32"; do
    command -v "$cc" >/dev/null || { echo "missing $cc (brew install mingw-w64)"; exit 2; }
done

echo "--- 64-bit ---"
"$MINGW64" -O2 -Wall -Wextra -o dist/overlayinject64.exe overlayinject.c
echo "--- 32-bit ---"
"$MINGW32" -O2 -Wall -Wextra -o dist/overlayinject32.exe overlayinject.c

ls -l dist/
echo "built overlayinject64.exe, overlayinject32.exe"
