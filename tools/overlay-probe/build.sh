#!/bin/sh
# Build the overlay probes. ARCH=x86_64 to match the bottle's Rosetta side.
set -e
cd "$(dirname "$0")"
ARCH="${ARCH:-arm64}"
FW="-framework Cocoa -framework Metal -framework QuartzCore"
clang -c -arch "$ARCH" fishhook.c -o fishhook.o
clang -arch "$ARCH" -fobjc-arc $FW -o metalprobe  metalprobe.m
clang -arch "$ARCH" -fobjc-arc $FW fishhook.o -o metalprobe3 metalprobe3.m
clang -arch "$ARCH" -fobjc-arc $FW -o metalprobe5 metalprobe5.m
echo "built metalprobe, metalprobe3, metalprobe5 ($ARCH)"

# inputprobe — the #28 oracle. A PE, so mingw rather than clang: it has to run
# INSIDE the bottle to be a real overlay target and to see the pad through
# winebus/xinput/dinput the way a title does. Both bitnesses, because #28 asks
# for the answer on both (Mars is 64-bit, Among Us 32-bit).
if command -v x86_64-w64-mingw32-gcc >/dev/null 2>&1; then
    x86_64-w64-mingw32-gcc -O1 -mwindows -o inputprobe64.exe inputprobe.c \
        -ldinput8 -ldxguid -luuid -lole32
    i686-w64-mingw32-gcc   -O1 -mwindows -o inputprobe32.exe inputprobe.c \
        -ldinput8 -ldxguid -luuid -lole32
    echo "built inputprobe64.exe, inputprobe32.exe"
else
    echo "skipping inputprobe: mingw not installed"
fi
