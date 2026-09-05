---
status: current
re-verify-on: a Steam client update — the whole answer rests on how `gameoverlayrenderer.dylib` installs its Metal hooks (a swizzle of `-[NSApplication init]`, two selector names, one once-guard), and Valve replaced that file on 2026-09-03 during this project's life, so the offsets below are one build's; also a CrossOver upgrade — the display-driver selection in `win32u`/`explorer`, ntdll's `CX_APPLEGPTK_LIBD3DSHARED_PATH` preload and `AppInit_DLLs` staying unimplemented are CodeWeavers'/upstream Wine's to change
---

# A DRM-wrapped title's overlay is recoverable — the renderer can be armed late

**Yes.** Today a DRM-wrapped title launches with the overlay off, because the DRM route
(ADR 0014) and the overlay injector (ADR 0003) both want the title's first static import and
cannot both have it. That framing turns out to be a statement about *the injector*, not about
the overlay. Valve's renderer does not have to be loaded before `NSApplication`: it needs its
Metal-hook installer to *run*, and it only runs that installer from a swizzled
`-[NSApplication init]`. Load the renderer late and the swizzle never fires — but the installer
is reachable through the ObjC runtime by name, and **calling it by hand, in a Wine process where
`winemac.drv` and `NSApplication` are already up, installs all five hooks and arms the overlay on
the hotkey.** The "deadline" ADR 0003 is built on is Valve's own trigger, not a law of the
platform — and the DRM route already has our code in the process, so recovering the overlay is a
call, not a new load site.

Two further routes that beat `user32`/`winemac` *without* rewriting the title's imports were also
built and measured positive; the highest-value candidate the brief named (`AppInit_DLLs`) is dead
three independent ways. Everything is ranked in §0 and detailed below.

**Measured:** 2026-09-05, bottle `steam-shim`, CrossOver **26.3.0.39832**
(`wine-11.0-8726-g2e2f5fca349`, new WoW64), macOS **26.4 (25E246)**, Apple Silicon. Steam client
with `gameoverlayrenderer.dylib` **462 736 B, mtime 2026-09-03** — **not** the 497 216 B /
2026-08-03 build `steam-overlay-feasibility.md` cited, so its `0x1e194` `getenv` offset has moved
(now `0x1aef6`) and every offset below is for the new x86_64 slice. Test subjects:
`instruments/overlay-probe/d3dprobe.c` (with its honest `SHIM_NO_SELF_PULL=1`) and a native
harness derived from `attic/overlay-probe/metalprobe5.m`. **Space Marine 2 was not launched by
this work** — every bottle measurement is on `d3dprobe`, driven into the exact state the real
title's run C recorded (renderer arriving after `winemac.drv`, `NSApp` non-nil). The bottle was
left in the hand-armed DRM state it started in.

Claims are marked **[MEASURED]** (a command ran, a file was read, a log exists) or **[REASONED]**
(inferred from measured facts, with the basis stated).

---

## 0. The core question, answered and ranked

Ranked by (a) does it beat `user32`/`winemac`, (b) would anti-tamper object, (c) cost,
(d) evidence.

| | beats the deadline? | anti-tamper sees a modified image? | implementation cost | confidence |
|---|---|---|---|---|
| **A. Late trigger from the shim's own unixlib** | *deadline dissolved* — arms after `NSApp` and `winemac` exist | **no** — no injection, no import rewrite, no suspended process, no extra PE; one ObjC-runtime call in a dylib the DRM route already loads | ~20 lines in `shim_unix.cpp` + lifting the `USE_DRM`→overlay-off interlock | **high** — measured in the bottle with `winemac` present; the hotkey armed the overlay |
| **B. Display-driver shim** (`Software\Wine\Drivers\Graphics`) | **yes** — by construction it *is* what loads `winemac.drv`, so it runs first | one extra unsigned PE in the process; **title image untouched, no IAT rewrite** | one PE + one registry value; bottle-global; explorer restart | **high it works** (measured 5/5, `winemac` absent at attach); anti-tamper reaction unmeasured |
| **C. ntdll preload** via `CX_APPLEGPTK_LIBD3DSHARED_PATH` | **yes** — before any PE is mapped | no — nothing PE-side at all | wrapper dylib **plus a mirror CrossOver root**, because `bin/wine` overwrites the variable through the front door | high it works (measured); low it ships |
| D. `AppInit_DLLs` | — | — | — | **dead** — upstream stub, absent from CrossOver's binaries, measured no-op |
| E. `DYLD_INSERT_LIBRARIES` + the entitlement | yes | no | **not ours to ship** — `wineloader` lacks `allow-dyld-environment-variables`; re-signing is App Management + per-update repair | dead for us; alive only as a CodeWeavers ask |
| F. Proton's `GameOverlayRenderer64.dll` | n/a | n/a | n/a | **a facade** — its exports are trampolined into `lsteamclient` stubs; Linux's overlay arrives as `gameoverlayrenderer.so` by `LD_PRELOAD` |

