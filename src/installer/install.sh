#!/bin/sh
# From-source install — the FIRST adapter over the build/deploy seam (#34).
#
#   ./src/installer/install.sh              build, then deploy
#   ./src/installer/install.sh --uninstall  remove payload + launcher + logs
#   ./src/installer/install.sh --verify     re-hash what is installed
#   ./src/installer/install.sh --dry-run    say what a deploy would do
#
# Everything this script used to do itself now lives on one side of the seam or
# the other:
#
#   build.sh   repo -> payload    needs clang, mingw-w64, python3, a checkout
#   deploy.sh  payload -> receipt needs none of them
#
# The split is what makes a distributable possible (ADR 0002): the brew formula
# is the second adapter and the launcher app the third, and neither has a
# compiler. Keeping this script means the clone path — the one a contributor
# uses twenty times a day — is still one command.
#
# Any other argument is passed straight through to deploy.sh, so
# `--overlay 0` and friends work here too.
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"

case "${1:-}" in
    --uninstall|--verify|--receipt)
        # Nothing to build for these: they read the receipt of whatever is
        # already installed. Prefer the DEPLOYED copy of deploy.sh — it is the
        # one that was actually used, and it exists even when this checkout has
        # moved on.
        [ -f "$HERE/../layout/gen/shim_paths.sh" ] || "$HERE/../layout/build.sh" >/dev/null
        . "$HERE/../layout/gen/shim_paths.sh"
        deployed="$HOME/$SHIM_PATH_LIVE_REL/deploy.sh"
        if [ -x "$deployed" ]; then exec "$deployed" "$@"; fi
        exec "$HERE/deploy.sh" "$@" ;;
esac

PAYLOAD="$("$HERE/build.sh" | tail -1)"
exec "$HERE/deploy.sh" --payload "$PAYLOAD" "$@"
