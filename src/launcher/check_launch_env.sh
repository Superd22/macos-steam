#!/bin/sh
# Acceptance test for what the launcher hands to Steam (#85, carried into #42).
#
# The bug this guards against is specific: the launcher used to ASSIGN
# DYLD_INSERT_LIBRARIES rather than merge into it, dropping whatever the user or
# another tool had inserted. It was invisible in the shell launcher because
# /bin/sh drops DYLD_* on the way in, so there was never anything to clobber. A
# Mach-O inherits it, so the bug became live the moment this app landed.
#
# The stated acceptance test is: launched with a pre-existing
# DYLD_INSERT_LIBRARIES, the launcher hands steam_osx both entries with ours
# first, and launching twice does not duplicate ours. That is asked here without
# launching Steam at all, via --print-env, which prints exactly the three
# variables the exec would carry.
set -eu
cd "$(dirname "$0")"
. ../layout/gen/shim_paths.sh
# The switch's env var name is policy, not a string: name it through the
# manifest here exactly as every other reader does (#33).
. ../layout/gen/shim_policy.sh

BIN="$SHIM_PATH_DIST/$SHIM_PATH_LAUNCHER_BIN"
[ -x "$BIN" ] || { echo "launcher: no binary at $BIN — run build.sh first" >&2; exit 2; }

OURS="$HOME/$SHIM_PATH_ENABLER_REL"
[ -f "$OURS" ] || { echo "launcher: nothing deployed at $OURS; skipping (deploy first)" >&2; exit 0; }

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
# A real, loadable dylib to stand in for "something else was already inserted":
# dyld kills any process whose insertion cannot be loaded, this one included, so
# the stand-in cannot be a made-up path. Our own injector no-ops outside
# steam_osx, which makes it the safe choice.
OTHER="$TMP/other.dylib"
cp "$OURS" "$OTHER"
LINK="$TMP/link-to-ours.dylib"
ln -s "$OURS" "$LINK"

fail=0
dyld_of() { env "$@" "./$BIN" $SHIM_PATH_PRINT_ENV_FLAG | sed -n 's/^DYLD_INSERT_LIBRARIES=//p'; }
expect() {
    what="$1"; got="$2"; want="$3"
    if [ "$got" = "$want" ]; then
        printf '  ok   %s\n' "$what"
    else
        printf '  FAIL %s\n       got  %s\n       want %s\n' "$what" "$got" "$want"; fail=1
    fi
}

echo "launcher: DYLD_INSERT_LIBRARIES merge"
expect "nothing inherited: ours alone" \
       "$(dyld_of DYLD_INSERT_LIBRARIES=)" "$OURS"
expect "another entry inherited: both, ours first" \
       "$(dyld_of DYLD_INSERT_LIBRARIES="$OTHER")" "$OURS:$OTHER"
expect "ours already listed: not added twice" \
       "$(dyld_of DYLD_INSERT_LIBRARIES="$OURS")" "$OURS"
expect "ours listed among others: order and count unchanged" \
       "$(dyld_of DYLD_INSERT_LIBRARIES="$OTHER:$OURS")" "$OTHER:$OURS"
# The nested-launch case, and the reason membership is tested on resolved paths:
# a symlinked or relative spelling is the SAME dylib, and comparing literals is
# how the list grows without bound across launches.
expect "ours reached through a symlink: recognised, left as found" \
       "$(dyld_of DYLD_INSERT_LIBRARIES="$LINK")" "$LINK"

# The overlay is always STATED, never omitted: unset means ON below this point,
# so an absent variable cannot express "off" (ADR 0006).
echo "launcher: overlay is stated"
stated="$(env "$SHIM_ENV_OVERLAY=0" "./$BIN" $SHIM_PATH_PRINT_ENV_FLAG \
          | sed -n "s/^$SHIM_ENV_OVERLAY=//p")"
stored="$(defaults read "$SHIM_PATH_PREFS_DOMAIN" overlay 2>/dev/null || echo unset)"
case "$stored" in
    unset) expect "no stored preference: the environment's explicit 0 is honoured" "$stated" "0" ;;
    0)     expect "stored preference off: honoured over the environment" "$stated" "0" ;;
    *)     expect "stored preference on: honoured over the environment" "$stated" "1" ;;
esac

[ "$fail" = 0 ] || exit 1
echo "launcher: launch environment ok"
