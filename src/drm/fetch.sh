#!/bin/sh
# Fetch Valve's signed Windows steamclient64.dll from Valve (ADR 0014).
#
# The DRM route needs the genuine article on disk, because that is what the
# wrapper reads and checks the signature of. macOS Steam does not ship one --
# `legacycompat/`, which is where Proton gets it, is Linux-only. So we take it
# from Valve's own public client manifest: the same unauthenticated endpoint
# steamcmd bootstraps from. No account, no depot protocol, no redistribution by
# us -- the bytes come from Valve, to the user, over TLS, and are checked
# against the SHA-256 Valve publishes for them.
#
# Idempotent: re-running with the cache already at the published SHA-256 does
# nothing. Safe to call on every launch, though the launch script does not.
#
#   ./fetch.sh [--force]
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
# Beside us in the deployed payload, two levels up in the dev tree (ADR 0005).
for _p in "$HERE/shim_paths.sh" "$HERE/../layout/gen/shim_paths.sh"; do
    [ -f "$_p" ] && { . "$_p"; break; }
done
[ -n "${SHIM_PATH_CLIENT_MANIFEST_URL:-}" ] || {
    printf 'drm-fetch: shim_paths.sh not found beside %s or in ../layout/gen\n' "$HERE" >&2
    exit 2
}

CACHE="$HOME/$SHIM_PATH_CLIENT_CACHE_REL"
STAMP="$CACHE/.manifest-sha256"
FORCE=0
[ "${1:-}" = "--force" ] && FORCE=1

say() { printf 'drm-fetch: %s\n' "$*" >&2; }

# The manifest is a few KB of VDF. Take the package's own file name and the
# SHA-256 Valve publishes beside it -- the FIRST sha2 in the block, which is the
# plain zip's; the later sha2vz belongs to the LZMA-compressed variant we do not
# use (decompressing it would mean shipping an LZMA implementation).
TMP="${TMPDIR:-/tmp}"
mani="$(mktemp "$TMP/drm-manifest.XXXXXX")" || exit 1
trap 'rm -f "$mani"' EXIT INT TERM
say "reading Valve's client manifest"
curl -fsSL --max-time 60 -o "$mani" "$SHIM_PATH_CLIENT_MANIFEST_URL" || {
    say "could not fetch $SHIM_PATH_CLIENT_MANIFEST_URL"; exit 3; }

eval "$(awk -v pkg="$SHIM_PATH_CLIENT_PKG" '
    $0 ~ "^\t\"" pkg "\"$" { inpkg = 1; next }
    inpkg && /^\t\}/       { exit }
    inpkg && /"file"/      { if (!f) { gsub(/.*"file"[ \t]+"/, ""); gsub(/".*/, ""); f = $0 } }
    inpkg && /"sha2"/      { if (!s) { gsub(/.*"sha2"[ \t]+"/, ""); gsub(/".*/, ""); s = $0 } }
    END { printf "PKG_FILE=%s\nPKG_SHA=%s\n", f, s }
' "$mani")"

[ -n "${PKG_FILE:-}" ] && [ -n "${PKG_SHA:-}" ] || {
    say "manifest has no $SHIM_PATH_CLIENT_PKG package -- Valve changed its layout"; exit 3; }

# All three, not just the one that ships: build.sh --regen reads the other two as
# references, and a cache pruned down to the shipped file would make this say
# "nothing to do" and the regeneration fail with a traceback.
if [ "$FORCE" = 0 ] && [ -f "$STAMP" ] && [ "$(cat "$STAMP")" = "$PKG_SHA" ] \
   && [ -f "$CACHE/$SHIM_PATH_PE64" ] \
   && [ -f "$CACHE/$SHIM_PATH_SHADOW_TIER0" ] \
   && [ -f "$CACHE/$SHIM_PATH_SHADOW_VSTDLIB" ]; then
    say "already at $PKG_SHA -- nothing to do"
    exit 0
fi

