#!/bin/sh
# Build the overlay probes. ARCH=x86_64 to match the bottle's Rosetta side.
set -e
cd "$(dirname "$0")"
ARCH="${ARCH:-arm64}"
FW="-framework Cocoa -framework Metal -framework QuartzCore"
clang -c -arch "$ARCH" fishhook.c -o fishhook.o
clang -arch "$ARCH" -fobjc-arc $FW -o metalprobe  metalprobe.m
clang -arch "$ARCH" -fobjc-arc $FW fishhook.o -o metalprobe3 metalprobe3.m
echo "built metalprobe, metalprobe3 ($ARCH)"
