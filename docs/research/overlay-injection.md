---
status: current
re-verify-on: CrossOver upgrade — the load-order deadline is a property of when `winemac.so` is demand-loaded, and the Wine loader owns that; also on a Steam client update, which replaces `gameoverlayrenderer.dylib`
---

# Getting Valve's overlay renderer into a Windows title — what is measured

**The overlay draws over real Windows titles in the bottle, through the shipped compat tool,
with no Windows Steam.** This document is the measured arrangement: what has to load, when it
has to load, and what was tried that does not work. It is the evidence behind
**ADR 0003** and the reason `src/overlay-inject/` exists.

**Measured:** 2026-08-23 → 2026-08-24, sessions of #22, #24, #25, #26.
CrossOver 26.2.0.39821, `wine-11.0-8723-g7e8a47752e3`, bottle `steam-shim`, on Apple Silicon,
macOS 26.5.2.

The binary-level anatomy this rests on — how Valve's renderer hooks Metal, what the two halves
of the overlay are, what CrossOver's present chain bottoms out into — is in
`steam-overlay-feasibility.md` §1–§4 and §6, which remains the reference for all of it.

Claims are marked **[V]** VERIFIED (a command was run, a file was read) or **[I]** INFERRED
(reasoned from verified facts, with the basis stated).

---

## Summary

Five things had to be true at once. All five are measured, and all five hold:

1. **The renderer arms for a process the client has no relationship with.** [V] Not launched by
   Steam, not calling Steamworks — set `SteamOverlayGameId` to a real appid and Shift+Tab draws
   the real overlay. §1.
2. **It must be loaded before `NSApplication` is instantiated.** [V] That, and nothing else, is
   the gate. Not dyld's interposition window, not the Metal device, not the layer. §2.
3. **The 15 dyld interposes are not needed** for the Metal path. [V] §3.
4. **Inside a Wine process the deadline is winnable**, because `winemac.so` is demand-loaded on
   the first USER call, not at `user32` init. [V] §4.
5. **The swizzles see D3DMetal's frames**, so a real Direct3D title is no different from a
   plain `CAMetalLayer` one. [V] §5.

The loader that puts this together is **import-table injection into a `CREATE_SUSPENDED`
process** — ordering, not speed. [V] §6. It needs no entitlement, no re-signed CrossOver
binary, no App Management grant, and touches nothing inside `CrossOver.app`. The alternative —
`DYLD_INSERT_LIBRARIES` at launch — needs all four, which is §7.