# Same directory as the manifest: Valve serves the packages beside it.
base="${SHIM_PATH_CLIENT_MANIFEST_URL%/*}"
zip="$(mktemp "$TMP/drm-package.XXXXXX")" || exit 1
trap 'rm -f "$mani" "$zip"' EXIT INT TERM
say "downloading $PKG_FILE (this is ~60 MB, once per Steam client update)"
curl -fsSL --max-time 900 -o "$zip" "$base/$PKG_FILE" || { say "download failed"; exit 3; }

got="$(shasum -a 256 "$zip" | cut -d' ' -f1)"
[ "$got" = "$PKG_SHA" ] || { say "SHA-256 mismatch: got $got, Valve published $PKG_SHA"; exit 4; }
say "SHA-256 verified against Valve's manifest"

# steamclient64.dll is the one that ships. The other two are references for
# regenerating the shadows' export lists (build.sh --regen); they are NOT
# deployed -- ours replace them.
mkdir -p "$CACHE"
tmpd="$(mktemp -d "$TMP/drm-extract.XXXXXX")" || exit 1
trap 'rm -f "$mani" "$zip"; rm -rf "$tmpd"' EXIT INT TERM
unzip -o -q -j "$zip" "$SHIM_PATH_PE64" "$SHIM_PATH_SHADOW_TIER0" "$SHIM_PATH_SHADOW_VSTDLIB" \
      -d "$tmpd" || { say "package does not contain the expected DLLs"; exit 3; }

# The one property that makes the whole route work: the file must carry Valve's
# signature block. If it does not, we fetched the wrong thing and the DRM stub
# would reject it -- fail here, where the reason is legible.
if ! head -c 68 "$tmpd/$SHIM_PATH_PE64" | tail -c 4 | grep -q 'VLV'; then
    say "$SHIM_PATH_PE64 carries no VLV signature block -- refusing to install it"
    exit 4
fi

for f in "$SHIM_PATH_PE64" "$SHIM_PATH_SHADOW_TIER0" "$SHIM_PATH_SHADOW_VSTDLIB"; do
    mv -f "$tmpd/$f" "$CACHE/$f"
done
printf '%s\n' "$PKG_SHA" > "$STAMP"
say "installed into $CACHE"

# The one check that has to happen HERE. Every name this DLL imports from a
# shadowed library must exist as an export on ours, or the loader refuses to
# bind it -- and refuses silently: the title dies with the same DRM dialog as if
# none of this had ever been built. Only at fetch time do we have the client
# build the user will actually run, so a green build on some other machine says
# nothing. Best-effort: a payload without python3 skips it rather than blocking
# an install that is probably fine.
CHECK="$HERE/check_shadow.py"
[ -f "$CHECK" ] || CHECK="$HERE/../drm/check_shadow.py"
SHADOWS="$HERE/$SHIM_PATH_DIST"
[ -f "$SHADOWS/$SHIM_PATH_SHADOW_TIER0" ] || SHADOWS="$HERE/../drm/$SHIM_PATH_DIST"
if [ -f "$CHECK" ] && [ -f "$SHADOWS/$SHIM_PATH_SHADOW_TIER0" ] && command -v python3 >/dev/null 2>&1; then
    if ! python3 "$CHECK" "$CACHE/$SHIM_PATH_PE64" \
            "$SHIM_PATH_SHADOW_TIER0"   "$SHADOWS/$SHIM_PATH_SHADOW_TIER0" \
            "$SHIM_PATH_SHADOW_VSTDLIB" "$SHADOWS/$SHIM_PATH_SHADOW_VSTDLIB"; then
        say ""
        say "This Steam client build needs names our shadows do not export, so a"
        say "DRM-wrapped title would fail to start with no explanation. Update the"
        say "tool; the files above are installed and will work once it catches up."
        exit 5
    fi
else
    say "note: coverage against this client build not checked (no python3 or no shadows)"
fi

say "DRM-wrapped titles are now available; non-DRM titles are unaffected"
