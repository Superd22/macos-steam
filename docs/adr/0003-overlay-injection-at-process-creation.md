# 3. The Steam overlay gets into the game by injection at process creation, from inside Wine

Date: 2026-08-24

## Status

Accepted

## Context

Valve already ships the macOS overlay renderer (`gameoverlayrenderer.dylib`), and it hooks
Apple's own classes — `CAMetalDrawable`, `MTLCommandBuffer` — rather than any Windows graphics
API. So the overlay problem was never "write a renderer" or "reverse Valve's IPC" (#21). It is
one problem only: **get that dylib into the game process, early enough.**

Three facts, all measured on this machine, define the whole solution space. They are recorded in
`docs/research/steam-overlay-feasibility.md` (Addendum 2) and `tools/overlay-probe/`.

**1. The renderer arms itself exactly once, at load, and the deadline is `NSApplication`.**
`metalprobe5` moves a `dlopen` of the renderer across a program's startup and bisects it:

| renderer loaded | result |
|---|---|
| before `NSApplication` is instantiated | installs 5 `MTLCommandBuffer` hooks, arms, overlay draws |
| after | attaches, prints its module list, hooks **nothing**, forever |

There is no retry and no lazy second chance. Every design below is judged on one question: does it
beat `NSApplication`?

**2. `DYLD_INSERT_LIBRARIES` — the mechanism Valve's own docs prescribe — is unavailable to us.**
CrossOver's `wineloader` is hardened-runtime signed without
`com.apple.security.cs.allow-dyld-environment-variables`, so dyld strips the variable. Restoring
insertion means putting an entitled binary inside CrossOver.app, which macOS 14+ gates behind an
**App Management** TCC prompt and which must be redone after every CrossOver update (#24). Mirror
roots do not rescue it: `ntdll` resolves the loader relative to its own path, and a copied
`ntdll.so` in a mirror root SIGSEGVs (Addendum A5).

**3. But the Mac display driver is demand-loaded, which reopens the whole thing.**
`winemac.so` — and with it `NSApplication` — does *not* come up with `user32`. `u32probe` holds a
Wine process at three stages while `vmmap` samples it: absent at process init, **still absent after
`user32.dll` is loaded**, present only after a single `GetDesktopWindow()`.

That is the load-bearing fact. Every PE `DllMain` runs during loader init, before the title's entry
point and therefore before its first USER call. **Any DLL we can get loaded at process init beats
`winemac.so`, and with it the deadline** — with no entitlement, no bundle modification and no TCC
prompt.

So the question narrows to: what loads a DLL of ours at process init, on every title?

Candidates considered:

- **`AppInit_DLLs`** — Windows' own "load this into every process" registry hook. **Not available**:
  CrossOver's Wine does not implement it; the string appears nowhere in its tree.
- **Import-time sideload** — a DLL in the title's folder shadowing one it imports (`VERSION.dll`,
  `WINMM.dll`). Works, but is per-title, needs export forwarding to the shadowed DLL or the title
  will not start, and is overwritten by Steam file verification. **Not shippable**; useful only as a
  throwaway test rig.
- **Patch `ntdll.so` in CrossOver.app** to add an `LC_LOAD_DYLIB` for the renderer. Certain to win
  the race and needs no entitlement — but writes inside a signed third-party bundle, so it inherits
  App Management and the per-update repair. Strictly worse than injection.
- **Injection at process creation** — we already create the game process, so create it suspended,
  put our DLL in, resume.

## Decision

**The compat tool launches the title through an injector, which creates the game process with
`CREATE_SUSPENDED | DEBUG_PROCESS`, injects a small PE of ours before any game code runs, and
resumes. That PE causes our unixlib to load, whose constructor `dlopen`s Valve's renderer.**

Two details are load-bearing and are not interchangeable with the obvious alternatives.

**Suspended, not the initial debug breakpoint.** With `CREATE_SUSPENDED` the process has only
`ntdll` mapped and the main thread has never run, so the title's static imports are not yet
resolved. Injecting here is before *any* game `DllMain`. If we waited for the initial breakpoint
instead, every static-import `DllMain` would already have run, and one USER call in any of them
loads `winemac.so` and blows the deadline. Suspended removes that risk class rather than betting
against it.

**`DEBUG_PROCESS`, so children are covered too.** A title that relaunches itself — a launcher exe,
a 32-bit stub that starts a 64-bit binary — would otherwise render in a process we never touched.
As a debugger we receive `CREATE_PROCESS_DEBUG_EVENT` for each child and re-arm. This is the
difference between "works on Among Us" and "works on all titles", which is the project's bar.

### Specified well enough to build

```
compat tool (steamclient-shim-launch.sh)
  └── wine overlayinject.exe -- <title.exe> [args]        ← new, replaces the direct launch
        ├── CreateProcess(title, CREATE_SUSPENDED | DEBUG_PROCESS)
        ├── inject overlayhook.dll into the child          (VirtualAllocEx + WriteProcessMemory
        │                                                    + CreateRemoteThread → LoadLibraryW)
        ├── ResumeThread
        └── debug loop: on CREATE_PROCESS_DEBUG_EVENT for a child, inject + continue;
            exit when the top-level process exits, propagating its exit code
```

- `overlayhook.dll` — PE, both bitnesses. Its `DllMain(DLL_PROCESS_ATTACH)` does one thing: force
  our unixlib to load. It carries no overlay logic of its own.
- `steamclient{,64}.so` — the existing unixlib. Its constructor already `dlopen`s the renderer under
  `SHIM_OVERLAY` (`tools/shim/shim_unix.cpp`). Unchanged by this ADR.
- The env the renderer reads — `SteamOverlayGameId=$APPID`, `SteamNoOverlayUIDrawing` **unset**,
  `STEAM_OVERLAY_LOGGING=1` — is already exported by the launch script.
- Exit code and stdio must pass through the injector unchanged: Steam reads the title's exit status
  through the compat tool, and `waitforexitandrun` semantics depend on it.

### Verification

The renderer's own log is the oracle, and it is opt-in (`STEAM_OVERLAY_LOGGING`) — running without
it is what made #22 misread a timing failure as a structural one and close this whole route.
`/tmp/gameoverlayrenderer.<pid>.log` containing `Hooking …` means we beat the deadline; the same log
stopping after `Modules at GameOverlayRenderer.dll attach` means we did not.

## Consequences

- **No macOS platform blockers.** No entitlement, no re-signed loader, no mirror root, no write
  inside CrossOver.app, no App Management prompt, and nothing to repair after a CrossOver update.
  Every blocker #24 identified belongs to insertion-at-launch, which this does not use.
- **Nothing is written into title directories**, so Steam file verification cannot undo it and
  uninstalling is deleting our own files.
- **We take on a debugger loop.** Being the title's debugger is a real coupling: a title that
  objects to being debugged, or that wants to be its own debugger, is a failure mode we do not have
  today. Mitigation if it bites: fall back to `CREATE_SUSPENDED` alone and lose child coverage.
- **The overlay stays off by default** (`SHIM_OVERLAY`) until it is proven on real titles. A title
  that believes an overlay exists can wait on one forever.
- **Valve's renderer is loaded from the user's own Steam install** and never redistributed, so this
  raises no licensing question — the same shape as ADR 0002's vehicle A.
- **Input parity is not addressed here.** `lsteamclient`'s `GameOverlayActivated_t` / `keybd_event`
  gate has consumers in Valve's Wine fork (`winex11.drv`, `dinput`, `hidclass.sys`, `xinput1_3`)
  that CrossOver's Wine does not have (#21). Gamepad and dinput behaviour while the overlay is up
  is a separate, still-unpriced problem.

### The one unknown this decision does not resolve

Every measurement so far renders through a plain `CAMetalLayer`. A real title renders through
**D3DMetal**, and whether Valve's swizzles see those frames is unproven (S-4). The study argues they
must — Valve hooks Apple's classes, which D3DMetal bottoms out into — but nobody has run it.

**This ADR is therefore accepted as the design, and deliberately not started as the build.** S-4 is
answered first with a throwaway import-time sideload on one title, which is not shippable and is not
meant to be. If the overlay draws over a D3DMetal frame, this gets built. If it does not, the
injector would have been a perfect delivery system for a renderer that sees nothing, and the
conversation moves to architecture (c).

## Correction, 2026-08-24: `CREATE_SUSPENDED` alone is not enough under Wine

The decision above stands — inject at process creation, from inside Wine — but one premise in it was
wrong, and it cost a title.

> *"With `CREATE_SUSPENDED` the process has only `ntdll` mapped and the main thread has never run, so
> the title's static imports are not yet resolved. Injecting here is before *any* game `DllMain`."*

True on Windows. **False under Wine**, because the loader init (`LdrInitializeThunk`) runs on
whichever thread runs *first* — and with the main thread suspended, that is our injected remote
thread. `LoadLibraryW` therefore returns only after the title's entire static-import graph has
loaded and run. Measured on Surviving Mars: the payload's own `DllMain` reports
`user32=LOADED dxgi=LOADED`, and by then `dxgi` had brought up `winemac.so` and `NSApplication`.

Among Us passed only because its imports never touch USER. That is per-title luck, which is the
thing this ADR exists to avoid.

**The mechanism is therefore import-table injection, not a remote thread.** In the suspended
process, before anything has read them, the exe's import directory is rewritten so the payload is
the title's **first static import**. The loader then initialises it before `user32`, before `dxgi`,
before anything can touch the display — an ordering the loader guarantees rather than a head start.
The original directory has no spare room, so a new one is built in memory allocated inside the
target (within 2GB of the image, since RVAs are 32-bit) and the data directory is repointed at it.

`CREATE_SUSPENDED` is still required — it is what gives us an untouched image to rewrite. The remote
thread survives as a fallback for when the patch cannot be applied, since a title-dependent overlay
beats none.

**[I]** `GetModuleHandle` returning non-NULL for `dxgi` at our `DllMain` is not a failure: the loader
maps every dependency before running any `DllMain`. Mapped is not initialised, and initialised is
what loads the driver.

Confirmed on Surviving Mars (64-bit, `import [0] of 23`) and Among Us (32-bit, via the bitness
handover), both drawing the overlay.

## Links

- Supersedes the injection half of #24's two shipping stories (CodeWeavers ask / in-place re-sign).
  The CodeWeavers ask for `allow-dyld-environment-variables` remains worth sending — it would delete
  this ADR's machinery entirely — but it is not a dependency of anything now.
- Evidence: `docs/research/steam-overlay-feasibility.md` Addendum 2 (B1–B7); harnesses in
  `tools/overlay-probe/`.
- Issues: #21 (feasibility), #22 (which closed (a2) on a mute negative — corrected by B1–B2),
  #24 (App Management), #23 (out-of-process routing, ships regardless).
