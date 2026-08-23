# overlay-probe — is Valve's macOS overlay reachable from a process we control?

Harnesses for #22 / #21. Each presents a real `CAMetalLayer` through
`nextDrawable → presentDrawable: → commit`, which is the cycle
`gameoverlayrenderer.dylib` swizzles.

```sh
./build.sh                      # ARCH=x86_64 ./build.sh to match the bottle
R="$HOME/Library/Application Support/Steam/Steam.AppBundle/Steam/Contents/MacOS/gameoverlayrenderer.dylib"

# ALWAYS set these — the renderer's log is opt-in, and without it a failure is mute.
LOG="STEAM_OVERLAY_LOGGING=1 STEAM_OVERLAY_LOGGING_FLUSH=1"   # -> /tmp/gameoverlayrenderer.<pid>.log

# A — renderer present at launch. Overlay works.
env -u SteamNoOverlayUIDrawing $LOG DYLD_INSERT_LIBRARIES="$R" \
    SteamAppId=3215050 SteamGameId=3215050 SteamOverlayGameId=3215050 ./metalprobe

# C — renderer arrives by dlopen, BEFORE [NSApplication sharedApplication].
#     Overlay works. No insertion, no interpose recovery, no entitlement.
env -u SteamNoOverlayUIDrawing $LOG DLOPEN_WHEN=ctor OVERLAY_DLOPEN="$R" \
    SteamAppId=3215050 SteamGameId=3215050 SteamOverlayGameId=3215050 ./metalprobe5

# B — the same dlopen, after the app is up. Nothing. (metalprobe3's timing.)
env -u SteamNoOverlayUIDrawing $LOG DLOPEN_WHEN=late OVERLAY_DLOPEN="$R" \
    SteamAppId=3215050 SteamGameId=3215050 SteamOverlayGameId=3215050 ./metalprobe5
```

Then press **Shift+Tab**. Steam must be running and `SteamNoOverlayUIDrawing`
must be unset — the renderer bails on it explicitly.

## What these established

- **The client arms the overlay for a process it has no relationship with.** A is a
  plain unsigned Metal binary, not launched by Steam and never calling Steamworks;
  with `SteamOverlayGameId` set to a real appid the client starts `gameoverlayui`
  and composites over its layer. This retired #21's biggest unknown.
- **`dlopen` works — the variable is load *time*, not insertion.** `metalprobe5`
  moves the `dlopen` across the app's startup and bisects the boundary:

  | `DLOPEN_WHEN` | dlopen happens | `Hooking` lines | overlay |
  |---|---|---|---|
  | `ctor`   | constructor, before `main` | 5/5 | ✅ |
  | `main`   | top of `main`              | 5/5 | ✅ |
  | `nsapp`  | after `[NSApplication sharedApplication]` | 0 | ❌ |
  | `device` | after `MTLCreateSystemDefaultDevice()`    | 0 | ❌ |
  | `layer`  | after the `CAMetalLayer` is attached      | 0 | ❌ |
  | `late`   | after the window is on screen             | 0 | ❌ |

  **The gate is `NSApplication` instantiation.** Load before it and the renderer
  hooks the five `MTLCommandBuffer` selectors and arms; load after it and attach
  runs, prints its module list, and installs nothing.
- **The 15 interposes are not needed.** `metalprobe5` does no GOT rebinding at all
  — no `fishhook`, no `__DATA,__interpose` parsing — and the overlay still draws.
  (`metalprobe3` kept the recovery code; it is what proved the interposes were not
  the gate, and it stays useful for #21's inserted-stub idea.)

## Read the log, always

`STEAM_OVERLAY_LOGGING` is a `getenv` in the dylib (`0x1e194`), so the log is
opt-in. #22 concluded "(a2) is dead" from runs where it was unset and no log was
written, which is why the failure looked mechanism-less. With it on, the failing
runs stop after `Modules at GameOverlayRenderer.dll attach` and the succeeding
ones continue into `Hooking …` — the whole finding above is one grep on that file.

`fishhook` is Facebook's, BSD-licensed, vendored unmodified; `fishhook.h` is
reconstructed from its API.
