# overlay-probe — is Valve's macOS overlay reachable from a process we control?

Harnesses for #22 (and whatever re-tests #21 needs). Each presents a real
`CAMetalLayer` through `nextDrawable → presentDrawable: → commit`, which is the
cycle `gameoverlayrenderer.dylib` swizzles.

```sh
./build.sh                      # ARCH=x86_64 ./build.sh to match the bottle
R="$HOME/Library/Application Support/Steam/Steam.AppBundle/Steam/Contents/MacOS/gameoverlayrenderer.dylib"

# A — renderer present at launch. Overlay WORKS (#22).
env -u SteamNoOverlayUIDrawing DYLD_INSERT_LIBRARIES="$R" \
    SteamAppId=3215050 SteamGameId=3215050 SteamOverlayGameId=3215050 ./metalprobe

# B — renderer arrives by dlopen, interposes recovered by GOT rebinding.
#     Overlay does NOT appear (#22). SKIP_RUNLOOP=1 avoids stalling the pump;
#     NO_REBIND=1 is the control.
env -u SteamNoOverlayUIDrawing SKIP_RUNLOOP=1 OVERLAY_DLOPEN="$R" \
    SteamAppId=3215050 SteamGameId=3215050 SteamOverlayGameId=3215050 ./metalprobe3
```

Then press **Shift+Tab**. Steam must be running and `SteamNoOverlayUIDrawing`
must be unset — the renderer bails on it explicitly.

## What these established

- **The client arms the overlay for a process it has no relationship with.** A is a
  plain unsigned Metal binary, not launched by Steam and never calling Steamworks;
  with `SteamOverlayGameId` set to a real appid the client starts `gameoverlayui`
  and composites over its layer. This retired #21's biggest unknown.
- **Load order is the variable, not the interposes.** A and B differ only in when
  the renderer loads. `metalprobe3` parses `__DATA,__interpose` and rebinds all 15
  entries via fishhook — verified 15/15 identified — and the overlay still does not
  appear, so `dlopen` loses something other than interposition.
- `CFRunLoopRun`/`CFRunLoopRunInMode` cannot be rebound usefully here: Valve's
  replacements do not pump a run loop they did not set up, so the harness stalls.

`fishhook` is Facebook's, BSD-licensed, vendored unmodified; `fishhook.h` is
reconstructed from its API.
