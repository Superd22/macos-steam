#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")"
# Universal (arm64 + x86_64): the injector is inherited via DYLD_INSERT_LIBRARIES
# by steam_osx (arm64) AND by the x86_64-under-Rosetta CrossOver children Steam
# spawns; an arch-mismatched slice would make dyld fail to load it into a child.
# It no-ops outside steam_osx (see in_steam_client()), so both slices are safe.
clang -arch arm64 -arch x86_64 -dynamiclib -O2 -Wall -Wextra \
      -o libcompat-enabler.dylib enabler.c
codesign -f -s - libcompat-enabler.dylib
echo "built: $(pwd)/libcompat-enabler.dylib"
