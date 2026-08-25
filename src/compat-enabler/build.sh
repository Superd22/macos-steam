#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")"
# The deploy contract (#32): the dylib's own basename and the log path it writes
# to are the manifest's, not this script's.
../layout/build.sh
. ../layout/gen/shim_paths.sh
# Universal (arm64 + x86_64): the injector is inherited via DYLD_INSERT_LIBRARIES
# by steam_osx (arm64) AND by the x86_64-under-Rosetta CrossOver children Steam
# spawns; an arch-mismatched slice would make dyld fail to load it into a child.
# It no-ops outside steam_osx (see in_steam_client()), so both slices are safe.
clang -arch arm64 -arch x86_64 -dynamiclib -O2 -Wall -Wextra \
      -I../layout/gen \
      -o "$SHIM_PATH_ENABLER" enabler.c
codesign -f -s - "$SHIM_PATH_ENABLER"
echo "built: $(pwd)/$SHIM_PATH_ENABLER"
