# overlay-probe — the two overlay claims that get re-measured

Both probes here are PEs that run **inside the bottle**, because that is the only place the
overlay claims mean anything. Rerun them after a CrossOver or Steam client bump.

```sh
./build.sh        # inputprobe32/64.exe + d3dprobe.exe (needs mingw)
```

The Metal-side harnesses that first established the overlay is reachable at all
(`metalprobe`, `metalprobe5`, `u32probe`) are archived in `attic/overlay-probe/` — that
question is closed and ADR 0003 holds its answer.

## `d3dprobe` — S-4: does the overlay see D3DMetal's frames? (#26)

`metalprobe` proves the Metal path; a real title renders through Direct3D. `d3dprobe.c` is a
Windows D3D11 program (real device, real swap chain, real `Present`) run **inside the bottle**,
which pulls the renderer in from the top of `WinMain` — before it makes any USER call, which is
early enough because `winemac.so` is demand-loaded.

```sh
x86_64-w64-mingw32-gcc -mwindows -o d3dprobe.exe d3dprobe.c -luuid
B="$HOME/Library/Application Support/CrossOver/Bottles/steam-shim"
cp d3dprobe.exe "$B/drive_c/"          # needs C:\shim from src/shim (both halves)

CX="$HOME/Applications/CrossOver.app/Contents/SharedSupport/CrossOver"
cd "$B/drive_c" && env -u SteamNoOverlayUIDrawing \
  WINEPREFIX="$B" CX_ROOT="$CX" CX_BOTTLE=steam-shim \
  WINEDLLPATH="$CX/lib/wine/x86_64-windows:$B/drive_c/shim" \
  SHIM_OVERLAY=1 STEAM_OVERLAY_LOGGING=1 STEAM_OVERLAY_LOGGING_FLUSH=1 \
  SteamAppId=945360 SteamGameId=945360 SteamOverlayGameId=945360 \
  "$CX/CrossOver-Hosted Application/wineloader" c:\\d3dprobe.exe
```

It answers yes: 5/5 hooks, and `ValveGetScreenSize( 640, 480 )` / `ValveGetOutputBounds` show the
renderer tracking the **D3D window's** drawable. Detail in `docs/research/overlay-injection.md` §5.

### `SHIM_LATE_PULL=1` — the same probe in a DRM-wrapped title's timing (#112)

The run above is the *early* question. `SHIM_LATE_PULL=1` defers the shim `LoadLibrary` until after
the window and the D3D device exist, which puts the probe in the state a wrapped title's process is
in when our unixlib arrives on the DRM route: `winemac` up, `NSApplication` already init'd, so
Valve's `-[NSApplication init]` swizzle can never fire again. It is the cheap fixture for the late
arming — no title, no DRM bottle-arming, one `grep`:

```
                                    winemac at attach   Hooking
SHIM_LATE_PULL=1, unixlib at main            present         0     <- what shipped before #112
SHIM_LATE_PULL=1, unixlib with arming        present         5
SHIM_LATE_PULL=1, SHIM_OVERLAY=0             (no renderer log at all)
(unset), unixlib with arming                  absent         5     <- early path, unregressed
```

Measured 2026-09-05, CrossOver 26.3.0.39832. The third row is the control that matters and the
fourth is the one that must never move: the arming is gated on `NSApp` being non-nil, so on the
injector path it declines and says so (`overlay: NSApp not up yet` in `shim-unix.log`).

**Two traps this probe fell into first, both relevant to #25's injector:** a console exe loses the
race before `main` (the console attach reaches USER, which demand-loads `winemac.so`), and a static
`d3d11` import runs its `DllMain` before `main`. Hence no console and a hand-`LoadLibrary`d d3d11.

## `inputprobe` — what happens to a title's input while the overlay is up? (#28)

A real D3D11 window that also listens on keyboard, mouse, XInput and DirectInput, and logs
every edge. `input-parity-run.sh` drives it end to end: it walks an operator through a fixed
three-phase sequence — overlay DOWN, overlay UP, overlay DOWN again — stamping the log at
each boundary, and reads out the per-channel counts.

```sh
./input-parity-run.sh            # 64-bit probe (default)
BITS=32 ./input-parity-run.sh    # 32-bit probe, the Among Us bitness
```

The bracketing phases are the point. "The pad did nothing while the overlay was up" is
indistinguishable from a pad that was never plugged in; a channel is GATED only if B is 0
while A and C are not, and a channel that is 0 in A proves nothing.

## Read the log, always

`STEAM_OVERLAY_LOGGING` is a `getenv` in the dylib (`0x1e194`), so the log is
opt-in. #22 concluded "(a2) is dead" from runs where it was unset and no log was
written, which is why the failure looked mechanism-less. With it on, the failing
runs stop after `Modules at GameOverlayRenderer.dll attach` and the succeeding
ones continue into `Hooking …` — the whole finding above is one grep on that file.
