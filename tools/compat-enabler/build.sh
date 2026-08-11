#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")"
clang -arch arm64 -dynamiclib -O2 -Wall -Wextra \
      -o libcompat-enabler.dylib enabler.c
codesign -f -s - libcompat-enabler.dylib
echo "built: $(pwd)/libcompat-enabler.dylib"
