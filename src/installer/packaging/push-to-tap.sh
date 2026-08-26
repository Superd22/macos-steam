#!/bin/sh
# Publish the rendered formula to the Homebrew tap (#42).
#
#   src/installer/packaging/push-to-tap.sh <version> [formula]
#
# Called by semantic-release's `success` step — after the release exists and
# its assets are uploaded, because the formula points at one of those assets
# and a tap that references a release that does not exist yet is a broken
# `brew install` for however long the gap lasts.
#
# The tap is a SEPARATE repo, so a workflow's built-in GITHUB_TOKEN cannot
# reach it: that token is scoped to the repo it runs in. Hence TAP_TOKEN, a
# fine-grained PAT with Contents:write on the tap and nothing else.
#
# Without the token this is a warning, not a failure. The release itself still
# stands and carries the formula as an asset, so the recovery is copying one
# file — whereas failing here would mark a perfectly good published release as
# a failed run.
set -eu

VERSION="${1:?usage: push-to-tap.sh <version> [formula]}"
REPO="$(cd "$(dirname "$0")/../../.." && pwd)"
FORMULA="${2:-$REPO/dist-release/macos-steam-shim.rb}"
TAP="Superd22/homebrew-macos-steam"

log() { printf '[tap] %s\n' "$*"; }

[ -f "$FORMULA" ] || { log "no formula at $FORMULA"; exit 1; }

if [ -z "${TAP_TOKEN:-}" ]; then
    log "TAP_TOKEN not set — tap NOT updated."
    log "Copy macos-steam-shim.rb from the release assets into $TAP/Formula/,"
    log "or set the secret and re-run: see the README's Releasing section."
    exit 0
fi

work="$(mktemp -d)/tap"
git clone --quiet "https://x-access-token:${TAP_TOKEN}@github.com/${TAP}.git" "$work"
mkdir -p "$work/Formula"
cp "$FORMULA" "$work/Formula/macos-steam-shim.rb"

git -C "$work" config user.name  "github-actions[bot]"
git -C "$work" config user.email "41898282+github-actions[bot]@users.noreply.github.com"
git -C "$work" add Formula/macos-steam-shim.rb

if git -C "$work" diff --cached --quiet; then
    log "tap already at $VERSION"
else
    git -C "$work" commit --quiet -m "macos-steam-shim $VERSION"
    git -C "$work" push --quiet
    log "pushed macos-steam-shim $VERSION to $TAP"
fi
rm -rf "$(dirname "$work")"
