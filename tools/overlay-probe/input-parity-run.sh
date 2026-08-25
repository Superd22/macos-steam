#!/bin/sh
# input-parity-run.sh — the #28 measurement, driven end to end.
#
# #28 asks what happens to a title's input while the Steam overlay is up. The
# parts that can be synthesised from the mac side (keyboard, mouse motion) are
# already automatable; a gamepad is not, and neither is an honest mouse click.
# So this script runs inputprobe as a live overlay target and walks an operator
# through a fixed sequence, stamping the probe's log at every phase boundary.
#
# The point of the fixed sequence is the CONTROL. "The pad did nothing while
# the overlay was up" means nothing on its own — it is indistinguishable from a
# pad that was never plugged in. Every phase with the overlay UP is therefore
# bracketed by the identical phase with it DOWN, and the verdict is the diff.
#
#   ./input-parity-run.sh            64-bit probe (default)
#   BITS=32 ./input-parity-run.sh    32-bit probe, the Among Us bitness
#
# Reads out at the end. Full log: <bottle>/drive_c/inputprobe.log
set -eu

HERE="$(cd "$(dirname "$0")" && pwd)"
BITS="${BITS:-64}"
BOTTLE_NAME="${SHIM_BOTTLE:-steam-shim}"
B="$HOME/Library/Application Support/CrossOver/Bottles/$BOTTLE_NAME"
CX="${CX_APP:-$HOME/Applications/CrossOver.app}/Contents/SharedSupport/CrossOver"
APPID="${SHIM_APPID:-945360}"
EXE="inputprobe${BITS}.exe"
LOG="$B/drive_c/inputprobe.log"
MARK="$B/drive_c/inputprobe.mark"

[ -d "$B" ] || { echo "bottle '$BOTTLE_NAME' not found at $B" >&2; exit 2; }
[ -f "$HERE/$EXE" ] || { echo "$EXE not built — run build.sh" >&2; exit 2; }
if ! /usr/bin/pgrep -qx steam_osx; then
    echo "native macOS Steam is not running — the client is what arms the overlay." >&2
    exit 2
fi

cp -f "$HERE/$EXE" "$B/drive_c/"
rm -f "$LOG" "$MARK"

echo "--- starting $EXE in bottle '$BOTTLE_NAME' (appid $APPID) ---"
( cd "$B/drive_c" && env -u SteamNoOverlayUIDrawing \
    WINEPREFIX="$B" CX_ROOT="$CX" CX_BOTTLE="$BOTTLE_NAME" \
    WINEDLLPATH="$CX/lib/wine/x86_64-windows:$CX/lib/wine/i386-windows:$B/drive_c/shim" \
    SHIM_OVERLAY=1 STEAM_OVERLAY_LOGGING=1 STEAM_OVERLAY_LOGGING_FLUSH=1 \
    SteamAppId="$APPID" SteamGameId="$APPID" SteamOverlayGameId="$APPID" \
    "$CX/CrossOver-Hosted Application/wineloader" "c:\\$EXE" ) >/tmp/inputprobe-run.log 2>&1 &

# Wait for the probe to be ready rather than sleeping a guess: a slow D3D device
# creation would otherwise put the first phase before the window exists.
n=0
while [ $n -lt 60 ]; do
    grep -q -- '--- ready' "$LOG" 2>/dev/null && break
    sleep 1; n=$((n + 1))
done
grep -q -- '--- ready' "$LOG" 2>/dev/null || { echo "probe never became ready; see $LOG" >&2; exit 3; }
echo "probe ready. Its window is 'inputprobe — #28 input parity'."
echo

mark() { printf '%s' "$1" > "$MARK"; sleep 1; }

phase() {   # phase <mark> <human instruction>
    mark "$1"
    echo "================================================================"
    echo "  $2"
    echo "================================================================"
    printf '  press RETURN when done > '
    read -r _ </dev/tty
    echo
}

cat <<'INTRO'
Click the probe window once to give it focus, then follow each step. Do each
one for a few seconds — the log records edges, so a brief wiggle is enough.

INTRO
printf 'press RETURN to begin > '; read -r _ </dev/tty; echo

phase "down A-CONTROL-overlay-DOWN" \
  "A. CONTROL, overlay CLOSED.
     - type some letters
     - move the mouse over the window, and CLICK in it
     - move both thumbsticks, press A/B/X/Y, pull both triggers, press the dpad"

phase "up B-overlay-UP-open-it-now" \
  "B. Press Shift+Tab NOW to OPEN the overlay, then with it open:
     - type into the overlay's search/chat box
     - move the mouse and click on something in the overlay
     - move both thumbsticks, press A/B/X/Y, pull both triggers, press the dpad
     (this is the phase that answers the ticket)"

phase "down C-overlay-CLOSED-again" \
  "C. Press Shift+Tab to CLOSE the overlay, then repeat A exactly:
     - type some letters
     - move the mouse and click in the window
     - move both thumbsticks, press A/B/X/Y, both triggers, the dpad
     (this is what proves input comes BACK cleanly)"

mark "END"
sleep 2

echo
echo "================= READOUT ================="
echo
for p in A B C; do
    case $p in
      A) from="A-CONTROL"; label="A  overlay DOWN (control)";;
      B) from="B-overlay-UP"; label="B  overlay UP";;
      C) from="C-overlay-CLOSED"; label="C  overlay DOWN again";;
    esac
    seg=$(awk -v s="$from" '
        index($0, "=== MARK: ") && index($0, s) { on=1; next }
        index($0, "=== MARK: ") && on { on=0 }
        on { print }' "$LOG")
    kb=$(printf '%s\n' "$seg" | grep -c 'KEY   down' || true)
    ms=$(printf '%s\n' "$seg" | grep -c 'MOUSE ' || true)
    xi=$(printf '%s\n' "$seg" | grep -c 'XINPUT ' || true)
    di=$(printf '%s\n' "$seg" | grep -c 'DINPUT L=' || true)
    printf '%-26s  keys=%-4s mouse=%-4s xinput=%-4s dinput=%-4s\n' "$label" "$kb" "$ms" "$xi" "$di"
done
echo
echo "Read it as: a channel is GATED if B is 0 while A and C are not."
echo "A channel with B > 0 LEAKS — the title kept receiving it under the overlay."
echo "A channel that is 0 in A as well was never exercised; that row proves nothing."
echo
echo "full log: $LOG"
echo "renderer: /tmp/gameoverlayrenderer.<pid>.log  (grep Hooking / 'Enabling overlay')"
