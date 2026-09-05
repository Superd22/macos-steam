#!/bin/sh
# Deploy half of the install (#34): lay down an already-built PAYLOAD and write
# a RECEIPT saying exactly what was laid down.
#
#     deploy.sh --payload DIR [--overlay 0|1] [--dry-run]   deploy
#     deploy.sh --verify                                    re-hash what is installed
#     deploy.sh --receipt                                   print the receipt
#     deploy.sh --rollback                                  swap back to the previous version
#     deploy.sh --uninstall [--keep-logs]                   remove it all
#
# This script knows nothing about building, and nothing about this repository.
# That is the whole point of the seam: `deploy(payload) -> receipt` has three
# callers — install.sh from a clone, the brew formula, and the launcher app —
# and only the first of them has a compiler, a checkout, or python3.
#
# It ships INSIDE the payload it deploys, so an installed machine can verify and
# uninstall itself with no repo present. When run from inside a payload it needs
# no arguments at all.
#
# --- what a deploy actually does ---------------------------------------------
#
#   1. copy the payload to  <payload root>/versions/<version>/
#   2. point   <payload root>/current  at it
#   3. write the launcher .app, whose paths all route through `current`
#   4. write  <payload root>/receipt.json
#
# The version dir plus the `current` symlink is what makes an update cheap and a
# rollback possible: every consumer path in the manifest is built on LIVE_REL,
# so an update swaps ONE symlink and never rewrites the launcher, the compat
# tool registration Steam has cached, or anything inside a bundle.
set -eu

HERE="$(cd "$(dirname "$0")" && pwd)"
PAYLOAD=""
MODE="deploy"
DRYRUN=0
OVERLAY_ARG=""
KEEP_LOGS=0

while [ $# -gt 0 ]; do
    case "$1" in
        --payload)   PAYLOAD="$2"; shift 2 ;;
        --overlay)   OVERLAY_ARG="$2"; shift 2 ;;
        --dry-run)   DRYRUN=1; shift ;;
        --verify)    MODE="verify"; shift ;;
        --receipt)   MODE="receipt"; shift ;;
        --uninstall) MODE="uninstall"; shift ;;
        --rollback)  MODE="rollback"; shift ;;
        --keep-logs) KEEP_LOGS=1; shift ;;
        -h|--help)   sed -n '2,12p' "$0"; exit 0 ;;
        *) printf 'deploy: unknown argument %s\n' "$1" >&2; exit 2 ;;
    esac
done

log() { if [ "$DRYRUN" = 1 ]; then printf '[deploy:dry] %s\n' "$*"; else printf '[deploy] %s\n' "$*"; fi; }
die() { printf '[deploy] %s\n' "$*" >&2; exit 2; }

# --- the deploy contract (#32) ------------------------------------------------
# Sourced from the payload when there is one, from the dev tree otherwise. Both
# spellings are the same generated file; which one exists tells us whether a
# repo is present, and nothing else in this script depends on that answer.
if [ -z "$PAYLOAD" ] && [ -f "$HERE/VERSION" ] && [ -f "$HERE/shim_paths.sh" ]; then
    PAYLOAD="$HERE"          # we are running from inside a deployed payload
fi
for _p in "$PAYLOAD/shim_paths.sh" "$HERE/../layout/gen/shim_paths.sh"; do
    [ -n "$_p" ] && [ -f "$_p" ] && { . "$_p"; CONTRACT="$_p"; break; }
done
for _p in "$PAYLOAD/shim_policy.sh" "$HERE/../layout/gen/shim_policy.sh"; do
    [ -n "$_p" ] && [ -f "$_p" ] && { . "$_p"; break; }
done
[ "${CONTRACT:-}" ] || die "no deploy contract found — pass --payload DIR"
unset _p

