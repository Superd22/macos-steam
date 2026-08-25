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

## Correction, 2026-08-25: `DEBUG_PROCESS` cannot cover the child that matters

The decision stands — inject at process creation, from inside Wine — but the
child-coverage half of it was built on an assumption that does not hold, and #27
was right to call it "untested code carrying an implicit claim". It was worse
than untested: it could not have worked.

> *"**`DEBUG_PROCESS`, so children are covered too.** A title that relaunches itself —
> a launcher exe, **a 32-bit stub that starts a 64-bit binary** — would otherwise
> render in a process we never touched. As a debugger we receive
> `CREATE_PROCESS_DEBUG_EVENT` for each child and re-arm."*

The example the ADR chose is precisely the one the mechanism cannot handle.
**A 32-bit debugger cannot debug a 64-bit child** — a hard rule on Windows, and
Wine behaves the same way. Warhammer 40,000: Space Marine – Master Crafted
Edition is exactly that shape: `SpaceMarineBootstrapper.exe` is PE32 and starts
`SpaceMarine.exe`, which is PE32+ (its `.ini` says `ApplicationPath=SpaceMarine.exe`,
`WaitForExit=0`).

Measured: **no `CREATE_PROCESS_DEBUG_EVENT` arrives at all.** The debug loop logs
no child, the game receives the payload only later through the ordinary
`steam_api` → registry route — by which time its `DllMain` reports
`d3d12=LOADED dxgi=LOADED` — and `/tmp/gameoverlayrenderer.<pid>.log` contains
**zero `Hooking` lines**. The overlay simply did not exist for that title, and
the top-level injection had reported success, exactly as #27 predicted.

### The mechanism is a hook on the parent's `CreateProcess`

The parent's own `CreateProcess` is the one place guaranteed to run before the
child executes anything, and the payload is already inside the parent as its
first static import. So it IAT-hooks `CreateProcessA/W` (only under
`SHIM_OVERLAY`), forces `CREATE_SUSPENDED`, and hands the child's pid to
`overlayinject<child's bitness>.exe --attach`, which performs the same import
patch this ADR already specifies — in the right bitness. The hook then resumes
the child, unless the caller asked for a suspended process itself.

The bitness problem is *removed* rather than worked around: the patch is always
performed by a process of the child's own bitness, so no cross-bitness memory
write or PE parse is ever attempted.

Confirmed on Space Marine, launched through its bootstrapper:

```
--- overlayinject (64-bit) --attach pid=768
  patched imports: C:\shim\steamclient64.dll is now import [0] of 25
  attached: payload is the child's first static import
gameoverlayrenderer.<game pid>.log: Hooking=5
```

`DEBUG_PROCESS` is kept. It still covers same-bitness children, and it is the
only thing that reports a child created by a module whose `CreateProcess` import
was resolved before our hook went in. The two are complementary; neither is
sufficient alone.

### Consequence for "we take on a debugger loop"

The coupling this ADR accepted is now smaller in practice: the common
relaunch shape is covered by the hook, which is not a debugger and carries none
of the debugger's failure modes. The `DEBUG_PROCESS` caveat above still applies
to the cases only it covers.

## Addendum, 2026-08-25: input parity is better than expected (#28)

The Consequences above say *"Input parity is not addressed here… Gamepad and dinput
behaviour while the overlay is up is a separate, still-unpriced problem."* It has
now been measured, and the pessimism was misplaced.

#28's reasoning was: on Windows and in Proton, `lsteamclient` raises
`GameOverlayActivated_t` and fires a `keybd_event`, and the *consumers* of that
gate live in Valve's Wine fork (`winex11.drv`, `dinput`, `hidclass.sys`,
`xinput1_3`). CrossOver's Wine has none of them, so the half of the mechanism
that tells the game to stop reacting to input should simply be absent.

**It is absent, and input is gated anyway** — because Valve's renderer does not
rely on that path at all. It swizzles
`nextEventMatchingMask:untilDate:inMode:dequeue:`, so it consumes events at the
**Cocoa event pump, below Wine**, before `winemac.drv` ever converts them into
Windows messages. Nothing in Wine has to co-operate.

