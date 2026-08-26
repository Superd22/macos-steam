#!/bin/sh
# Render the brew formula from its template (#42).
#
#   src/installer/packaging/render-formula.sh <version> <url> <sha256> [outfile]
#
# The same substitution shape build.sh uses for the two vdfs, and for the same
# reason: facts that must agree are filled in once, from what the release
# actually produced, rather than typed into a formula by hand.
#
# Two classes of substitution happen here:
#
#   the release facts   @VERSION@ @URL@ @SHA256@ — from the arguments, i.e.
#                       from the tarball CI just built and uploaded.
#   the deploy contract @PAYLOAD_REL@ @LAUNCHER_REL@ — from src/layout, because
#                       the formula's caveats tell a user where their payload
#                       and launcher went, and those are manifest-owned paths
#                       (#32). The drift guard rejects the template if it spells
#                       them out, and it is right to: a formula that names a
#                       stale path is a user looking in the wrong directory.
#
# A human never edits the rendered .rb — edit the .in.
set -eu
[ $# -ge 3 ] || { sed -n '3,4p' "$0" >&2; exit 2; }
VERSION="$1"; URL="$2"; SHA256="$3"; OUT="${4:--}"
HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../../.." && pwd)"

# The contract, regenerated if this is a fresh checkout that has not built yet
# (the formula-lint job does not build). Same call every other build.sh makes.
[ -f "$REPO/src/layout/gen/shim_paths.sh" ] || "$REPO/src/layout/build.sh" >/dev/null
. "$REPO/src/layout/gen/shim_paths.sh"

render() {
    sed -e "s|@VERSION@|$VERSION|g" \
        -e "s|@URL@|$URL|g" \
        -e "s|@SHA256@|$SHA256|g" \
        -e "s|@PAYLOAD_REL@|$SHIM_PATH_PAYLOAD_REL|g" \
        -e "s|@LAUNCHER_REL@|$SHIM_PATH_LAUNCHER_REL|g" "$HERE/macos-steam-shim.rb.in"
}

if [ "$OUT" = "-" ]; then render; else render > "$OUT"; echo "$OUT"; fi