: "${HOME:?deploy: HOME is unset — there is nowhere to deploy to}"
ROOT="$HOME/$SHIM_PATH_PAYLOAD_REL"
VERSIONS="$HOME/$SHIM_PATH_VERSIONS_REL"
LIVE="$HOME/$SHIM_PATH_LIVE_REL"
RECEIPT="$HOME/$SHIM_PATH_RECEIPT_REL"
APP="$HOME/$SHIM_PATH_LAUNCHER_REL"
STEAM_OSX="$HOME/$SHIM_PATH_STEAM_OSX_REL"
LOGS="$HOME/$SHIM_PATH_LOG_DIR_REL"

# --- receipt helpers ----------------------------------------------------------
# The receipt is JSON because its readers are a shell script, a Swift app and
# whatever a release check is written in. It is written with printf rather than
# a JSON library on purpose: this script has to run on a machine with no python3
# and no repo, which is exactly the machine a brew install lands on.
json_str() {
    # Only the two characters JSON actually forbids raw. Paths here are $HOME-
    # relative manifest values plus a version string; none can contain a control
    # character, and a backslash or quote in $HOME never reaches the receipt
    # because nothing $HOME-derived is recorded.
    printf '%s' "$1" | sed -e 's|\\|\\\\|g' -e 's|"|\\"|g'
}
sha256_of() { /usr/bin/shasum -a 256 "$1" 2>/dev/null | cut -d' ' -f1; }

# What the machine actually is, recorded beside what the release was tested
# against. Diagnose reads both: "works here" and "was measured here" are
# different claims, and the gap between them is the first thing worth seeing.
observed_macos()     { /usr/bin/sw_vers -productVersion 2>/dev/null || echo unknown; }
observed_crossover() {
    /usr/bin/defaults read "$HOME/$SHIM_PATH_CX_APP_REL/Contents/Info.plist" \
        CFBundleShortVersionString 2>/dev/null || echo absent
}
observed_steam() {
    [ -x "$STEAM_OSX" ] || { echo absent; return; }
    /bin/date -r "$STEAM_OSX" -u '+%Y-%m-%dT%H:%M:%SZ' 2>/dev/null || echo unknown
}

read_receipt_field() {
    # One flat scalar out of the receipt, without a JSON parser. The receipt is
    # emitted by write_receipt below and never hand-edited, so its shape is
    # known: one "key": "value" per line.
    [ -f "$RECEIPT" ] || return 1
    sed -n 's/^  "'"$1"'": "\(.*\)",\{0,1\}$/\1/p' "$RECEIPT" | head -1
}

receipt_files() {
    # The file list, as `<sha256> <$HOME-relative path>` lines.
    [ -f "$RECEIPT" ] || return 1
    sed -n 's/^    { "sha256": "\([0-9a-f]*\)", "path": "\(.*\)" }[,]\{0,1\}$/\1 \2/p' "$RECEIPT"
}

case "$MODE" in
receipt)
    [ -f "$RECEIPT" ] || die "nothing deployed — no receipt at $RECEIPT"
    cat "$RECEIPT"; exit 0 ;;

verify)
    # The claim "it is installed and current" turned into a check anyone can
    # run. Before the receipt existed, verification was the user reading a log
    # for `patched 1 site(s)` — which says the injector ran, not that the
    # payload on disk is the payload that was shipped.
    [ -f "$RECEIPT" ] || die "nothing deployed — no receipt at $RECEIPT"
    v="$(read_receipt_field version)"
    # Via a file, not a pipe: a `while read` on the right of a pipe runs in a
    # subshell, and the tally would not survive it.
    tmp="${TMPDIR:-/tmp}/shim-verify.$$"
    receipt_files > "$tmp"
    total=0; bad=0
    while read -r want path; do
        total=$((total + 1))
        got="$(sha256_of "$HOME/$path" || true)"
        if [ -z "$got" ]; then
            printf '  MISSING  %s\n' "$path"; bad=$((bad + 1))
        elif [ "$got" != "$want" ]; then
            printf '  CHANGED  %s\n' "$path"; bad=$((bad + 1))
        fi
    done < "$tmp"
    rm -f "$tmp"
    # The symlink is not in the file list — it is the one thing whose VALUE, not
    # whose contents, is the deploy. A payload that verifies while `current`
    # points elsewhere is a live version nobody deployed.
    link="$(readlink "$LIVE" 2>/dev/null || echo '')"
    if [ "$link" != "$SHIM_PATH_VERSIONS/$v" ]; then
        printf '  CURRENT  points at %s, receipt says versions/%s\n' "${link:-nothing}" "$v"
        bad=$((bad + 1))
    fi
    if [ "$bad" != 0 ]; then
        log "$v: $total files checked, $bad problem(s) — redeploy to repair"
        exit 1
    fi
    log "$v: $total files checked, all match — deployed and intact"
    exit 0 ;;