Always set `STEAM_OVERLAY_LOGGING=1`. The renderer is silent without it, and a mute failure is
what made the first attempt at this look structurally impossible (see
[Wrong turns](#wrong-turns)).

---

## 1. Arming passes — the client will drive an overlay for a foreign process [V]

`metalprobe` (`attic/overlay-probe/`) is a plain unsigned Metal binary. It is not launched by
Steam and never calls Steamworks. With `SteamOverlayGameId` set to a real appid,
`DYLD_INSERT_LIBRARIES` pointing at `gameoverlayrenderer.dylib`, and `SteamNoOverlayUIDrawing`
unset, **Shift+Tab draws the real overlay over its `CAMetalLayer`.**

This was the study's single biggest risk — whether the native client would start
`gameoverlayui` for a Windows-platform app at all. It does better than pass: the client does not
appear to check its relationship with the process it is arming.

## 2. The gate is `NSApplication`, not the renderer's load mechanism [V]

`attic/overlay-probe/metalprobe5.m` is `metalprobe` with the `dlopen` call site movable across
startup by `DLOPEN_WHEN`. Renderer, environment and render loop are otherwise identical; the
call site is the only variable.

| `DLOPEN_WHEN` | dlopen happens | `Hooking` lines | overlay |
|---|---|---|---|
| `ctor`   | `__attribute__((constructor))`, before `main` | 5/5 | ✅ draws |
| `main`   | first statement of `main`                     | 5/5 | ✅ draws |
| `nsapp`  | after `[NSApplication sharedApplication]`      | 0 | ❌ |
| `device` | after `MTLCreateSystemDefaultDevice()`        | 0 | ❌ |
| `layer`  | after the `CAMetalLayer` is created and attached | 0 | ❌ |
| `late`   | after the window is on screen                  | 0 | ❌ |

`ctor` reproduced across three runs; the overlay was visually confirmed over `metalprobe5`'s
layer on Shift+Tab, and the log's `Enabling overlay` corroborates it.

Load before `NSApp` exists and attach installs the five `MTLCommandBuffer` hooks. Load after and
attach still runs and still prints its module list — and hooks nothing. A successful attach:

```
GameID = 3215050, AppID = 3215050, OverlayGameID = 3215050, PID: 98558 Executable: metalprobe5
Modules at GameOverlayRenderer.dll attach
----------------------------
Hooking _MTLCommandBuffer::presentDrawable: for MTLCommandBuffer
Hooking _MTLCommandBuffer::presentDrawable:atTime: for MTLCommandBuffer
Hooking _MTLCommandBuffer::presentDrawable:afterMinimumDuration: for MTLCommandBuffer
Hooking AGXG15XFamilyCommandBuffer::commit for MTLCommandBuffer
Hooking _MTLCommandBuffer::addScheduledHandler: for MTLCommandBuffer
ValveGetScreenSize( 640, 480 )
Detected hot-key via base input, now requesting overlay enable
Enabling overlay
```

**`STEAM_OVERLAY_LOGGING` is what makes any of this observable.** It and
`STEAM_OVERLAY_LOGGING_FLUSH` are `getenv` calls in the dylib (`0x1e194`, `0x1e17e` in the
x86_64 slice) gating `/tmp/gameoverlayrenderer.%d.log`. [V] Set it on every overlay run.

## 3. The interposes are unnecessary [V]

`metalprobe5` links no `fishhook`, parses no `__DATA,__interpose`, rebinds nothing — and the
overlay draws. The "precisely-known hole" of `steam-overlay-feasibility.md` §3.4, and #21's
GOT-rebinding plan with it, are moot for the Metal path.

The recovery code in `metalprobe3` still works and is still correct; it is simply not on the
path. It matters only if #21's inserted-stub idea comes back.

## 4. The Wine-side deadline is winnable — the mac driver is lazy [V]

Given §2, the question inside a Wine process is whether *any* point exists early enough to beat
`NSApplication`. Two mechanisms were checked.

**`AppInit_DLLs` does not exist here.** The string appears nowhere in CrossOver's Wine tree —
not in `user32.dll`, `win32u.dll`, `ntdll.dll`, nor anywhere under `lib/wine/`. Wine never
implemented it. This is a dead route, not a registry value we are failing to set.

**`winemac.so` loads far later than assumed.** `attic/overlay-probe/u32probe.c` holds at three
stages while `vmmap` samples the process:

| stage | `winemac.so` mapped? |
|---|---|
| A — process init, before `user32` is loaded | no |
| B — `user32.dll` loaded, no USER call made | **no** |
| C — after a single `GetDesktopWindow()` | **yes**, 7 mappings |

C is the control that makes A and B mean something: `vmmap` can see the module, it simply is not
there yet. **The display driver is demand-loaded on the first USER call.**

**[I]** Every PE `DllMain` runs during loader init, before the title's entry point and therefore
before its first USER call — so any DLL loaded at process init beats `winemac.so`, and with it
`NSApplication`. The race is winnable from inside Wine, with no entitlement, no bundle
modification and no TCC prompt.

## 5. D3DMetal's frames go through the same swizzles [V]

Every measurement up to here drew through a plain `CAMetalLayer`. A real title draws through
Direct3D, which CrossOver translates to Metal. `instruments/overlay-probe/d3dprobe.c` is a
Windows D3D11 program run inside the bottle — `D3D11CreateDeviceAndSwapChain` (feature level
`0xb000`), a real swap chain, `Present` in a loop:

```
GameID = 945360, AppID = 945360, OverlayGameID = 945360, PID: 40608 Executable: wineloader
Modules at GameOverlayRenderer.dll attach          <- winemac.so NOT in the list
----------------------------
Hooking _MTLCommandBuffer::presentDrawable: for MTLCommandBuffer
Hooking _MTLCommandBuffer::presentDrawable:atTime: for MTLCommandBuffer
Hooking _MTLCommandBuffer::presentDrawable:afterMinimumDuration: for MTLCommandBuffer
Hooking AGXG15XFamilyCommandBuffer::commit for MTLCommandBuffer
Hooking _MTLCommandBuffer::addScheduledHandler: for MTLCommandBuffer
ValveGetScreenSize( 640, 480 )
ValveGetOutputBounds( 104, 130, 632, 446 )
SetScaleFactors( 1.01, 1.08, 0.99, 0.93 )
Detected hot-key via base input, now requesting overlay enable
Enabling overlay
```

`ValveGetScreenSize( 640, 480 )` is the probe's client area and `ValveGetOutputBounds` its
on-screen rectangle: the renderer is not merely loaded, it is tracking **the D3D window's own
swapchain drawable** and computing overlay geometry from it. Valve hooks Apple's
`MTLCommandBuffer`, and D3DMetal bottoms out into it like everything else.

**Two build notes, both learned the hard way.** The first D3D probe was a console exe with
`printf` and a static `d3d11` import, and it found `winemac.so` already loaded at attach:

- **A console attach reaches USER.** A console process has lost the race before its first
  statement. An injected payload must not assume a quiet CRT.
- **Static imports run their `DllMain` before `main`.** `d3d11.dll` is `LoadLibrary`d by hand in
  the probe for that reason. This is why ADR 0003 specifies `CREATE_SUSPENDED` rather than the
  initial debug breakpoint: at the breakpoint every static import has already initialised, and
  any one of them touching USER ends it.

## 6. The loader: import-table injection, not a thread race [V]

#25 built the injector, and the first two real titles disagreed with each other — which is how
the last real flaw surfaced.

**`CREATE_SUSPENDED` + `CreateRemoteThread` is not early enough under Wine.** It puts the payload
in *after* the title's static imports, because `LdrInitializeThunk` runs on whichever thread runs
first, and with the main thread suspended that is our injected thread. Measured on Surviving
Mars: the payload's `DllMain` reports `user32=LOADED dxgi=LOADED`, `winemac.so` is in the
renderer's module list, and no hooks install. Among Us survives the same mechanism only because
its imports never touch USER.

**Import-table injection fixes it by ordering rather than by speed.** In the suspended process
the exe's import directory is rewritten so the payload is import `[0]`; the loader initialises it
before the title's own imports initialise.

| title | bitness | result |
|---|---|---|
| Surviving Mars | 64-bit | `import [0] of 23` · `winemac` absent at attach · 5/5 hooks · `ValveGetScreenSize( 1766, 1097 )` · overlay draws |
| Among Us | 32-bit (via handover) | `import [0]` · `winemac` absent · 5/5 hooks · overlay draws |
| `d3dprobe` | 64-bit, self-pull off | `import [0] of 11` · `winemac` absent · 5/5 hooks |

**[I]** `dxgi=LOADED` at our `DllMain` is expected and harmless: the loader maps every dependency
before running any `DllMain`. **Mapped is not initialised**, and it is initialisation that brings
up the driver. Reading that field as a failure sends the fix in the wrong direction.

## 7. The route not taken: `DYLD_INSERT_LIBRARIES` at launch

Injecting at launch instead of from inside Wine was the original plan, and everything about it is
measured too. It works, and its shipping story is what rules it out. Kept here because it is the
fallback if §6's mechanism ever breaks, and because three of these findings constrain anything
that touches CrossOver's binaries, whatever it is for.

- **The binary to re-sign is in the lib tree, not `bin/`.** [V] The process that becomes the game
  is reached by a second exec: `game process → /var/folders/…/winetemp-…/wineloader`, whose inode
  is `…/CrossOver/lib/wine/x86_64-unix/wine`. `bin/wineloader` is a different inode and is
  irrelevant to injection. (`CrossOver-Hosted Application/wineloader` is a hard link to
  `bin/wineloader`, so it is not a third variant.)
- **Relocation preserves entitlements.** [V] The `winetemp` path in `ntdll.so` (single xref to
  `/winetemp-%llu-%llu-%lu-%lu/` at `0x22648`) runs
  `asprintf → strlcat → mkdir → symlink("<dir>/ntdll.so") → stat → link → symlink (fallback) →
  posix_spawn`. `link()` is a **hard link**: same inode, same signature, same entitlements. An
  entitled source yields an entitled game process.
- **The front door is fully steerable, and steering it is not enough.** [V] `[Wine] BinPath` is a
  bottle config key (`bin/wine:654`) taking a `:`-separated list (`cxwhich`, `bin/wine:25`) and
  steers `WINELOADER`; `LibPath` is its sibling. `DYLD_*` cannot be passed in from outside
  (`/usr/bin/perl` is SIP-restricted and dyld strips `DYLD_*`), but `[EnvironmentVariables]` in
  `cxbottle.conf` sets them inside perl before the exec (`CXBottle.pm:9-29`). `CX_ROOT` cannot be
  set from the environment — `locate_cx_root` (`bin/wine:45-82`) computes it from
  `cxwhich($ENV{PATH}, $0)` and overwrites it. A mirror root was built and `CX_LOG` confirms all
  three take effect — **and the game still relocates from the stock loader**, because `ntdll`
  resolves the real loader relative to its own path.
- **`codesign` on the shipped loader fails; sign a copy.** [V] `codesign --force --sign -`
  directly on `lib/wine/x86_64-unix/wine` returns `internal error in Code Signing subsystem` —
  the inode had 19 links, one real path plus 18 accumulated `winetemp` dirs. The failure is
  atomic (signature, inode, size and validity all unchanged; CrossOver still runs). Signing a
  copy in `/tmp` succeeds and yields a correctly entitled binary.
- **Installing that copy is blocked by App Management.** [V] `cp /tmp/wine.entitled "$W"` →
  `Operation not permitted`. Not POSIX permissions, not `schg` — `ls -lO` shows no flags and even
  `touch` of a *new* file in that directory fails. `CrossOver.app` is a signed, notarized
  third-party bundle (`TeamIdentifier=9C6B7X7Z8E`), and since macOS 14 modifying another app's
  bundle requires the **App Management** TCC grant for the writing process.

**[I]** So shipping the launch-injection route means an installer that prompts for App Management
over CrossOver, rewrites a CodeWeavers binary inside their signed bundle, and silently re-does it
after every CrossOver update. §6's route costs none of that.

**The ask that would delete all of it.** [V]
`com.apple.security.cs.allow-dyld-environment-variables` is the entitlement Valve's own
documentation names as the macOS overlay's requirement
(`steam-overlay-feasibility.md` §6.3); CrossOver's `wineloader` ships the harder-to-justify
`com.apple.security.cs.disable-library-validation` and not this one. Asking CodeWeavers to add it
is a one-line change on their side with a citable rationale. It is no longer on the critical path,
but it remains the cheapest thing nobody has tried.

**Library validation is not in the way either.** [V] A dylib signed by Valve's team `MXGJJ98X76`
loads into the CodeWeavers-signed, hardened game process today — observed in the Among Us run of
§8's first attempt. `disable-library-validation` holds in practice, not just on paper.

## 8. Where this stands

Graphics, arming, load timing, the Wine-side deadline, D3DMetal and the loader ordering are all
measured and passing, and the overlay draws over two real Windows titles of both bitnesses
through the shipped compat tool.

What remains is not feasibility:

- **Child-process injection is written but untested** — neither test title relaunches itself.
- **Input parity is still unpriced.** Proton's `GameOverlayActivated_t` key-up repair
  (`steam-overlay-feasibility.md` §6.2) has no measured macOS equivalent yet, and §3's result
  says only that the *Metal* path needs no interposes — it says nothing about input.

---

## Wrong turns

Each of these was measured, believed, and overturned by a later measurement. They are here
because the shape of the mistake is reusable, not the conclusion.

### "`dlopen` is dead — the renderer must be inserted at launch"

Recorded after #22, and it closed the cheapest architecture in the study for a day. The harness
was identical apart from load time: inserted at launch the overlay drew, `dlopen`'d it did not,
and rebinding 13 of the 15 dyld interposes made no difference. The reading was that `dlopen`
loses dyld's interposition window and that this is structural.

It is not. **The failure was load *time*, and the timing #22 tested is not the timing the route
would have** — every `dlopen` it tried happened after the window was on screen. §2's `ctor` and
`main` rows are `dlopen` calls, and they hook 5/5.

Two things made the wrong conclusion look solid. The interpose-recovery mechanism genuinely works
as designed — parsing `__DATA,__interpose` and matching each entry's dyld-bound `original` against
`dlsym` identifies 15/15 and hands back Valve's own replacements — so its failure to help read as
evidence that something deeper was missing. And the renderer **ran mute**: without
`STEAM_OVERLAY_LOGGING` there was no log at all, so no stage could be named and a structural
explanation was as good as any other. A negative with no diagnostic is not a strong negative.

### The ranking that followed from it

With `dlopen` closed, launch-injection became the only candidate, its ranking of record was
"viable, injection unproven end-to-end, shipping story depends on the CodeWeavers ask", and the
DIY-renderer path (`steam-overlay-feasibility.md` §5(c)) became the fallback. Superseded: the
route with no shipping-story cost is the one that ships, and §7's costs belong to launch
injection alone.

### The mirror-root family

Two variants were designed and both are dead. A mirror `CX_ROOT` holding a re-signed loader is
defeated by `ntdll` resolving the loader relative to its own path (§7). A mirror root holding a
copy of `ntdll.so` with an added `LC_LOAD_DYLIB` for the renderer — the plan that survived
#22 — was never needed once §4 showed the race is winnable from inside Wine. For the record, the
three ways `ntdll.so` can sit in a mirror root:

| `ntdll.so` in the mirror | result |
|---|---|
| symlink | works, but resolves back into CrossOver's tree — stock sibling wins |
| copy | **SIGSEGV** (exit 139, no output) — signature verifies clean, so not a signing fault |
| hard link | `Operation not permitted` across the app bundle |

### "Our unixlib can `dlopen` the renderer itself"

The shim's own `steamclient.so` constructor does fire, and Valve's renderer does load and attach
inside the real game process — `SHIM_OVERLAY=1`, Among Us (945360) through the compat tool:

```
GameID = 945360, AppID = 945360, OverlayGameID = 945360, PID: 10454 Executable: Among Us.exe
Modules at GameOverlayRenderer.dll attach
----------------------------
                                        <- nothing. no Hooking lines.
```

Line 527 of that same module list is `…/lib/wine/x86_64-unix/winemac.so`: the mac driver was
already loaded, so `NSApplication` already existed, and by §2 that is exactly the state in which
attach installs nothing.

The cause is structural rather than a tuning problem. Our unixlib is `dlopen`'d by ntdll when the
PE `steamclient.dll` is loaded, and that happens when the title calls `SteamAPI_Init` — by which
point the engine has long since brought up its window. No arrangement of the shim moves that
earlier, because the trigger belongs to the game. The loader has to be independent of the title's
call order, which is what §6 is.

### "A remote thread in a suspended process runs before the title's imports"

True on Windows, false under Wine, and it is §6's whole subject: `LdrInitializeThunk` runs on
whichever thread runs first, and with the main thread suspended that is the injected thread — so
the payload lands *after* the static imports rather than before. Among Us passed anyway and
Surviving Mars did not, which is the only reason it was caught.
