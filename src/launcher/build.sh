#!/bin/sh
# Build the launcher app's binary (#42).
#
#   ./src/launcher/build.sh        -> dist/launcher
#
# One Mach-O, arm64, ad-hoc signed. It is put INTO a bundle by deploy.sh, not
# here: which bundle, under what name, and beside what nested helper is deploy
# contract (ADR 0010), and this half only has to produce the executable.
#
# Built locally, always. That is not a preference — it is the shipping story
# (#42): an unsigned, un-notarized app downloaded as a DMG is blocked behind
# System Settings on macOS 14+, but a locally built binary carries no quarantine
# bit, so Gatekeeper never interrogates it. The brew formula builds from source
# for the same reason.
set -eu
cd "$(dirname "$0")"

# The deploy contract, third dialect (#32, #42). Regenerate first: the launcher
# compiles the payload paths in exactly as the C halves do, and building against
# a stale contract is the drift the manifest exists to end.
../layout/build.sh

# The output dir is a manifest name like every other shipped path (#32).
. ../layout/gen/shim_paths.sh
OUT="$SHIM_PATH_DIST"
mkdir -p "$OUT"

# Sources in dependency-free order — swiftc takes the whole module at once, so
# the order is for readers, not for the compiler.
swiftc -O -swift-version 5 \
    -target arm64-apple-macos14 \
    -o "$OUT/$SHIM_PATH_LAUNCHER_BIN" \
    ../layout/gen/ShimPaths.swift \
    ../layout/gen/ShimPolicy.swift \
    Shell.swift Prefs.swift Receipt.swift LogWatch.swift Preflight.swift \
    Diagnose.swift Launch.swift AppModel.swift UI.swift \
    main.swift

# Ad-hoc, like the injector dylib beside it. There is no Developer ID yet; when
# there is, this is the line that changes and nothing else does.
codesign -f -s - "$OUT/$SHIM_PATH_LAUNCHER_BIN"

ls -l "$OUT/$SHIM_PATH_LAUNCHER_BIN"
echo "built launcher (arm64, ad-hoc signed)"

# #85's acceptance test, run on every build: the merge into
# DYLD_INSERT_LIBRARIES is the one behaviour here that a plausible-looking
# rewrite silently breaks, and it is invisible until another tool's insertion
# goes missing three layers away. It needs a deployed payload to compare
# against and says so — and skips — when there is none.
./check_launch_env.sh
