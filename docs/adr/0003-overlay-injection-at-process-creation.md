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
`docs/research/overlay-injection.md`, `instruments/overlay-probe/` and `attic/overlay-probe/`.

**1. The renderer arms itself exactly once, at load, and the deadline is `NSApplication`.**
`metalprobe5` moves a `dlopen` of the renderer across a program's startup and bisects it:

| renderer loaded | result |
|---|---|
| before `NSApplication` is instantiated | installs 5 `MTLCommandBuffer` hooks, arms, overlay draws |
| after | attaches, prints its module list, hooks **nothing**, forever |

There is no retry and no lazy second chance. Every design below is judged on one question: does it
beat `NSApplication`?

> **Restated 2026-09-05 (#113).** Both rows of that table are exact and still reproduce. The words
> around them — "arms itself at load", "the deadline", "no second chance" — are not: the renderer
> installs those hooks from a swizzled `-[NSApplication init]`, so a late load misses a *call*, and
> the call can be made by hand afterwards. See the correction at the bottom of this ADR. The
> decision below is unaffected; what changes is that beating `NSApplication` stops being the only
> way to get hooks installed.

**2. `DYLD_INSERT_LIBRARIES` — the mechanism Valve's own docs prescribe — is unavailable to us.**
CrossOver's `wineloader` is hardened-runtime signed without
`com.apple.security.cs.allow-dyld-environment-variables`, so dyld strips the variable. Restoring
insertion means putting an entitled binary inside CrossOver.app, which macOS 14+ gates behind an
**App Management** TCC prompt and which must be redone after every CrossOver update (#24). Mirror
roots do not rescue it: `ntdll` resolves the loader relative to its own path, and a copied
`ntdll.so` in a mirror root SIGSEGVs (`docs/research/overlay-injection.md`, Wrong turns).

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
  CrossOver's Wine does not implement it; the string appears nowhere in its tree. (Upgraded from a
  static observation to a live negative in 2026-09: upstream ships `LoadAppInitDlls` as a `TRACE`-only
  stub, and planting the registry values loads nothing —
  `docs/research/drm-overlay-late-arming.md` §5.)
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
  `SHIM_OVERLAY` (`src/shim/shim_unix.cpp`). Unchanged by this ADR.
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
- ~~**The overlay stays off by default** (`SHIM_OVERLAY`) until it is proven on real titles. A title
  that believes an overlay exists can wait on one forever.~~ **Superseded 2026-08-25** — the route
  is proven across titles and the default is now ON; `SHIM_OVERLAY=0` opts out. See the addendum
  below for what carried over and what did not.
- **Valve's renderer is loaded from the user's own Steam install** and never redistributed, so this
  raises no licensing question — the same shape as ADR 0002's vehicle A.
- ~~**Input parity is not addressed here.**~~ **Priced 2026-08-25 (#28)** — the concern was that
  `lsteamclient`'s `GameOverlayActivated_t` / `keybd_event` gate has consumers in Valve's Wine fork
  (`winex11.drv`, `dinput`, `hidclass.sys`, `xinput1_3`) that CrossOver's Wine lacks. Measured, the
  bill is small: gamepad is fine, and the only leak is hover — which reproduces under real Windows
  Steam in a bottle, so it is parity, not a regression. See the #28 addendum.

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
- Evidence: `docs/research/overlay-injection.md` §1–§6; harnesses in
  `instruments/overlay-probe/` + `attic/overlay-probe/`. The feasibility study that preceded it,
  and the binary-level anatomy it still holds, are in `docs/research/steam-overlay-feasibility.md`.
- Issues: #21 (feasibility), #22 (which closed (a2) on a mute negative — since corrected),
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

Measured two ways. With `inputprobe` (`instruments/overlay-probe/`), a real D3D11
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

### Confirmed on a real title, 2026-08-25

Surviving Mars, launched through the compat tool with `SHIM_OVERLAY=1`. The
trigger was not the one predicted — the Paradox promo link on the main menu
(`ParadoxMenu.lua:419`, `OpenUrl(entry.url)` with no force flag) rather than the
Workshop banner — which makes it better evidence, being a call site a player hits
without looking for it.

```
overlay: dlopen(.../gameoverlayrenderer.dylib) -> 0x775e10 pid=48787
overlay: predicates IsOverlayEnabled=0x21124c354 BOverlayNeedsPresent=0x21124c03c
         SetNotificationPosition=0x21124bb19 (#23)
IsOverlayEnabled() -> 1 (renderer loaded)
ActivateGameOverlayToWebPage("https://www.paradoxinteractive.com/games/...", mode=0)
         slot=30 fn=0x221953c06
```

`gameoverlayrenderer.<pid>.log` carries five `Hooking` lines for the title's own
process. The page drew in the overlay.

The `IsOverlayEnabled() -> 1` is the line that closes the argument, because the
same code returns `0` in the harness. Nothing about the shim differs between the
two runs; what differs is that Mars has a Metal swapchain to hook and a console
exe does not. A boolean tracking `SHIM_OVERLAY` would have said `1` in both, and
the harness case is exactly the shape of a title that would then have hung.

One of the five renderer logs (a child process) has zero `Hooking` lines and did
not arm. Consistent with #27: the injector covers the child, but a helper
process with no swapchain has nothing to hook, and `IsOverlayEnabled` correctly
answers `false` there rather than being forced true by the parent's state.


## Addendum, 2026-08-25: the overlay is on by default (#21)

The Consequences section above made the default conditional on "proven on real
titles". It has been, across titles, so the default is flipped. `SHIM_OVERLAY=0`
opts out, at the installer (`SHIM_OVERLAY=0 ./install.sh`, which bakes the value
into the launcher) or per-launch.

**What did not flip is the interlock.** "On by default" is still conditional on
being able to deliver a renderer: the compat-tool launch script keeps requiring
both `overlayinject{32,64}.exe` before arming anything, because a title told an
overlay exists can wait on one forever, and no injector means no compositor.

**The asymmetry this introduces is the part to be careful about.** *Unset* now
means ON. Every layer that wants the overlay off must therefore export a literal
`SHIM_OVERLAY=0` rather than simply omit the variable — omission used to mean
"off" and now means the opposite. Three places were changed for this and all
three are load-bearing:

- `install.sh` always writes the value into the launcher, `1` or `0`, never
  omits it — a reinstall is how a user turns the overlay off, and "off" has to
  reach the bottle as a statement
- the launch script's no-injector branch exports `SHIM_OVERLAY=0` explicitly,
  which is what stops the unixlib's new default from `dlopen`ing a renderer into
  a process with nothing to place it
- `run.sh` does the same in its disabled branch, so the negative control stays a
  real control

Verified both directions: unset → `overlay: dlopen(...) -> 0x1b3580`, and
`SHIM_OVERLAY=0` → no `dlopen` line at all.

**Superseded in mechanism by ADR 0006, not in policy.** The three load-bearing
places above (and two more that were not listed) each wrote their own test for
this rule, and one of them — the PE half's `GetEnvironmentVariableA(...,
NULL, 0) > 0` — read an explicit `SHIM_OVERLAY=0` as ON. The rule now has an
owner: one generated predicate, `shim_overlay_enabled`, declared in
`src/layout/layout.json`. The default and the interlock are unchanged.

## Correction, 2026-09-05: the deadline is a missed method call, not a race (#113)

**The decision stands, unchanged.** Injection at process creation is still how an ordinary title
gets the overlay, still the only mechanism that is per-title and needs nothing bottle-global, and
every measurement in this ADR still reproduces. What is wrong is the *reason* given for it, and the
wrong reason made a whole class of solutions look impossible for a month.

### What this ADR says, and what is actually true

The Context frames the problem as a race: `NSApplication` is a deadline, the renderer "arms itself
exactly once, at load", and a design is judged on whether it *beats* `winemac.so`. Every
observation behind that framing is real. The mechanism is not.

`docs/research/drm-overlay-late-arming.md` §1 disassembles the renderer. It carries an ObjC
category `SteamMetalHook` on `NSApplication`, and at setup it **exchanges `-[NSApplication init]`
with its own `-steammetalhook_init`**. The five Metal hooks are written by a once-guarded installer
that is called from **exactly one place: inside that swizzled method.** So:

- load before `NSApplication` is instantiated → the app's own `[NSApplication sharedApplication]`
  runs `init`, the swizzle fires, the hooks go in. The first row of the Context's table.
- load after → the swizzle is installed correctly and **nothing ever calls it**. Not a lost race:
  a method call that is never made. The second row.

The two are indistinguishable from outside, which is why the correlation held for every run anyone
had done — including #107's spike B, which rendered 2,100+ frames after a successful `dlopen` and
hooked none of them, and read that as conclusive proof of a hard deadline.

**It is recoverable.** The installer is reachable through the ObjC runtime by name, and calling it
with `self = nil` — in a process where `winemac.drv` and `NSApplication` are already up — installs
5/5 hooks:

```
pid 79384   Hooking=5   winemac present, NSApp up, installer called by hand
pid 80198   Hooking=0   same conditions, control
```

### Why this does not change the decision

Injection is still the right mechanism for a title we launch ourselves:

- It is **per-title**, which the alternatives in `drm-overlay-late-arming.md` §3 (a display-driver
  shim, registered bottle-globally) are not — and ADR 0012 gives the *user* a per-title say, so a
  bottle-global delivery would be the wrong granularity for the switch it has to obey.
- Getting hooks in **before the title's first frame** is still strictly better than getting them in
  later, and injection is what does that.
- Nothing above about `DYLD_INSERT_LIBRARIES`, the demand-loading of `winemac.so`, or the debugger
  loop is affected. Facts 2 and 3 of the Context stand as written.

What changes is that **being import[0] is one way to make the installer run, not the only way**.
That distinction is the whole of #112: a DRM-wrapped title cannot have the injector (ADR 0014), and
now does not need it — the shim's own unixlib calls the installer once `NSApp` is up. See ADR
0014's 2026-09-05 amendment.

### The guard that keeps this ADR's path untouched

The late arming is gated on `NSApp` being **non-nil**, which is precisely "Valve's trigger has
already fired and cannot fire again". On the injector path `NSApp` is nil when our constructor runs,
the arming declines and says so in `shim-unix.log`, and this ADR's ordering is bit-for-bit what it
was. Measured both ways, with the injector route re-run for the occasion: 5/5 hooks, `winemac`
absent at attach, unchanged.

One thing named in the 2026-08-25 addendum below did move, and it is worth saying plainly because
that addendum ends with "the default and the interlock are unchanged". The **interlock** in
`steamclient-shim-launch.sh` now asks "is there a way to deliver a renderer" rather than "is there
an injector", because there are two ways. Its purpose is untouched — a title told an overlay exists
can still wait on one forever, so the env is still never armed without a delivery path — and the
`USE_DRM = 0` condition on the *injector call site* is untouched too, because that is where the
exclusivity with ADR 0014 actually lives.

### What is still unproven, and is not claimed here

The late path is **not** a proven equal of this one, and nothing in this correction should be read
as retiring the injector:

- **Input parity on the late path is unmeasured.** The 2026-08-25 addendum below measured the
  *early* path only. The renderer's cursor/event swizzles are installed from its own load path
  rather than from the `init` swizzle, which is why the late path is expected to keep input —
  reasoned from the disassembly, not measured. `input-parity-run.sh` needs an operator.
- **Nobody has seen the overlay panel on a wrapped title.** The hooks are installed and the renderer
  tracks the drawable frame by frame; the hotkey needs a human.
- **Anti-tamper tolerance of the late path is unmeasured** against a title actually running its
  anti-cheat.

### Why the wrong version is still above

It is left in place, with a pointer rather than a deletion. The race framing is what the injector
was designed against, it is why `overlayinject` looks the way it does, and a reader who finds only
the corrected version cannot tell which parts of the design were load-bearing for a reason that has
since evaporated. `docs/research/` keeps its wrong turns for the same reason.
