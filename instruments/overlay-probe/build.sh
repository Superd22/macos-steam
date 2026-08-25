#!/bin/sh
# Build the overlay instruments. Both are PEs, so mingw rather than clang: they
# have to run INSIDE the bottle to be real overlay targets.
set -e
cd "$(dirname "$0")"

if ! command -v x86_64-w64-mingw32-gcc >/dev/null 2>&1; then
    echo "mingw not installed — nothing to build"; exit 1
fi

# inputprobe — the #28 oracle. Both bitnesses, because #28 asks for the answer
# on both (Mars is 64-bit, Among Us 32-bit); it sees the pad through
# winebus/xinput/dinput the way a title does.
x86_64-w64-mingw32-gcc -O1 -mwindows -o inputprobe64.exe inputprobe.c \
    -ldinput8 -ldxguid -luuid -lole32
i686-w64-mingw32-gcc   -O1 -mwindows -o inputprobe32.exe inputprobe.c \
    -ldinput8 -ldxguid -luuid -lole32
echo "built inputprobe64.exe, inputprobe32.exe"

# d3dprobe — the #26 oracle: a real D3D11 device, swap chain and Present, so the
# renderer is measured on the path a title actually renders through.
x86_64-w64-mingw32-gcc -mwindows -o d3dprobe.exe d3dprobe.c -luuid
echo "built d3dprobe.exe"
