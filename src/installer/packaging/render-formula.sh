#!/bin/sh
# Render the brew formula from its template (#42).
#
#   src/installer/packaging/render-formula.sh <version> <url> <sha256> [outfile]
#
# The same substitution shape build.sh uses for the two vdfs, and for the same
# reason: three facts (the version, the tarball it names, and that tarball's
# hash) must agree, so they are filled in once from what the release actually
# produced rather than typed into a formula by hand.
#
# Release CI calls this with the checksum of the tarball it just uploaded. A
# human never edits the rendered .rb — edit the .in.
set -eu
[ $# -ge 3 ] || { sed -n '2,4p' "$0" >&2; exit 2; }
VERSION="$1"; URL="$2"; SHA256="$3"; OUT="${4:--}"
HERE="$(cd "$(dirname "$0")" && pwd)"

render() {
    sed -e "s|@VERSION@|$VERSION|g" \
        -e "s|@URL@|$URL|g" \
        -e "s|@SHA256@|$SHA256|g" "$HERE/macos-steam-shim.rb.in"
}

if [ "$OUT" = "-" ]; then render; else render > "$OUT"; echo "$OUT"; fi