Measured two ways. With `inputprobe` (`tools/overlay-probe/`), a real D3D11
overlay target that logs every input channel against the overlay's state:
synthesised keystrokes and mouse motion reach the title's message queue with the
overlay down, and produce **nothing at all** with it up — while the title keeps
rendering, and `gameoverlayui` is running against its pid. The control matters:
the same synthetic sequence traced exact coordinates with the overlay closed, so
"no events" is a gate, not a broken instrument.

And on Space Marine, with a real Xbox controller:

| channel | overlay up |
|---|---|
| keyboard | swallowed |
| gamepad — buttons and both sticks | swallowed |
| mouse clicks | swallowed |
| mouse *position* (hover highlight in-game) | **still reaches the title** |
| on close | full functionality returns, cursor included |

The gamepad result is the surprising one, and it is the one #28 called out as
having "no consumer at all in CrossOver's Wine".

**The one leak is hover, and it is parity, not a regression.** The same
behaviour was observed in a bottle running real Windows Steam, so it is how this
title behaves under Valve's own overlay rather than something our route
introduces. Recorded as a known limitation; not worth chasing.

Note this removes the ADR's implied prerequisite that shipping the overlay would
first require reimplementing the `GameOverlayActivated_t` input gate PE-side.
It would not. The remaining blocker on turning the overlay on by default is the
Steamworks overlay API surface (#23), not input.

## Addendum, 2026-08-25: the overlay API surface, and who answers `IsOverlayEnabled` (#23)

The addendum above named #23 as the remaining blocker on turning the overlay on
by default. It is now closed, and one decision inside it is worth recording here
because it is the difference between an overlay that works and a title that
hangs.

**`IsOverlayEnabled` is not ours to answer.** The original plan was an honest
hardcoded `false`, on the sound reasoning that a title told `true` with no
compositor pauses forever waiting for a panel that never appears. #25 made that
answer wrong without making the reasoning wrong — it now has to track reality,
in both directions, or it is a hang one way and a dead button the other.

So we do not track it. Valve's renderer exports `IsOverlayEnabled`,
`BOverlayNeedsPresent` and `SetNotificationPosition`, and the unixlib already
has the renderer in-process, so it `dlsym`s them and forwards. That makes the
answer correct by construction rather than by bookkeeping we could get out of
step: with `SHIM_OVERLAY` off we never `dlopen` the renderer, there is no
symbol, and the answer is `false`.

The important part is that this distinguishes **loaded** from **armed**.
Disassembly of `gameoverlayrenderer.dylib` shows the byte `IsOverlayEnabled`
returns starts at `0` and is written `1` at exactly one site — inside the
client-handshake loop, after a successful virtual call, alongside
`"Forcing internal overlay disable and requesting ui disable"`. Loading the
dylib is not enough to make it say yes. Confirmed behaviourally: the harness run
with `SHIM_OVERLAY=1` logs `overlay: dlopen(...) -> 0x3d1580` and all three
symbols resolved, and `IsOverlayEnabled() -> 0 (renderer loaded)` — because a
console exe has no swapchain and the overlay never armed. That is the right
answer, and it is one a boolean tracking `SHIM_OVERLAY` would have got wrong.

**There is no inset setter.** `SetOverlayNotificationInset` is accept-and-ignore
by necessity, not by choice: the renderer exports no counterpart. It returns
void and moves a toast a few pixels, so nothing observable to a title depends
on it.

**The activators cross the seam with their slot number.** Every other forwarded
method in the shim reaches the native object by casting the handle to one fixed
C++ class in declaration order. `ISteamFriends::ActivateGameOverlay` cannot:
it sits at slot 19, 20, 21, 22, 28 or 27 depending on which of the fifteen
versions the title asked for, so one class would silently call `GetClanTag` or
`SetPlayedWith` on thirteen of them. The PE half already resolves methods
against the version's own generated table, so it sends the slot it found and
the unix half indexes the native vtable with it. This is safe *here* and only
here: `ISteamFriends` and `ISteamUtils` have no same-name overload in any
version, which is the one condition under which the MSVC order the PE side
holds and the dylib's Itanium order cannot diverge.

Unarmed, the activators still forward, and Valve's client brings the native
macOS Steam window to the front on the right page. That is degraded, but it is
a real answer where a stub was a dead button.