**Recommendation: ship A, keep B in reserve.** A is the only candidate that needs no new load
site, mutates nothing bottle-global, and stays inside the exact anti-tamper envelope ADR 0014 was
written to respect (in-process, at loader/runtime, title's own bytes untouched). Its one
dependency is Valve's `NSApplication`-init swizzle, which §1 pins precisely and which the
`re-verify-on` tracks. **B is the fallback** if Valve ever removes that swizzle: it depends on
nothing of Valve's internal structure, only on the renderer loading before `NSApplication`, and
it too rewrites none of the title's imports. C is a fact worth recording, not a route to ship.

This **reopens the "mutually exclusive" consequence of ADR 0014**: that exclusivity is real only
for the *injector*, which needs import[0]. A and B deliver the overlay by other means, so a
wrapped title need not launch with the overlay off. See §6.

---

## 1. What the renderer actually checks (brief item 2)

The known correlation was "`winemac.so` present at attach ⇒ zero hooks." It is a proxy, and the
32-bit Space Marine 2 launcher had already broken it before this work: on disk,
`/tmp/gameoverlayrenderer.96699.log` (`Warhammer 40000 Space Marine 2.exe`, i386) has **no
`winemac` in its module list and zero `Hooking` lines** — `winemac` absent yet no hooks, which the
proxy cannot explain. **[MEASURED]** The cause is narrower and is in the binary.

### 1.1 Metal hooks are installed only from a swizzled `-[NSApplication init]` [MEASURED]

`otool -ov` / `otool -tV -arch x86_64` on the 2026-09-03 dylib:

- One ObjC category, `SteamMetalHook`, on `_OBJC_CLASS_$_NSApplication`, with two instance
  methods: `steammetalhook_init` (IMP `0x115d0`) and
  `steamhooked_nextEventMatchingMask:untilDate:inMode:dequeue:` (IMP `0x1bea4`) (`__objc_catlist`).
- The renderer's setup (reached from the `CBaseOverlayRenderer` path at `0x151f9`→`0x11bb2`) does
  `objc_lookUpClass("NSApplication")` then calls the swizzle helper `0x1c0c6` with selectors
  `init` and `steammetalhook_init` — i.e. it **exchanges `-[NSApplication init]` with its own
  `steammetalhook_init`**.
- `steammetalhook_init` (`0x115d0`) allocs two `NSLock`s, calls the **once-guarded** Metal-hook
  installer `0x11622` (guard byte at `0x16771`, set to 1 on first entry), then tail-messages the
  original `init` on `self`.
- `0x11622` is the only writer of the five hooks: `MTLCreateSystemDefaultDevice` →
  `newCommandQueue` → for each of `presentDrawable:`, `presentDrawable:atTime:`,
  `presentDrawable:afterMinimumDuration:`, `commit`, `addScheduledHandler:` it swaps in an
  `imp_implementationWithBlock`, printing `"Hooking %s::%s for %s"` (`0x11a57`) or
  `"Failed to hook …"` (`0x119db`).

So the installer runs **iff `-[NSApplication init]` is invoked after the swizzle is in place.**
Load the renderer before `winemac.drv` brings up `NSApplication` and the app's own
`[NSApplication sharedApplication]` triggers it; load after, and `init` has already run — the
swizzle is installed but nothing calls it. That, not "winemac present", is the bail. The
module-list dump (`"Modules at GameOverlayRenderer.dll attach"`, `0x1acfd`, via a
`__dyld_get_image_name` loop) prints on *attach* regardless, which is why a failed run still logs
the module list and nothing more — the shape every failing log on disk shows.

Input is not gated the same way: the cursor/event swizzles (`NSCursor` set/push/pop/hide/unhide,
`NSEvent` pressedMouseButtons/mouseLocation, `NSApplication`
`nextEventMatchingMask:…`) are installed at `0x1bc10` from the renderer's own load path, **not**
from the `init` swizzle. **[REASONED]** from the disassembly, and consistent with §2's late-trigger
run logging `"Detected hot-key via base input"` with only the Metal installer forced.

### 1.2 `SteamNoOverlayUIDrawing` is still a hard veto [MEASURED]

Independently of the above, `0x1b0c0` does `getenv("SteamNoOverlayUIDrawing")` → `atoi` → `== 1`
and logs `"SteamNoOverlayUIDrawing was set"`. Any route must keep it unset, exactly as the launch
script already does for the injector path.

---

## 2. Route A — arm the renderer late, by calling its installer (measured positive)

If the installer only needs to *run*, we can run it. Locate the swizzled IMP through the ObjC
runtime (no hardcoded offset): after the renderer is loaded, one of `-[NSApplication init]` or
`-[NSApplication steammetalhook_init]` has an IMP inside `gameoverlayrenderer.dylib`
(`dladdr`); call it with `self = nil`, so its tail-message to the original `init` is a safe no-op.

**Native harness** (`metalprobe5` lineage, renderer `dlopen`'d *after* the window is up —
metalprobe5's `late` timing, which scores 0/5 today):

```
run  DLOPEN_WHEN=late  LATE_TRIGGER   Hooking   note
77568   late            (none)          0        control: dead, as ADR 0003 predicts
77377   late            call installer  5        + ValveGetScreenSize/OutputBounds
```

`init IMP … in …/gameoverlayrenderer.dylib`, `calling renderer IMP with self=nil`, `returned` —
then five `Hooking` lines. **[MEASURED]**

**In the bottle**, the decisive control. A stand-in at the shim's own renderer path
(`$HOME/…/gameoverlayrenderer.dylib`) is loaded by the real `steamclient64.so` unixlib exactly as
today, i.e. late (during `CreateInterface`, long after `winemac.drv` is up). `d3dprobe` runs with
`SHIM_NO_SELF_PULL` unset so the unixlib is what pulls the renderer:

```
pid    winemac at attach   LATE_TRIGGER   Hooking   renderer log
79384   present (winemac=1) call installer   5       ValveGetOutputBounds, SetScaleFactors,
                                                     "Detected hot-key … Enabling overlay"
80198   present             (none)           0       module list only
```

`79384` is the whole finding: **`winemac` loaded, `NSApp` non-nil, and the overlay still installs
its five hooks and arms on the hotkey**, because the installer was called by hand. `80198`, the
same path without the call, is the 0-hook state shipping today. **[MEASURED]**

### 2.1 Why this fits the DRM route with almost no new code [REASONED]

On the DRM route our shim is already in the process: Valve's signed DLL → trampolines →
`lsteamclient` → the `steamclient64.so` unixlib, whose `overlay_load` constructor already
`dlopen`s the renderer under the overlay switch (`src/shim/shim_unix.cpp`). That `dlopen` happens
late and today yields 0 hooks — the "our unixlib can dlopen the renderer itself" wrong turn in
`overlay-injection.md`. The missing piece is precisely §2's call. To ship A:

1. On the DRM route, stop forcing the overlay off: keep the switch on, leave `SteamNoOverlayUIDrawing`
   unset, set `SteamOverlayGameId=$APPID` (the launch script already knows how — it is the
   injector branch, minus the injector).
2. In `overlay_load`, after `dlopen`, resolve the swizzled IMP via the ObjC runtime and call it
   with `self=nil`.

No import rewrite, no suspended process, no `CreateRemoteThread`, no second module — the four
things ADR 0014 says anti-tamper rejects, none of which this does. It is the same class of move as
the DRM trampolines themselves: in-process, at runtime, the title's own bytes untouched. The
residual risk is honest: this was proven with `d3dprobe`, not against Space Marine 2's
EasyAntiCheat, and it calls an undocumented Valve internal located by selector name (robust to
address changes, not to Valve deleting the swizzle — the `re-verify-on`).

---

## 3. Route B — a display-driver shim (measured positive, no import rewrite)

`explorer`/`win32u` load the graphics driver by reading `HKCU\Software\Wine\Drivers\Graphics`
(default `"mac,x11,wayland"`), and for each name load `wine<name>.drv`
(`dlls/win32u/driver.c`, `programs/explorer/desktop.c` `load_graphics_driver`, both `wine-11.0`).
The driver DLL registers itself as *the* user driver from its own `DllMain`
(`winemac.drv` → `macdrv_init` → `macdrv_start_cocoa_app` → `NSApplication`). So a DLL named first
in that list runs its `DllMain` **before `winemac.drv` starts Cocoa**.

Planting `winesteamoverlay.drv` (a 25-line PE whose `DllMain` `LoadLibrary`s our shim, then
`LoadLibrary`s the real `winemac.drv`) and setting `Graphics="steamoverlay,mac"`, then `d3dprobe`
with `SHIM_NO_SELF_PULL=1` so **only the driver shim can bring the renderer in**:

```
C:\winesteamoverlay.log:  winesteamoverlay.drv attach: winemac.drv=0000000000000000  ← not yet loaded
                          shim -> loaded ; winemac.drv -> loaded
system.reg:               GraphicsDriver = winesteamoverlay.drv   ← explorer selected ours
/tmp/gameoverlayrenderer.83629.log:  Hooking=5, winemac absent at attach
```

**[MEASURED]** The overlay installs its hooks with the title's imports untouched — the only PE
footprint is one extra module in the process, and the game exe on disk and its IAT are byte-identical
to an unmodified launch. `winemac.drv` still drives (it registered itself via
`__wine_set_user_driver`); our shim merely rode in first. Costs and caveats: it is **bottle-global**
(the key affects every process and `explorer`, the same shape as `SteamClientDll64`; mitigation is
`SHIM_BOTTLE`), needs an `explorer`/wineserver restart to re-select, and its tolerance by real
anti-tamper is unmeasured. It is the fallback to A because it depends on nothing inside Valve's
dylib — only on loading before `NSApplication`, which it guarantees structurally.

---

## 4. Route C — ntdll's own preload hook (works; does not ship through the front door)

CrossOver's `ntdll.so` reads `CX_APPLEGPTK_LIBD3DSHARED_PATH` at init and `dlopen`s it
(`_getenv` at `0x2415c` in `ntdll.so`, then `dlopen`/`dlsym` of `supports_non_native_code_regions`
and `register_non_native_code_region`) — a native `.dylib` loaded at process init, before any PE
is mapped. A wrapper at that path that forwards the two symbols to the real
`libd3dshared.dylib` and `dlopen`s Valve's renderer from its constructor:

```
/tmp/libd3dwrap.log:  ctor: winemac.so already loaded? 0x0   ← we are earliest
                      real libd3dshared … ; renderer …
/tmp/gameoverlayrenderer.81135.log:  Hooking=5, winemac absent at attach   (SHIM_NO_SELF_PULL=1)
```

**[MEASURED]** It beats the deadline cleanly and touches nothing PE-side. But it does **not
ship**: `bin/wine` sets `CX_APPLEGPTK_LIBD3DSHARED_PATH` itself, unconditionally, *after*
`CXBottle::set_environment` runs (`bin/wine`, "libd3dshared is used regardless…"), so a value from
`cxbottle.conf [EnvironmentVariables]` is overwritten and the front door the launch script uses
(`wine --bottle`) cannot steer it. Delivering it means either replacing CrossOver's own
`libd3dshared.dylib` in place (inside the signed bundle → App Management, per-update repair, the
exact cost `overlay-injection.md §7` rules out) or a mirror `CX_ROOT` — strictly more machinery
than A or B. Recorded as a mechanism, not a route.

---

## 5. Route D — `AppInit_DLLs` is dead three ways (brief item 1)

The brief's highest-value candidate. Established dead by three independent checks:

1. **Upstream Wine implements it as a stub.** `dlls/kernelbase/loader.c` (`wine-11.0`):
   `void WINAPI LoadAppInitDlls(void) { TRACE("\n"); }` — it loads nothing. **[MEASURED]**
2. **CrossOver ships that stub.** The string `AppInit_DLLs` appears only as an *export name* in
   `kernel32.dll`/`kernelbase.dll` (the `LoadAppInitDlls` entry point) and **nowhere** as the
   registry path a real implementation would read — a binary scan (utf-8 and utf-16le) of the
   whole `lib/wine` tree finds it in no `user32`, `win32u`, or `ntdll`. **[MEASURED]**
3. **It is a measured no-op.** Planting
   `HKLM\…\Windows NT\CurrentVersion\Windows\AppInit_DLLs = C:\shim\steamclient64.dll` and
   `LoadAppInit_DLLs = 1`, then `d3dprobe` with `SHIM_NO_SELF_PULL=1`: no renderer log, no line
   added to `shim-unix.log` — the shim never loaded. Values removed afterward. **[MEASURED]**

So even the load-order question the brief posed (does `AppInit` beat `winemac`?) is moot here:
there is no load to order. This upgrades `overlay-injection.md §4`'s "the string appears nowhere"
from a static observation to a live negative.

**`WINEDLLOVERRIDES` is not an alternative:** it selects builtin-vs-native for a name, it does not
reorder the import graph, so it cannot put a DLL ahead of `user32`; and it is separately measured
not to survive CrossOver's front door (`steam-drm-shared-memory.md`). **[REASONED]** + cited.

---

## 6. Route E — the entitlement, and Route F — Proton's Windows PE (brief items 5, 3)

**E.** `codesign -dv --entitlements -` on CrossOver 26.3's
`lib/wine/x86_64-unix/wine`: `flags=0x10000(runtime)`, entitlements
`com.apple.security.cs.disable-library-validation`, `…allow-unsigned-executable-memory`,
`…disable-executable-page-protection` — and **not**
`com.apple.security.cs.allow-dyld-environment-variables`. **[MEASURED]** So `DYLD_*` is stripped,
as `overlay-injection.md §7` records. There is **no supported CrossOver mechanism for preload-style
injection through the front door** (confirmed: `[EnvironmentVariables]` cannot set `DYLD_*` and is
overridden for the one native-preload hook that exists, §4). The only clean fix is CodeWeavers
adding that entitlement — a one-line, citably-justified ask, still worth sending, still not a
dependency of anything now. **Re-signing their binary ourselves is not shippable** — it lives inside
a signed, notarized third-party bundle whose modification macOS 14+ gates behind App Management and
which every CrossOver update undoes. Public traces of others hitting the same wall exist but are
unverified forum/issue posts ([CXPatcher #239](https://github.com/italomandara/CXPatcher/discussions/239):
"CrossOver also doesn't let me use export DYLD_INSERT_LIBRARIES=…, it just acts like the command
doesn't exist"); the entitlement fact above is the primary source.

**F.** Proton copies `GameOverlayRenderer64.dll`/`GameOverlayRenderer.dll` into every prefix
(`proton`, `proton_11.0`). It is **not a Windows-side overlay we have overlooked** — it is a
facade. Valve's ntdll special-cases its basename exactly as it does `steamclient64`
(`ValveSoftware/wine dlls/ntdll/loader.c`: the `use_lsteamclient()` block matches
`gameoverlayrenderer`/`gameoverlayrenderer64` and trampolines the module's exports into
`lsteamclient`), and those `lsteamclient` exports are **`@ stub`s**
(`lsteamclient/lsteamclient.spec`: `BOverlayNeedsPresent`, `IsOverlayEnabled`, `OverlayHookD3D3`,
… all `stub`). The *actual* Linux overlay is the native `gameoverlayrenderer.so`, delivered by
`LD_PRELOAD` from the Steam client — which Valve's own unix loader even special-cases to
*suppress* for `explorer.exe` (`dlls/ntdll/unix/loader.c`: "HACK: Unset LD_PRELOAD … to disable
buggy gameoverlayrenderer.so"). **[MEASURED]** So the closest worked example is structurally the
same as ours: load Valve's *native* renderer into the game process. It offers no new delivery
mechanism, and it confirms A's shape is the sanctioned one.

---

## 7. What is measured vs reasoned, and what should become an issue

**Measured:** §1 (the swizzle/installer structure and the `SteamNoOverlayUIDrawing` veto, from
disassembly); §2 (late trigger arms the overlay with `winemac` present — native and in-bottle,
each with its 0-hook control); §3 (driver shim delivers the overlay with no import rewrite); §4
(`AppInit_DLLs` dead three ways); §5's `WINEDLLOVERRIDES`-via-front-door and §6-E entitlement
facts; §6-F Proton facade.

**Reasoned:** A's fit into the DRM route (§2.1) — the mechanism is proven, the ~20-line wiring
into `shim_unix.cpp` is not yet built; input parity on the late path (§1.1) — argued from the
disassembly and the hotkey firing in run 79384, not measured channel-by-channel; anti-tamper
tolerance of A and B — reasoned from "no import rewrite, in-process, at runtime", **not** measured
against Space Marine 2's EasyAntiCheat.

**Should become issues:**

- **Build Route A into the shim** (the §2.1 change) and measure it end-to-end on Space Marine 2
  through Steam — the one test this research deliberately did not run. This also lets ADR 0014's
  "mutually exclusive / overlay off for wrapped titles" consequence be amended: the exclusivity is
  the injector's, not the overlay's.
- **Fold the corrected bail condition into ADR 0003 / `overlay-injection.md`.** The deadline is a
  missed method call, not a property of `winemac`; "our unixlib can dlopen the renderer itself" is
  no longer a dead wrong turn once the installer is called.
- **Route B as a general (non-DRM) fallback** for any title where import[0] is contested (e.g.
  the same anti-tamper titles the injector loses), tracked separately.
