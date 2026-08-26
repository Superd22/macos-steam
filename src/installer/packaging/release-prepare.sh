#!/bin/sh
# Everything a release needs built, before anything is published (#42).
#
#   src/installer/packaging/release-prepare.sh <version> [outdir]
#
# Called by semantic-release's `prepare` step, and runnable by hand — the same
# code path either way, which is the point: a release path that only exists
# inside CI is one nobody can test before it runs.
#
# It deliberately does NOT read a git tag. semantic-release computes the next
# version, hands it here, and only creates the tag AFTER prepare succeeds — so
# at this moment the tag does not exist yet and `git archive <tag>` would fail.
# The tarball is therefore built from the WORKING TREE, filtered through
# `git ls-files` so only tracked sources go in. That also means the VERSION
# file written below is the one that ships, without a commit in between.
#
# Order matters, and every step is a gate on the next:
#
#   1. stamp VERSION            the number everything downstream reads
#   2. build                    every checker this repo has runs inside this
#   3. tarball                  tracked sources only, no dist/, no .git
#   4. build the TARBALL        the claim a release actually makes
#   5. render the formula       against the checksum of what step 3 produced
#
# If any of it fails, semantic-release aborts before the release exists. A
# published release that cannot be installed is worse than no release.
set -eu

VERSION="${1:?usage: release-prepare.sh <version> [outdir]}"
REPO="$(cd "$(dirname "$0")/../../.." && pwd)"
OUT="${2:-$REPO/dist-release}"
NAME="macos-steam-shim-$VERSION"

log() { printf '[release] %s\n' "$*"; }

rm -rf "$OUT"; mkdir -p "$OUT"

# --- 1. stamp -----------------------------------------------------------------
# version.sh reads this file, and everything else reads version.sh. Writing it
# before the build is what makes the payload, the receipt, the .app's
# Info.plist and the formula all agree without any of them being told twice.
log "stamping VERSION = $VERSION"
printf '%s\n' "$VERSION" > "$REPO/VERSION"

# --- 2. build -----------------------------------------------------------------
# The gates are not a separate step anywhere in this project: the layout drift
# check, the policy agreement check, the four shim checkers and the launcher's
# DYLD merge test all run as part of this.
log "building (runs every gate)"
"$REPO/src/installer/build.sh" >/dev/null

# --- 3. tarball ---------------------------------------------------------------
# What ships is SOURCE. There is no Developer ID, and a locally built binary is
# the only one macOS runs without sending the user to System Settings, so the
# release is the input to a build and the formula performs it (ADR 0002).
#
# git ls-files, not the directory: dist/ and gen/ are ignored, so nothing built
# in step 2 can leak in. attic/ and instruments/ are left out by the pathspec —
# ADR 0004's three roots say src/ ships and the other two measure.
log "tarball $NAME.tar.gz"
stage="$(mktemp -d)/$NAME"
mkdir -p "$stage"
( cd "$REPO" && git ls-files src VERSION LICENSE NOTICE README.md CONTEXT.md CHANGELOG.md docs ) > "$OUT/.filelist"
rsync -a --files-from="$OUT/.filelist" "$REPO/" "$stage/"
# VERSION was just written and is not committed yet, so take the working copy.
printf '%s\n' "$VERSION" > "$stage/VERSION"
tar -czf "$OUT/$NAME.tar.gz" -C "$(dirname "$stage")" "$NAME"
rm -f "$OUT/.filelist"

( cd "$OUT" && shasum -a 256 "$NAME.tar.gz" > "$NAME.tar.gz.sha256" )
SHA256="$(awk '{print $1}' < "$OUT/$NAME.tar.gz.sha256")"
log "sha256 $SHA256"

# --- 4. the tarball itself builds ---------------------------------------------
# Building the checkout proves the repo builds, which is not the claim a release
# makes. The claim is that what a user DOWNLOADS builds — with no .git, no
# attic/, no instruments/ — and stamps the bare release version rather than the
# +gSHA spelling version.sh emits inside a clone.
log "verifying the tarball builds as unpacked"
work="$(mktemp -d)"
tar -xzf "$OUT/$NAME.tar.gz" -C "$work"
"$work/$NAME/src/installer/build.sh" "$work/payload" >/dev/null
stamped="$(cat "$work/payload/VERSION")"
[ "$stamped" = "$VERSION" ] || {
    printf '[release] tarball payload stamped %s, expected %s\n' "$stamped" "$VERSION" >&2
    exit 1
}
"$work/payload/deploy.sh" --dry-run >/dev/null
rm -rf "$work"
log "tarball builds and stamps $stamped"

# --- 5. the formula -----------------------------------------------------------
# The URL is where @semantic-release/github will put the asset from step 3.
URL="https://github.com/Superd22/macos-steam/releases/download/v$VERSION/$NAME.tar.gz"
"$REPO/src/installer/packaging/render-formula.sh" "$VERSION" "$URL" "$SHA256" \
    "$OUT/macos-steam-shim.rb" >/dev/null
log "rendered formula -> $OUT/macos-steam-shim.rb"

log "ready: $(ls "$OUT" | tr '\n' ' ')"