rollback)
    # Undo an update without a network, a payload or a repo: the previous
    # version is still on disk, and it carries the script that deploys it. This
    # is the whole reason a deploy is a copy plus a symlink swap rather than an
    # in-place overwrite.
    live="$(readlink "$LIVE" 2>/dev/null || echo '')"
    prev=""
    for d in $(ls -t "$VERSIONS" 2>/dev/null); do
        [ "$SHIM_PATH_VERSIONS/$d" = "$live" ] && continue
        prev="$d"; break
    done
    [ -n "$prev" ] || die "no other version on disk to roll back to"
    log "rolling back to $prev"
    exec "$VERSIONS/$prev/deploy.sh" --payload "$VERSIONS/$prev" ;;

uninstall)
    v="$(read_receipt_field version 2>/dev/null || echo unknown)"
    rm -rf "$APP" "$ROOT"
    log "removed $APP"
    log "removed $ROOT (version $v)"
    if [ "$KEEP_LOGS" = 0 ]; then rm -rf "$LOGS"; log "removed $LOGS"; fi
    log "Steam itself was never modified — nothing else to undo."
    log "The CrossOver bottle is left alone; delete it yourself if you want it gone."
    exit 0 ;;
esac

# --- deploy -------------------------------------------------------------------
[ -n "$PAYLOAD" ] || die "no payload — pass --payload DIR (build it with src/installer/build.sh)"
[ -f "$PAYLOAD/VERSION" ] || die "$PAYLOAD is not a payload — no VERSION file"
VERSION="$(tr -d ' \n' < "$PAYLOAD/VERSION")"
# What we exec is not what Valve's installer puts on disk. Steam.app ships a
# bootstrapper; the bundle we exec is the one it unpacks under Application
# Support on its FIRST successful run. So a fresh machine that has installed
# Steam and never opened it fails this check with Steam sitting right there in
# /Applications — and "native Steam not found" sends that user off to download
# what they already have. The two states are one stat call apart and their
# remedies have nothing in common, so they get two messages.
steam_app_installed() {
    [ -d "/$SHIM_PATH_STEAM_APP_REL" ] || [ -d "$HOME/$SHIM_PATH_STEAM_APP_REL" ]
}
if [ ! -x "$STEAM_OSX" ]; then
    if steam_app_installed; then
        why="Steam is installed but has never finished its first run, so the
      bundle we exec does not exist yet. Open Steam.app, sign in ONLINE, let
      it update, then quit it and run this again."
    else
        why="The native macOS Steam client is not installed.
      Get it from https://store.steampowered.com/about/ and sign in ONLINE."
    fi
    # A real deploy refuses: writing a launcher that execs a steam_osx which is
    # not there is not a partial success, it is a broken install that looks
    # finished. A DRY RUN is the opposite case — it is a report, and "you have
    # no native Steam" is precisely the thing a report should tell you, so it
    # says so and goes on to print the rest of the plan. That is also what makes
    # the deploy contract checkable on a machine that has no Steam, like CI.
    [ "$DRYRUN" = 1 ] || die "native Steam not found at
      $STEAM_OSX
      $why"
    log "native Steam NOT found at $STEAM_OSX — a real deploy would stop here"
    log "$why"
fi

