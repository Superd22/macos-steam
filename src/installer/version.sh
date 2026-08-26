#!/bin/sh
# The one place a version string is decided (#42).
#
# The base number lives in the repo's VERSION file, which a release tags. A
# build from a clone is NOT that release — it may carry uncommitted work — so it
# says so: `0.1.0+g1a2b3c4` or `0.1.0+g1a2b3c4.dirty`. A build from an unpacked
# release tarball has no .git and prints the base number alone.
#
# Everything downstream reads this: the payload's VERSION file, the receipt, the
# .app's CFBundleShortVersionString, and the launcher's About. A version typed
# in a second place is a version that lies about which build is installed.
set -eu
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
base="$(tr -d ' \n' < "$REPO/VERSION")"
if git -C "$REPO" rev-parse --git-dir >/dev/null 2>&1; then
    sha="$(git -C "$REPO" rev-parse --short HEAD 2>/dev/null || echo unknown)"
    dirty=""
    git -C "$REPO" diff --quiet HEAD -- 2>/dev/null || dirty=".dirty"
    printf '%s+g%s%s\n' "$base" "$sha" "$dirty"
else
    printf '%s\n' "$base"
fi