# Overlay (#21, ADR 0006). ON by default. The value is baked into the launcher
# because Steam does not forward arbitrary env to a compat tool, so the launcher
# is where it has to live. Ask the generated predicate, never the variable:
# unset means ON everywhere below this point, so `off` has to arrive as a
# literal 0 and an omitted variable would silently mean the opposite.
if [ -n "$OVERLAY_ARG" ]; then
    [ "$OVERLAY_ARG" = 0 ] && OVERLAY_ENV=0 || OVERLAY_ENV=1
elif shim_overlay_enabled; then
    OVERLAY_ENV=1
else
    OVERLAY_ENV=0
fi
[ "$OVERLAY_ENV" = 1 ] && log "overlay ON — --overlay 0 to disable" \
                       || log "overlay OFF"

# The compiled launcher (#42) reads the overlay from a stored preference at exec
# rather than from a value baked in here, which is what makes the settings pane
# able to change it without a reinstall (ADR 0011). So an explicit --overlay on
# a deploy has to write that preference, or the documented flag would quietly do
# nothing on the shipped path. Only when explicit: a plain deploy must not
# overwrite a choice the user made in the pane.
if [ -n "$OVERLAY_ARG" ] && [ -x /usr/bin/defaults ]; then
    if [ "$OVERLAY_ENV" = 1 ]; then _pref=true; else _pref=false; fi
    /usr/bin/defaults write "$SHIM_PATH_PREFS_DOMAIN" "$SHIM_PATH_PREF_OVERLAY" -bool "$_pref"
    log "wrote the launcher's overlay preference ($_pref)"
    unset _pref
fi

DEST="$VERSIONS/$VERSION"
if [ "$DRYRUN" = 1 ]; then
    log "would deploy $VERSION from $PAYLOAD"
    log "would write $DEST"
    log "would point $LIVE at versions/$VERSION"
    log "would write $APP"
    log "would write $RECEIPT"
    exit 0
fi

# Pre-versioning layouts put the artifacts directly in the payload root. They
# are not referenced by anything any more — every consumer path routes through
# `current` — so leaving them is leaving a second, stale copy of the payload
# where a confused reader will find it first.
for legacy in "$ROOT/$SHIM_PATH_ENABLER" "$ROOT/$SHIM_PATH_COMPAT_TOOLS"; do
    if [ -e "$legacy" ] && [ ! -L "$legacy" ]; then
        rm -rf "$legacy"; log "removed pre-versioning $(basename "$legacy")"
    fi
done

mkdir -p "$VERSIONS"
# Deploying a payload that IS the destination is not a mistake to reject — it is
# how a repair and a rollback are expressed, since every version dir carries the
# deploy.sh that laid it down. What would be a mistake is the copy: `rm -rf`ing
# the destination first would delete the source we are about to read.
if [ "$(cd "$PAYLOAD" && pwd -P)" = "$(cd "$DEST" 2>/dev/null && pwd -P || printf '\0')" ]; then
    log "payload is already $VERSION on disk — refreshing the symlink, launcher and receipt only"
else
    rm -rf "$DEST"
    mkdir -p "$DEST"
    # tar and not a move: the payload may be a read-only staging dir, a brew
    # cellar path, or the tree we are already running from.
    (cd "$PAYLOAD" && tar cf - .) | (cd "$DEST" && tar xf -)
fi
chmod +x "$DEST/deploy.sh" "$DEST/$SHIM_PATH_COMPAT_TOOLS/$SHIM_PATH_TOOL_NAME/$SHIM_PATH_LAUNCH_SH"
log "payload $VERSION -> $DEST"

# The swap. `ln -sfn` replaces the symlink itself rather than writing INTO the
# directory it points at, which is what a plain `ln -sf` would do here.
ln -sfn "$SHIM_PATH_VERSIONS/$VERSION" "$LIVE"
log "current -> versions/$VERSION"

# Keep exactly one previous version, so a bad update is one symlink away from
# being undone and disk use stays bounded without a policy nobody reads.
# Keep exactly one previous version, so a bad update is one symlink away from
# being undone and disk use stays bounded without a policy nobody reads. Newest
# first by mtime, which is deploy order — a version string does not sort.
ls -t "$VERSIONS" 2>/dev/null | tail -n +3 | while read -r old; do
    [ -n "$old" ] && [ "$old" != "$VERSION" ] || continue
    rm -rf "$VERSIONS/$old"; log "pruned old version $old"
done

# --- the launcher .app --------------------------------------------------------
# Vehicle A of ADR 0002: an unhardened bundle that touches zero Valve files. It
# is rewritten on every deploy, but nothing inside it names a version — every
# path it uses goes through `current` — so rewriting it is a convenience, not a
# requirement, and a rollback does not need it.
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS"
cat > "$APP/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleExecutable</key><string>$SHIM_PATH_LAUNCHER_BIN</string>
  <key>CFBundleIdentifier</key><string>$SHIM_PATH_BUNDLE_ID</string>
  <key>CFBundleName</key><string>Steam (macOS Play)</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleShortVersionString</key><string>$VERSION</string>
  <key>CFBundleVersion</key><string>$VERSION</string>
  <!-- steam_osx is universal, and the arch LaunchServices picks is inherited
       across the exec below. Without this key a Finder/open/Raycast launch
       lands on translated x86_64, where the injector's arm64 gate pattern
       matches nothing and the log reads "patched 0 site(s)". A shell exec from
       an arm64 terminal happened to pick arm64, which is why the direct path
       always looked fine. State the preference so every launch path agrees. -->
  <key>LSArchitecturePriority</key>
  <array><string>arm64</string></array>
</dict>
</plist>
PLIST

if [ -f "$PAYLOAD/$SHIM_PATH_LAUNCHER_BIN" ]; then
    # The compiled launcher (#42). It resolves the payload itself and carries
    # the preflight, the settings pane and the diagnose pane; deploy's whole job
    # is to put it in a bundle. Ad-hoc signed: locally built binaries carry no
    # quarantine bit, so Gatekeeper never interrogates it, but a signature makes
    # the bundle stable across rebuilds for LaunchServices.
    cp -f "$PAYLOAD/$SHIM_PATH_LAUNCHER_BIN" "$APP/Contents/MacOS/"

    # The settings pane, as a nested app. The option-click gesture is the fast
    # way in and the first-run screen mentions it once, but a gesture nobody
    # told you about is not discoverable — so the same binary also sits in a
    # bundle of its own, where Spotlight can find it by name. It is a COPY and
    # not a symlink: Bundle.main resolves through a symlink to the outer bundle,
    # and the pane recognises itself by its bundle id.
    HELPER="$APP/Contents/Applications/$SHIM_PATH_SETTINGS_APP"
    mkdir -p "$HELPER/Contents/MacOS"
    cp -f "$PAYLOAD/$SHIM_PATH_LAUNCHER_BIN" "$HELPER/Contents/MacOS/"
    cat > "$HELPER/Contents/Info.plist" <<HELPERPLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleExecutable</key><string>$SHIM_PATH_LAUNCHER_BIN</string>
  <key>CFBundleIdentifier</key><string>$SHIM_PATH_SETTINGS_ID</string>
  <key>CFBundleName</key><string>Steam Play Settings</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleShortVersionString</key><string>$VERSION</string>
  <key>CFBundleVersion</key><string>$VERSION</string>
  <key>LSArchitecturePriority</key>
  <array><string>arm64</string></array>
</dict>
</plist>
HELPERPLIST
    chmod +x "$HELPER/Contents/MacOS/$SHIM_PATH_LAUNCHER_BIN"
    # Inside out: signing the outer bundle seals the nested one, so a nested
    # bundle signed afterwards invalidates the seal it was sealed into.
    /usr/bin/codesign -f -s - "$HELPER" >/dev/null 2>&1 || true
    /usr/bin/codesign -f -s - "$APP" >/dev/null 2>&1 || log "ad-hoc signing skipped (no codesign)"
    log "launcher (compiled) + settings helper -> $APP"
else
    # The shell launcher: what shipped before #42, and still the fallback for a
    # payload built without a Swift toolchain.
    #
    # The body is a QUOTED heredoc: nothing in it is expanded at deploy time, so
    # the launcher resolves its own paths from $HOME when it runs. The unquoted
    # version interpolated $PAYLOAD and $STEAM_OSX — both $HOME-derived —
    # straight into a script that runs on every Steam launch, which turns a
    # quote, a backtick or a $(...) anywhere in $HOME into code executed by the
    # launcher. What IS substituted below is a manifest constant, never anything
    # derived from the user's environment.
    {
    printf '%s\n' '#!/bin/sh'
    printf '%s\n' '# Generated by deploy.sh — do not edit; redeploy instead.'
    printf '%s=%s\n' "$SHIM_ENV_OVERLAY" "$OVERLAY_ENV"
    cat <<'LAUNCHER'
# These exports are the only thing that delivers the three variables. An
# LSEnvironment dict in Info.plist would be the tidier way, but LaunchServices
# refuses to launch a bundle that has BOTH an LSEnvironment key and a #! script
# as CFBundleExecutable — open/Finder/Raycast fail with -54, surfaced as
# 'does not have permission to open "(null)"'. Exporting here works from every
# launch path, so the key is gone. SHIM_OVERLAY is always stated, 1 or 0, never
# omitted — unset means ON below this point, so omitting it cannot express
# "off". Then hand off to Valve's own binary, unmodified.
: "${HOME:?launcher: HOME is unset — cannot locate the payload or Steam}"
LAUNCHER
    printf 'shim_enabler="$HOME/%s"\n'                          "$SHIM_PATH_ENABLER_REL"
    cat <<'LAUNCHER'
# DYLD_INSERT_LIBRARIES is a ':'-separated list and we are one entry in it, not
# the owner of it (#85): prepend ours, keep whatever was already there, and add
# ourselves only once. Membership is tested on RESOLVED paths — the same dylib
# spelled relative or reached through a symlink is still the same dylib, and
# comparing literals is how the list grows without bound across nested launches.
# Measured caveat: an INHERITED value never reaches here, because /bin/sh drops
# DYLD_* on the way in. The merge is still the behaviour #42's compiled launcher
# — a Mach-O, which does inherit it — has to carry over.
shim_ours=$(/bin/realpath -q "$shim_enabler" || printf '%s' "$shim_enabler")
shim_rest="${DYLD_INSERT_LIBRARIES-}"
while [ -n "$shim_rest" ]; do
    shim_entry="${shim_rest%%:*}"
    case "$shim_rest" in *:*) shim_rest="${shim_rest#*:}" ;; *) shim_rest="" ;; esac
    [ "$(/bin/realpath -q "$shim_entry" || printf '%s' "$shim_entry")" = "$shim_ours" ] || continue
    shim_ours=""   # already listed: leave the value exactly as it was found
    break
done
[ -z "$shim_ours" ] || DYLD_INSERT_LIBRARIES="$shim_enabler${DYLD_INSERT_LIBRARIES:+:$DYLD_INSERT_LIBRARIES}"
export DYLD_INSERT_LIBRARIES
LAUNCHER
    printf 'export STEAM_EXTRA_COMPAT_TOOLS_PATHS="$HOME/%s"\n' "$SHIM_PATH_COMPAT_TOOLS_REL"
    printf 'export %s\n' "$SHIM_ENV_OVERLAY"
    printf 'exec "$HOME/%s" "$@"\n'                             "$SHIM_PATH_STEAM_OSX_REL"
    } > "$APP/Contents/MacOS/$SHIM_PATH_LAUNCHER_BIN"
    log "launcher (shell) -> $APP"
fi
chmod +x "$APP/Contents/MacOS/$SHIM_PATH_LAUNCHER_BIN"

# --- the receipt --------------------------------------------------------------
# Every deploy records what it laid down. Three readers, one file: --verify
# re-hashes it, --uninstall reads the version out of it, and the launcher's
# diagnose pane reads the compatibility block. Paths are $HOME-relative for the
# same reason the manifest holds none absolute — whose $HOME it is belongs to
# whoever is running, not to the contract.
. "$PAYLOAD/compatibility.env"
{
    printf '{\n'
    printf '  "version": "%s",\n'      "$(json_str "$VERSION")"
    printf '  "deployed_at": "%s",\n'  "$(/bin/date -u '+%Y-%m-%dT%H:%M:%SZ')"
    printf '  "live": "%s",\n'         "$(json_str "$SHIM_PATH_VERSIONS_REL/$VERSION")"
    printf '  "launcher": "%s",\n'     "$(json_str "$SHIM_PATH_LAUNCHER_REL")"
    printf '  "overlay": "%s",\n'      "$OVERLAY_ENV"
    printf '  "tested_macos": "%s",\n'      "$(json_str "$SHIM_TESTED_MACOS")"
    printf '  "tested_crossover": "%s",\n'  "$(json_str "$SHIM_TESTED_CROSSOVER")"
    printf '  "tested_steam": "%s",\n'      "$(json_str "$SHIM_TESTED_STEAM")"
    printf '  "tested_titles": "%s",\n'     "$(json_str "$SHIM_TESTED_TITLES")"
    printf '  "observed_macos": "%s",\n'     "$(json_str "$(observed_macos)")"
    printf '  "observed_crossover": "%s",\n' "$(json_str "$(observed_crossover)")"
    printf '  "observed_steam": "%s",\n'     "$(json_str "$(observed_steam)")"
    printf '  "files": [\n'
    # Every regular file under the live version, plus the two the launcher
    # bundle owns. Emitted sorted so two deploys of one version produce byte-
    # identical receipts, which is what makes a diff of them meaningful.
    {
        (cd "$DEST" && find . -type f | sed 's|^\./||' | sort | \
            while read -r f; do printf '%s\t%s\n' "$SHIM_PATH_VERSIONS_REL/$VERSION/$f" "$DEST/$f"; done)
        printf '%s\t%s\n' "$SHIM_PATH_LAUNCHER_REL/Contents/Info.plist" "$APP/Contents/Info.plist"
        printf '%s\t%s\n' "$SHIM_PATH_LAUNCHER_REL/Contents/MacOS/$SHIM_PATH_LAUNCHER_BIN" "$APP/Contents/MacOS/$SHIM_PATH_LAUNCHER_BIN"
        if [ -d "$APP/Contents/Applications/$SHIM_PATH_SETTINGS_APP" ]; then
            for f in Contents/Info.plist "Contents/MacOS/$SHIM_PATH_LAUNCHER_BIN"; do
                printf '%s\t%s\n' "$SHIM_PATH_LAUNCHER_REL/Contents/Applications/$SHIM_PATH_SETTINGS_APP/$f" \
                                    "$APP/Contents/Applications/$SHIM_PATH_SETTINGS_APP/$f"
            done
        fi
    } | {
        first=1
        while IFS="$(printf '\t')" read -r rel abs; do
            [ "$first" = 1 ] || printf ',\n'
            first=0
            printf '    { "sha256": "%s", "path": "%s" }' "$(sha256_of "$abs")" "$(json_str "$rel")"
        done
        printf '\n'
    }
    printf '  ]\n'
    printf '}\n'
} > "$RECEIPT"
log "receipt -> $RECEIPT ($(receipt_files | wc -l | tr -d ' ') files)"

log "done. Quit Steam, then launch 'Steam (macOS Play)' so the injector and the"
log "tool path are inherited. \`deploy.sh --verify\` checks the deploy itself;"
log "~/$SHIM_PATH_LOG_ENABLER_REL should say 'patched 1 site(s)'."
