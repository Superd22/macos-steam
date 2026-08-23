# Can the Steam in-game overlay work over the bridge? — a feasibility study

**Scope:** whether an *in-game composited* Steam overlay is achievable for a Windows title running
in the CrossOver **bottle**, driven by the **native macOS Steam client** through our **bridge**, with
**no Windows Steam**. Covers Valve's macOS overlay binaries, Proton's Linux arrangement, CrossOver's
present chain, macOS code-injection policy, and four candidate architectures. Overlay was scoped out
in `macossteamplayresearch.md` §7 step 5; this document establishes what scoping it back in costs.

**Investigated:** 2026-08-23.

**Primary sources.** All macOS binary citations are against the live install at
`~/Library/Application Support/Steam/Steam.AppBundle/Steam/Contents/MacOS/`:
`gameoverlayrenderer.dylib` (497 216 B, mtime 2026-08-03 22:46, fat x86_64+arm64, signed
`Developer ID Application: Valve Corporation (MXGJJ98X76)`, timestamp 3 Aug 2026 21:40:43),
`gameoverlayrenderer32.dylib` (271 712 B, mtime 2021-07-29, i386+x86_64+arm64),
`gameoverlayui` (167 120 B) and `gameoverlayui.dylib` (6 732 128 B, both mtime 2026-08-03 22:46),
`steamloader.dylib` (49 KB, mtime 2021-07-29), `ipcserver` (169 120 B, fat x86_64+arm64) and
`steamclient.dylib` (54 224 592 B, mtime 2026-08-03 22:46). CrossOver citations are against
`~/Applications/CrossOver.app` **version 26.2** shipping **`wine-11.0-8723-g7e8a47752e3`** — note this
is *newer* than the CrossOver 25.1.1 / wine-10.0 that `crossover-bridge-surface.md` and
`lsteamclient-mechanics.md` were written against; the ground has moved. Repo context is
`CONTEXT.md`, ADR 0001/0002, `docs/research/lsteamclient-mechanics.md`,
`docs/research/crossover-bridge-surface.md`, `docs/research/compat-vdf-platform-override.md`,
`tools/shim/FINDINGS.md`, and issues #18 / #19. External primary sources: `ValveSoftware/Proton`
branches `proton_9.0` and `proton_11.0` and `ValveSoftware/wine` branch `proton_9.0` (fetched via
`raw.githubusercontent.com`); `apple-oss-distributions/dyld` tag **dyld-1378**; Apple's entitlement
and Hardened Runtime documentation; Valve's Steamworks partner documentation; and Valve's Steam
client release notes pulled from
`api.steampowered.com/ISteamNews/GetNewsForApp/v2/?appid=593110&count=200` (200 items,
2022-11-30 → 2026-08-17). Where the only available source is reverse-engineering or forum material it
is named as such and its credibility rated (§6.5, §2.1).

Every claim below is marked **[V]** VERIFIED (a command was run, a file was read — the command or
`file:line` is given) or **[I]** INFERRED (reasoned from verified facts — the basis is stated).

---

## Summary (read this first)

**Verdict: an in-game composited overlay is feasible, and considerably more feasible than this
project has assumed.** The pessimism recorded in `macossteamplayresearch.md` §3 — "cross-process
overlay rendering relies on Linux/X11-specific injection and compositing tricks that don't map to
macOS" — is **wrong on the facts as they stand in August 2026**. So is the "Metal/MoltenVK stack is a
red herring" note in `tools/shim/FINDINGS.md` ~line 95, *once overlay is in scope*: that note is
correct about what it observed (frameworks mapped, no thread running in them) and wrong as a
conclusion about the overlay, because Valve's own macOS overlay renderer is a Metal renderer.

The six findings that change the picture:

1. **Valve ships a maintained, arm64-native, Metal-capable macOS overlay renderer.** [V]
   `gameoverlayrenderer.dylib` was rebuilt on 2026-08-03, is fat x86_64+arm64, links
   `Metal.framework` **and** `OpenGL.framework`, and contains an Objective-C class `SteamMetalHook`
   plus a `CMetalRenderer`/`CMetalDeviceCache` with four pipeline states (`BGRA8`, `BGR10A2`,
   `RGBA16Float`, screenshot). It hooks Metal by **Objective-C swizzling** of Apple's own classes:
   the categories it defines are on `MTLCommandBuffer`, `CAMetalDrawable` and `MTLDrawable`, with
   methods `steamoverlay_presentDrawable:`, `steamoverlay_presentDrawable:atTime:`,
   `steamoverlay_presentDrawable:afterMinimumDuration:`, `steamoverlay_nextDrawable`,
   `steamoverlay_commit`, `steamoverlay_addScheduledHandler:` and `steammetalhook_init`. This is not
   a GL-era artifact.

2. **CrossOver's present chain bottoms out in exactly those Apple classes, in the same address
   space.** [V] `lib/wine/x86_64-unix/winemac.so` imports `_OBJC_CLASS_$_CAMetalLayer` and calls
   `nextDrawable`; `lib/dxmt/x86_64-unix/winemetal.so` (DXMT) calls `nextDrawable`,
   `presentDrawable:` and `commit`; the shipped `D3DMetal.framework` links `Metal.framework` and
   references `presentDrawable:`. Because Valve's hook swizzles *Apple's* classes and not Valve's own
   code, it is agnostic to whether the caller is a native Mac game, `winemac.so`, DXMT or D3DMetal.
   **The Linux analogy holds, and holds better than expected**: on Linux the native renderer hooks
   the host-native present that Wine bottoms out into; on macOS the host-native present is
   Objective-C, and Valve already hooks it.

3. **The same is true for input.** [V] The renderer defines `SteamOverrideNextEvent`,
   `SteamOverrideNSCursor` and `SteamOverrideMouse` with swizzled
   `steamhooked_nextEventMatchingMask:untilDate:inMode:dequeue:`, `steamhooked_mouseLocation`,
   `steamhooked_pressedMouseButtons` and `steamhooked_resetCursorRects`. `winemac.so` imports
   `_OBJC_CLASS_$_NSApplication` and `_OBJC_CLASS_$_NSCursor`, contains the string
   `nextEventMatchingMask:untilDate:inMode:dequeue:`, and imports
   `_CGWarpMouseCursorPosition` / `_CGAssociateMouseAndMouseCursorPosition` — all of which the
   renderer also hooks. Wine's macOS driver and Valve's overlay are aimed at the same AppKit seam.

4. **Valve's renderer loads and initialises inside a CrossOver `wineloader` process today.** [V]
   Demonstrated on this machine (§3.3): `DYLD_INSERT_LIBRARIES=<gameoverlayrenderer.dylib>` into an
   ad-hoc-re-signed copy of CrossOver's `wineloader` produces
   `GameID = 480, AppID = 480, OverlayGameID = 480, PID: 90470, tid 259 Executable: wineloader`
   in `/tmp/gameoverlayrenderer.90470.log`. The x86_64 slice runs under Rosetta 2 (the module list
   in an earlier run includes `/usr/lib/libRosetta.dylib`), which is exactly the condition inside the
   bottle — CrossOver 26.2's entire unix side is x86_64-only [V].

5. **Valve documents this exact arrangement, and names the two entitlements it needs.** [V]
   https://partner.steamgames.com/doc/store/application/platforms: macOS builds need
   `com.apple.security.cs.disable-library-validation` ("allows loading the Steamworks SDK library and
   **overlay library**") and `com.apple.security.cs.allow-dyld-environment-variables` ("**enables the
   overlay library to be injected into the game process**"). Steamworks also states the overlay
   "supports games that use DirectX 7 - 12, OpenGL, **Metal**, and Vulkan", and Valve's own client
   release notes carry a macOS entry dated **2026-05-05: "Improved performance of Steam Overlay in
   games using Metal."** The macOS Metal overlay is live, maintained code — not a vestige.

6. **The blocker is not graphics. It is one entitlement.** [V] CrossOver's `wineloader` is signed
   with the hardened runtime (`flags=0x10000(runtime)`) and carries
   `com.apple.security.cs.disable-library-validation` but **not**
   `com.apple.security.cs.allow-dyld-environment-variables`. Measured: `DYLD_INSERT_LIBRARIES` and
   `DYLD_PRINT_LIBRARIES` are **silently stripped** for the shipped `wineloader`, while a positive
   control (unsigned binary) honours them. Re-signing a *copy* of `wineloader` ad-hoc with that
   entitlement added restores insertion [V]. That copy is the crux of the whole design — and it is
   the thing that trades a supported CrossOver against an unnotarised binary we ship.

**What that adds up to:** the overlay problem reduces to (a) getting one Valve dylib into the game's
Wine process at the right moment, (b) getting the native client to spawn `gameoverlayui` for that
process, and (c) accepting a defined degradation where dyld *interposition* (as opposed to ObjC
swizzling) cannot be recovered. None of it requires reverse-engineering Valve's overlay IPC, writing
a Metal renderer, or reimplementing the overlay UI. That is a fundamentally different — and much
smaller — problem than the one this project scoped out.

**The one unverified link.** Loading the renderer is proven; *arming* it is not. [V] With the
renderer inserted into a purpose-built Metal program that runs real `nextDrawable` →
`presentDrawable:` → `commit` cycles, it logs its header and **never logs `Hooking …`**. So the hook
is gated on something other than the graphics API — almost certainly the client-side handshake
(`SteamOverlayRunning_%llu`, `GameOverlayRender_PIDStream`), which means it cannot be demonstrated
without the native client agreeing to start an overlay instance for a Windows-platform app (§7 S-1,
S-3). That agreement is the single thing that could still sink architecture (a1), and it is an
experiment, not a research question.

**The cheapest move nobody has made yet.** `com.apple.security.cs.allow-dyld-environment-variables`
is a one-line change in CodeWeavers' build, it is the entitlement Valve's own documentation says the
overlay requires, and CrossOver already ships the harder-to-justify `disable-library-validation`
alongside it. **[I]** Asking CodeWeavers to add it would remove the re-signed-loader hack, the
notarisation problem and the licensing question in a single stroke. (Basis: §3.1's entitlement dump
and §6.3's Valve citation.) That should be tried before any of §5(a1)'s engineering.

---

## 1. Anatomy of the macOS overlay, from the binaries

### 1.1 The two halves and the stream between them

The overlay is two processes, exactly as on Windows. **[V]**

- **In-process renderer** — `gameoverlayrenderer.dylib`, loaded into the *game's* address space. It
  is a replay engine, not a UI. It contains no widgets; it consumes a serialised draw-command stream
  and issues Metal or GL draw calls into the frame the game was about to present.
- **Out-of-process UI** — `gameoverlayui` (a real executable) plus `gameoverlayui.dylib`, launched
  once per game process. It is VGUI-based (`otool -arch arm64 -L gameoverlayui` → `@loader_path/vgui2_s.dylib`)
  and takes CEF-rendered chrome from the Steam client as texture buffers rather than embedding CEF
  itself (`otool -L gameoverlayui.dylib` shows no CEF/Chromium link; the renderer carries the string
  `/tmp/steam_chrome_overlay_uid%d_spid%u`, and the client ships `chromehtml.dylib` separately).

The draw-command opcodes are legible directly in the renderer's error strings **[V]** —
`strings -a gameoverlayrenderer.dylib | grep 'Corrupt render stream'` yields
`k_EBeginFrame`, `k_ELoadTexture`, `k_EDeleteTexture`, `k_EDrawTexturedRect`,
`k_EDrawAndUpdateSharedTexture`, `k_EDrawChromePaintBufferRect`, `k_EDeleteChromePaintBuffer`,
`k_ESetCursor`, `k_ECreateCustomCursor`, `k_EDeleteCursor`, `k_EShowCursor`, `k_ESetHotKey`,
`k_ESendTextToGame`, `k_EIMECommand`, `k_EOverlayForceDisplayScale`. Alongside them:
`Left render loop without EndFrame!`, `Unsupported render command (%d) in vgui render stream`.

**[I]** The overlay's private protocol is therefore a *VGUI draw list*, not a pixel blit and not a
window composite. All layout, text shaping, CEF rasterisation and interaction logic live in
`gameoverlayui` and the Steam client; the in-process half is deliberately thin. This is the single
most important architectural fact for us: **reusing the renderer means we never have to understand
the protocol** — and conversely, *writing* a renderer means implementing that entire opcode set
against an undocumented, unversioned stream. (Basis: the opcode names, the "vgui render stream"
wording, and the absence of any UI toolkit in the renderer's link list.)

### 1.2 The transport

**[V]** POSIX shared memory + POSIX named semaphores + a Mach service, from
`nm -arch arm64 -u gameoverlayrenderer.dylib`: `_shm_open`, `_shm_unlink`, `_mmap`, `_ftruncate`,
`_sem_open`, `_sem_wait`, `_sem_trywait`, `_sem_post`, `_sem_close`, `_sem_unlink`, `_mach_msg`,
`_mach_port_allocate`, `_mach_port_deallocate`, `_bootstrap_look_up`, `_bootstrap_port`, `_socket`,
`_connect`. Valve's own type names appear in the RTTI strings: `N3IPC17PosixSharedMemoryE`,
`N3IPC10PosixEventE`, `N3IPC10PosixMutexE`, `N3IPC15BinarySemaphoreE`, `N3IPC10ISharedMemE`.

The named objects **[V]**:

```
GameOverlayRender_PIDStream
GameOverlayRender_PaintCmdStream_%d      GameOverlay_InputEventStream_%d
GameOverlayRender_SharedTex_%d_%d        GameOverlay_VGUIPaintingCompleted_%d
GameOverlayRender_SharedTexRead_%d_%d    GameOverlay_InGameRenderingCompleted_%d
GameOverlayRender_SharedTexWrite_%d_%d   GameOverlay_SerializedWorkQueued_%d
GameOverlay_ScreenshotStream_%d          GameOverlay_GameExitingEvent_%d
GameOverlay_AudioStream_%u               GameOverlay_MovieStream_%u
SteamOverlayRunning_%llu
```

`gameoverlayui.dylib` carries the same names with `%s` in place of `%d` **[V]** — the two halves
agree, keyed on the *game process's PID*. The Mach service is `com.valvesoftware.steam.ipctool`,
which is also the only interesting string in the separate `ipcserver` binary **[V]** — and the
renderer's log strings `Waiting on shared FD for texture %d: peer is too far ahead…` /
`…received an outdated FD for texture %d instead, skipping` show file descriptors being passed, which
is what the Mach channel is for. **[I]** `ipcserver` is the broker that lets two unrelated processes
exchange shm/IOSurface descriptors; the renderer looks it up with `bootstrap_look_up` rather than
inheriting anything. (Basis: `bootstrap_look_up` + `mach_msg` imports, the FD-passing log strings,
and `ipcserver`'s single service name.)

Texture sharing is `IOSurface`-based **[V]**: `_IOSurfaceLookup`, `_IOSurfaceGetWidth/Height`,
`newTextureWithDescriptor:iosurface:plane:`, `_CGLTexImageIOSurface2D`, and the error
`CMetalRenderer::ValveDrawSharedHandleTexture() called with bad IOSurfaceID`.

### 1.3 Who starts what

**[V]** `strings -a steamclient.dylib` contains `%s/gameoverlayui`, `STEAM_DYLD_INSERT_LIBRARIES`,
`SteamOverlayGameId`, `gameoverlayrenderer.dylib`, `GameOverlayRender_PIDStream`,
`GameOverlay: started '%s' (pid %d) for game process %d`, `GameOverlay: failed to execute process '%s': (%s)`,
`GameOverlay: chdir failed, errno %d`, `CSteamEngine::CheckForOverlayInstancesNeeded`,
`CUser::BSetOverlayPIDForGame: passed own procID as game procID`, and the app-config keys
`DisableOverlay`, `DisableOverlay_OSX`, `DisableOverlayInjection`, `allow_overlay`,
`GameOverlay_CompatMode`, `GameOverlay_TestMode`, `OverlayWindowBlacklist`.
`gameoverlayui.dylib` accepts `-pid`, `-gameid`, `-steampid` **[V]**.

**[I]** The sequence is: the client sets `STEAM_DYLD_INSERT_LIBRARIES` and `SteamOverlayGameId` on
the launched process; a launcher promotes the former to `DYLD_INSERT_LIBRARIES`; the renderer's
constructor runs, reads `SteamOverlayGameId`, and announces itself on `GameOverlayRender_PIDStream`;
`CSteamEngine::CheckForOverlayInstancesNeeded` in the client picks that up and `fork`/`exec`s
`gameoverlayui -pid <gamepid> -gameid <gid> -steampid <clientpid>`; the two then rendezvous on the
PID-keyed shm streams. (Basis: the direction of the string ownership — `PIDStream` appears in the
renderer and the client but *not* in `gameoverlayui.dylib`; the `-pid`/`-steampid` argument names;
and the `started '%s' (pid %d) for game process %d` log format.) **This is the part of the design we
have not yet exercised and it is spike S-3.**

The renderer is **self-registering and client-driven** — **[I]** it does not need `steamclient` in
its own address space and it does not need the game to call any Steamworks method to appear.
(Basis: its only four exports are `IsOverlayEnabled`, `BOverlayNeedsPresent`,
`SetNotificationPosition`, `ValveHookScreenshots` [V, `dyld_info -exports`]; it links no Steam
library; and it initialises fully in a process containing nothing but libSystem and AppKit, §3.4.)
It does, however, need the *client* to have armed it: §7 S-1 measures that the Metal hooks do not
install on graphics activity alone, so "appears without the game's help" is true of loading and
**not yet demonstrated** of hooking.

### 1.4 What `SteamNoOverlayUIDrawing` and the two stale binaries mean

**[V]** `SteamNoOverlayUIDrawing` and `SteamNoOverlayUI` are read by the renderer and the client
respectively; the renderer logs `SteamNoOverlayUIDrawing was set`. Our launch path sets it to `1`
(`tools/compat-tool/steamclient-shim-launch.sh:155`, `tools/shim/run.sh:39`) together with
`SteamOverlayGameId=0` (`:156`, `:40`). Turning overlay on begins by removing those two lines.

**[V]** `steamloader.dylib` (2021) is a DRM shim, not overlay machinery: its only export is
`_stub_entry_SteamLoader` and its arm64 slice declares an install name of `drmstub.arm64`.
`gameoverlayrenderer32.dylib`'s arm64 slice likewise declares `overlaystub.arm64` and defines **zero**
Objective-C classes, while its i386 slice links OpenGL and only weakly links Metal.

**[I]** Valve deliberately ships inert arm64 stubs for the legacy 32-bit/DRM loaders so that a fat
load on Apple Silicon succeeds and does nothing, and maintains only the 64-bit renderer. The mtime
split (2026 renderer, 2021 loaders) is therefore *not* evidence of neglect of the overlay — it is
evidence of neglect of **32-bit and DRM-loader** paths specifically. The overlay itself is live code.
(Basis: the stub install names, the empty class list, and the 2026-08-03 rebuild + fresh Developer ID
timestamp on the 64-bit renderer, `gameoverlayui`, `gameoverlayui.dylib` and `steamclient.dylib`
alike.) **Consequence for us: 32-bit titles (issue #20's Among Us path) get no overlay, ever.**

---

## 2. Our rendering path, and where the seam is

Issue #19 established the launch path renders **D3D12 → D3DMetal → Metal** inside the bottle, and
that going through CrossOver's front door (`wine --bottle`) is what gets D3DMetal rather than
MoltenVK. What that path actually consists of, in CrossOver 26.2 **[V]**:

| Component | Path | Arch |
|---|---|---|
| Wine mac driver | `lib/wine/x86_64-unix/winemac.so` | x86_64 |
| Wine Vulkan | `lib/wine/x86_64-unix/winevulkan.so` | x86_64 |
| DXMT (D3D11→Metal) | `lib/dxmt/{x86_64-unix/winemetal.so, x86_64-windows/winemetal.dll}` | x86_64 |
| D3DMetal (GPTK) | `lib64/apple_gptk/external/D3DMetal.framework` | x86_64 |
| Wine loader | `bin/wineloader` | x86_64 |

`lib/wine/` contains only `i386-windows`, `x86_64-unix`, `x86_64-windows` — **there is no arm64 unix
side** [V, `ls`]. The whole bottle runs under Rosetta 2.

Two facts make the seam concrete:

- **[V]** `nm -u lib/wine/x86_64-unix/winemac.so` imports `_OBJC_CLASS_$_CAMetalLayer`,
  `_OBJC_CLASS_$_NSApplication`, `_OBJC_CLASS_$_NSCursor`, `_CGWarpMouseCursorPosition`,
  `_CGAssociateMouseAndMouseCursorPosition`; its strings include `nextDrawable`, `mouseLocation`,
  `nextEventMatchingMask:untilDate:inMode:dequeue:`.
- **[V]** `strings lib/dxmt/x86_64-unix/winemetal.so` contains `nextDrawable`, `presentDrawable:`,
  `commit`; CrossOver's `D3DMetal` is a thin front for `/System/Library/Frameworks/D3DMetal.framework`
  (it links that path directly) and contains `presentDrawable:`.

**[I] D3DMetal being closed-source and proprietary does not matter.** The seam is not inside
D3DMetal; it is the Apple-owned Objective-C classes D3DMetal *calls*. Swizzling
`-[CAMetalLayer nextDrawable]` and `-[MTLCommandBuffer presentDrawable:]` intercepts every client of
Metal in the process, including closed-source ones, because Objective-C dispatch is a runtime
lookup that no caller can bypass without dropping to the private C ABI. (Basis: `objc_msgSend`
semantics; Valve's own hook is built on precisely this assumption for native Mac games;
`class_replaceMethod`/`class_getInstanceMethod`/`class_addMethod` are in the renderer's import list
[V].) The residual risk is that D3DMetal or DXMT caches an IMP or uses `objc_msgSendSuper` on a
private subclass, which the S-4 spike settles empirically rather than by argument.

The `winemetal.so`/`winemetal.dll` pair is worth noting for its own sake: **[I]** it is a Wine
**unixlib** exposing Metal to PE code over the same `__wine_unix_call` **seam** our **shim** uses
(ADR 0001) — i.e. CodeWeavers solved the same transport problem the same way. That makes it a
plausible alternate hook point if the ObjC route fails, but it only covers the DXMT/D3D11 path, not
D3DMetal/D3D12. (Basis: filename convention and `otool -L` showing `@rpath/ntdll.so`,
`@rpath/winemac.so` [V].)

### 2.1 One correction that matters for the hook

**[V]** `MTLCommandBuffer` and `MTLDrawable` are Objective-C **protocols**, not classes; the live
objects are private concrete classes (on Apple Silicon here, `AGXG15XFamilyCommandBuffer`), and
`presentDrawable:` resolves on *that* class. `CAMetalLayer`, by contrast, is a real public class and
`nextDrawable` is a real method on it (type encoding `@16@0:8`), verified by runtime introspection.

Valve's renderer is built for exactly this. Its `__objc_classname` section names `MTLCommandBuffer`,
`CAMetalDrawable`, `MTLDrawable`, `NSObject` as *category targets* **[V]**, and its imports include
`_class_getInstanceMethod`, `_class_getSuperclass`, `_class_copyMethodList`, `_class_replaceMethod`,
`_class_addMethod`, `_class_getName`, `_NSClassFromString`, `_NSSelectorFromString` **[V]** — the
toolkit for walking from a protocol name to the concrete class at runtime and patching it. Its log
format is `Hooking %s::%s for %s` / `Failed to hook %s::%s for %s` **[V]**, i.e. it reports
`class::selector` and expects to sometimes fail.

**[I]** So the fragile part of the hook is Valve's problem, already solved by Valve, and re-solved
every time Apple renames an `AGX…` class — which is a further reason to reuse their binary rather
than write our own (§5(c)). (Basis: the import list and the class/category names; not disassembled.)

**Precedent that the seam is real under Wine on macOS [V]:** Apple's own Metal Performance HUD
(`MTL_HUD_ENABLED=1`) draws over CrossOver- and Whisky-hosted Windows games. Something composites into
the same present chain we are targeting, today, in this exact stack. Conversely, DXVK's built-in
`DXVK_HUD` works because it draws *inside* DXVK before MoltenVK presents — which is why it is
irrelevant to us: issue #19 put us on D3DMetal, not DXVK.

**Counter-evidence, and it is the honest sort [V]:** the Whisky maintainers' own discussion
(github.com/orgs/Whisky-App/discussions/677) reports the Steam overlay *not* working under
Whisky — "It sure brings up diagnostic information, but no Steam Overlay." **[I]** That is the
*Windows* overlay (`GameOverlayRenderer64.dll`, from a Windows Steam running inside the bottle)
failing to hook D3DMetal/DXMT — i.e. it is evidence against architecture **(b)**, not against
(a1)/(a2), which use the macOS-native renderer that no one in that thread was running. Nobody appears
to have tried what §5(a1) proposes. (Basis: the thread describes a Windows-Steam-in-bottle setup;
CrossOver forum reports likewise say the overlay "only works when DXVK is enabled", consistent with
a PE-side D3D hook that D3DMetal bypasses.)


---

## 3. macOS injection policy, measured on this machine

This is the section where the answer had to be obtained by experiment rather than by reading, because
the outcome depends on the exact entitlements CodeWeavers ships.

### 3.1 CrossOver's entitlements

**[V]** `codesign -d --entitlements - --xml`:

- `CrossOver.app/Contents/MacOS/CrossOver` — `flags=0x10000(runtime)`, notarisation ticket stapled,
  `Developer ID Application: CodeWeavers Inc. (9C6B7X7Z8E)`. Entitlements: `automation.apple-events`,
  `cs.allow-unsigned-executable-memory`, `device.audio-input`, `device.camera`.
- `SharedSupport/CrossOver/bin/wineloader` — `flags=0x10000(runtime)`,
  identifier `com.codeweavers.CrossOver.wineloader`. Entitlements:
  `cs.allow-unsigned-executable-memory`, `cs.disable-executable-page-protection`,
  **`cs.disable-library-validation`**, `device.audio-input`, `device.camera`.

**`cs.disable-library-validation` is present and is load-bearing** — it is what permits a
Valve-signed (team `MXGJJ98X76`) dylib to be loaded into a CodeWeavers-signed (team `9C6B7X7Z8E`)
process at all. **`cs.allow-dyld-environment-variables` is absent.**

### 3.2 `DYLD_INSERT_LIBRARIES` is stripped for the shipped `wineloader` — measured

**[V]** Commands and outputs:

```
$ CO=~/Applications/CrossOver.app/Contents/SharedSupport/CrossOver
$ "$CO/bin/wineloader" --version                                   # control
wine-11.0-8723-g7e8a47752e3
$ DYLD_INSERT_LIBRARIES=/tmp/definitely-not-here.dylib "$CO/bin/wineloader" --version
wine-11.0-8723-g7e8a47752e3                                        # no dyld abort → var ignored
$ DYLD_PRINT_LIBRARIES=1 "$CO/bin/wineloader" --version
wine-11.0-8723-g7e8a47752e3                                        # no library trace → var ignored
$ DYLD_INSERT_LIBRARIES=$PWD/ins.dylib ./plainmain                 # positive control, unsigned
INSERTED-OK
$ DYLD_INSERT_LIBRARIES=$PWD/ins.dylib "$CO/bin/wineloader" --version
wine-11.0-8723-g7e8a47752e3                                        # constructor never ran
```

(`ins.dylib` is a one-line constructor printing `INSERTED-OK`; `plainmain` an empty `main`.)
The nonexistent-dylib case is the sharpest oracle: dyld resolves inserted images *before* `main`, so
an honoured `DYLD_INSERT_LIBRARIES` pointing at a missing file aborts the process. It did not.

### 3.3 An ad-hoc re-signed copy of `wineloader` restores insertion — measured

**[V]**:

```
$ cp "$CO/bin/wineloader" ./wineloader-copy
$ codesign --force --sign - --options runtime --entitlements ent.plist ./wineloader-copy
$ codesign -dv ./wineloader-copy   # → flags=0x10002(adhoc,runtime), +allow-dyld-environment-variables
$ DYLD_INSERT_LIBRARIES=$PWD/ins.dylib ./wineloader-copy --version
INSERTED-OK
wine: could not load ntdll.so: …/scratchpad/../lib/wine/x86_64-unix/ntdll.so (no such file)
```

`ent.plist` is CodeWeavers' four entitlements plus `com.apple.security.cs.allow-dyld-environment-variables`.
The `ntdll.so` failure is only the copy being outside CrossOver's tree — wine resolves it relative to
the loader. Placing the copy in a mirror root fixes it **[V]**:

```
$ mkdir -p fakeroot/bin fakeroot/lib && cp wineloader-copy fakeroot/bin/wineloader
$ codesign --force --sign - --options runtime --entitlements ent.plist fakeroot/bin/wineloader
$ ln -s "$CO/lib/wine" fakeroot/lib/wine ; ln -s "$CO/lib64" fakeroot/lib64
$ DYLD_INSERT_LIBRARIES=$PWD/ins.dylib fakeroot/bin/wineloader --version
INSERTED-OK
wine-11.0-8723-g7e8a47752e3
```

**[I]** A shipped product can therefore carry a mirror root — one real re-signed `wineloader`, the
rest symlinks into `CrossOver.app` — without writing inside the app bundle, which
`crossover-bridge-surface.md` §6 forbids. The costs are real and named in §5. (Basis: the run above;
plus ADR 0002's shipping vehicle already being our own `.app`.)

**Front-door conflict, unresolved [V]:** CrossOver's `bin/wine` perl launcher sets
`$ENV{WINELOADER}=cxwhich($bin_path,"wineloader")` at line 713 and restores it from `CX_WINELOADER`
at line 806, i.e. it **overwrites** any inherited `WINELOADER`. Our compat tool launches through that
front door on purpose (`steamclient-shim-launch.sh:181`, per issue #19). **[I]** Redirecting it means
making `cxwhich` find our loader — most plausibly by pointing `CX_ROOT` at the mirror root — not by
setting `WINELOADER`. Untested; spike S-2. (Basis: reading `bin/wine` lines 528, 713, 806, 1049,
1068, 1245.)

### 3.4 `dlopen` is a second route in — with a precisely-known hole

Our **unixlib** (`shim_unix.so`) is already resident in the game's Wine process. It can `dlopen` the
renderer with no environment variable at all.

**[V]** `dlopen` of Valve's renderer from an ordinary unsigned x86_64 process succeeds and the
library initialises:

```
$ ./dl "…/gameoverlayrenderer.dylib"
DLOPEN OK 0xe8d20
  IsOverlayEnabled = 0x10d3ea354      BOverlayNeedsPresent = 0x10d3ea03c
  SetNotificationPosition = 0x10d3e9b19  ValveHookScreenshots = 0x10d3e40f6
$ head -1 /tmp/gameoverlayrenderer.86710.log
… GameID = 0, AppID = 0, OverlayGameID = 0, PID: 86710, tid 259 Executable: dl
```

**But `dlopen` loses dyld interposition.** The renderer's `__DATA,__interpose` section holds **15**
entries **[V]**, `dyld_info -arch arm64 -fixups`:

```
OpenGL/_CGLChoosePixelFormat      OpenGL/_CGLFlushDrawable      OpenGL/_glSwapAPPLE
ApplicationServices/_HideCursor   ApplicationServices/_InitCursor  ApplicationServices/_ShowCursor
CoreFoundation/_CFRunLoopRun      CoreFoundation/_CFRunLoopRunInMode
CoreGraphics/_CGAssociateMouseAndMouseCursorPosition   CoreGraphics/_CGCursorIsVisible
CoreGraphics/_CGDisplayHideCursor CoreGraphics/_CGDisplayShowCursor
CoreGraphics/_CGDisplayMoveCursorToPoint  CoreGraphics/_CGGetLastMouseDelta
CoreGraphics/_CGWarpMouseCursorPosition
```

Measured, with a purpose-built triple (`libtarget.dylib` exporting `target()`, `libearly2.dylib`
linked against it and loaded at launch, `libinterpose2.dylib` interposing `target`) **[V]**:

```
$ ./drv2 $PWD/libinterpose2.dylib          # dlopen'd LATE
interposer2 loaded
early lib: target()=1                      # ← NOT interposed
$ DYLD_INSERT_LIBRARIES=$PWD/libinterpose2.dylib ./drv2 …   # inserted EARLY
interposer2 loaded
early lib: target()=4242                   # ← interposed
```

**[I]** Therefore the `dlopen` route gets everything done by Objective-C swizzling — the Metal present
hook, the AppKit event hook, the `NSCursor` overrides, which is the *majority* of the overlay — and
loses all 15 C-function interposes, because `winemac.so` is loaded long before our unixlib.
Practically that means: mouse capture/release and cursor visibility handoff between game and overlay
would be broken or partial, the CGL/`glSwapAPPLE` GL present path is lost (irrelevant — the bottle is
Metal), and `CFRunLoopRun` interception is lost. (Basis: the measurement above plus the mapping of
each interposed symbol to `winemac.so`'s import list [V].) **This is the precise cost of the
no-re-signing option** and it is what makes §5(a2) a degraded rather than an equivalent architecture.

### 3.5 Why the measurements came out that way — the platform rules behind them

The experiments in §3.2–§3.4 are the authority for this document; these citations explain them.

**[V]** Apple, `com.apple.security.cs.allow-dyld-environment-variables`
(https://developer.apple.com/documentation/bundleresources/entitlements/com.apple.security.cs.allow-dyld-environment-variables):

> A Boolean value that indicates whether the app may be affected by dynamic linker environment
> variables, which you can use to inject code into your app's process. … This causes the macOS dynamic
> linker (dyld) to read from environment variables that begin with `DYLD_`. … Injecting libraries or
> changing search paths with this feature may still require another entitlement. For example, you also
> need the `disable-library-validation` entitlement if an injected library isn't signed with the
> expected team ID.

**[V]** Apple, `com.apple.security.cs.disable-library-validation`:

> The Hardened Runtime enables library validation by default. This security-hardening feature prevents
> a program from loading frameworks, plug-ins, or libraries unless they're either signed by Apple or
> signed with the same Team ID as the main executable.

**[V]** Apple, Hardened Runtime (https://developer.apple.com/documentation/security/hardened-runtime):
"You add entitlements only to executables. Shared libraries, frameworks, and in-process plug-ins
inherit the entitlements of their host executable." — i.e. the entitlement must be on `wineloader`
itself; nothing we ship alongside can supply it.

**[V]** In dyld's own source (`apple-oss-distributions/dyld`, tag `dyld-1378`), the decision is not
dyld's: `DyldProcessConfig.cpp:964` calls `amfi_check_dyld_policy_self()` and keys on
`AMFI_DYLD_OUTPUT_ALLOW_PATH_VARS` / `…_ALLOW_LIBRARY_INTERPOSING` / `…_ALLOW_EMBEDDED_VARS`; the
"restricted" input is `UnsafeHeader::isRestricted()` = `hasSection("__RESTRICT","__restrict")`
(`mach_o/UnsafeHeader.cpp:2780`). Interposing is gated on the same flag set —
`RuntimeState::buildInterposingTables()` (`DyldRuntimeState.cpp:1298`) opens with
`if ( !config.security.allowInterposing ) return;`. The entitlement→flag mapping itself is inside
AMFI, which is closed source; that step is **[I]**, inferred from the measured behaviour in §3.2/§3.3.

Two corroborations worth having on record:

- **[V]** Independent replication on a second test harness confirmed the full truth table: ad-hoc
  signed with no hardened runtime → insertion works; hardened runtime with no entitlements → does not;
  hardened runtime + `allow-dyld-environment-variables` + `disable-library-validation` → works;
  hardened runtime + `disable-library-validation` alone → does **not**. This is exactly CrossOver's
  configuration, and exactly what §3.2 measured.
- **[V]** dyld's interposition *does* reach dyld-shared-cache framework symbols when the library is
  inserted at launch (demonstrated by replacing `MTLCreateSystemDefaultDevice`, with dyld logging
  `dyld: … has interposed '_MTLCreateSystemDefaultDevice' … replacing binds`), and does **not** when
  the library arrives by `dlopen` — the §3.4 result, from the other direction. dyld's own note is that
  interposing "is less safe … because the running program may have already copied the pointer values".

**[V] The stock `Steam.app` is itself hardened with no `com.apple.security.cs.*` entitlements at all**
(`codesign -d --entitlements`, team `MXGJJ98X76`). **[I]** That is not a problem for us — we never
inject into Steam; it is worth recording because it forecloses any architecture that would have tried
to. (Basis: the entitlement dump; all four architectures in §5 inject into the Wine process only.)


---

## 4. Input: who owns the keyboard when a Wine window is focused

Proton's PE-side contribution to the overlay is one hack, and only one: in the callback pump,
callback `0x14b` (`GameOverlayActivated_t`) triggers synthetic `keybd_event` key-ups for Shift and
Tab and swallows spurious deactivations (`lsteamclient/steamclient_main.c:528-550`, recorded in
`lsteamclient-mechanics.md` §6.1). **[I]** That is a *repair*, not a mechanism: the native renderer
has already eaten the real Shift+Tab at the host level, so the PE side is left holding two keys it
believes are still down, and Proton fakes the key-ups. It tells us nothing about how the overlay is
opened — it tells us the overlay is opened *below* the PE layer. `lsteamclient-mechanics.md` §9
already lists this hack under "skip entirely"; if overlay comes into scope, **it comes back**, and
its Wine-on-macOS equivalent is `__wine_send_input`/`NtUserSendInput` rather than `keybd_event`
(Wine's `winemac.so` is the injector). Small, well-understood, roughly a day — **but see §6.2, which
shows the hack is one end of a process-wide input gate that Valve's Wine fork implements in four
other DLLs and CrossOver's Wine does not.** That is the part of overlay input parity we cannot buy
off the shelf.

On the macOS side the hotkey is captured by the swizzled AppKit event pump **[V]**: the renderer's
`SteamOverrideNextEvent` class defines
`steamhooked_nextEventMatchingMask:untilDate:inMode:dequeue:`, and its imports include
`_CGEventCreateKeyboardEvent`, `_CGEventPost`, `_CGEventGetFlags`, `_CGEventSetFlags`,
`_CGEventKeyboardGetUnicodeString`, `_CGEventKeyboardSetUnicodeString`,
`+[NSEvent eventWithCGEvent:]`, `-[NSApplication postEvent:atStart:]` and
`+[NSEvent addLocalMonitorForEventsMatchingMask:handler:]`. Its logs include
`Detected hot-key via base input, now requesting overlay %s` and
`ESC press detected, requesting overlay disable` **[V]**.

**[I] The ownership question answers itself favourably.** When a bottle window is focused, the
process that owns the keyboard *is* the Wine process — `winemac.so` runs a real `NSApplication` and
pulls from `nextEventMatchingMask:` [V, string present]. Valve's hook sits on that same method in that
same process, upstream of `winemac.so`'s translation to Windows messages. So Shift+Tab is swallowed
before Wine ever sees it, and text typed into the overlay never reaches the game. (Basis: the two
string/import sets above and the fact that ObjC swizzling replaces the IMP process-wide.) The risk is
not "can it intercept" but *ordering* — whether Valve's swizzle installs before or after `winemac.so`
caches anything, which is again a matter for a spike, not an argument.

Cursor handoff is the weak spot, and it is weak for exactly the reason §3.4 gives: `NSCursor` is
swizzled (recoverable via `dlopen`) but `CGWarpMouseCursorPosition`,
`CGAssociateMouseAndMouseCursorPosition`, `CGDisplayHideCursor`/`ShowCursor` and
`CGGetLastMouseDelta` are **interposed** (not recoverable via `dlopen`) — and `winemac.so` imports
two of those five directly **[V]**. A game in relative-mouse mode would keep warping the cursor while
the overlay is up.

---

## 5. Architectures, ranked

Effort figures assume one engineer already fluent in this codebase, and count *to a working overlay
on one title*, not to a shippable feature.

### (a1) Reuse Valve's `gameoverlayrenderer.dylib`, inserted at launch via a re-signed loader — **RECOMMENDED**

**Verdict: feasible. Highest fidelity, and the only option that reaches parity with native macOS
Steam games.** This is architecture (a) from the brief, in its full-fidelity form.

What has to be built — **after** trying step 0, which is not engineering at all:

0. **Ask CodeWeavers to add `com.apple.security.cs.allow-dyld-environment-variables` to
   `wineloader`.** It is the entitlement Valve's own documentation names as the requirement for the
   macOS overlay (§6.3), CrossOver already ships its harder-to-justify companion, and granting it
   deletes steps 1 and 2 below along with the notarisation and licensing problems. **[I]** A vendor
   whose product is Windows games on macOS has an obvious interest in the Steam overlay working.
   (Basis: §3.1's entitlement dump and the Apple docs in §3.5.)
1. A **mirror root** shipped inside our `.app` (ADR 0002): `bin/wineloader` = a copy of CrossOver's,
   ad-hoc re-signed with CodeWeavers' four entitlements **plus**
   `com.apple.security.cs.allow-dyld-environment-variables`; `lib/`, `lib64/`, `share/` symlinked
   into `CrossOver.app`. Proven to run (§3.3).
2. Make CrossOver's front door use it (`CX_ROOT` redirect, or a supported CodeWeavers mechanism if
   one exists) — **spike S-2**, and the one place this can still go wrong architecturally.
3. In the compat tool: stop setting `SteamNoOverlayUIDrawing=1` and `SteamOverlayGameId=0`; promote
   the `STEAM_DYLD_INSERT_LIBRARIES` we already receive from Steam
   (`compat-vdf-platform-override.md` ~line 253 — it already names
   `…/steamloader.dylib:…/gameoverlayrenderer.dylib`) into `DYLD_INSERT_LIBRARIES`, dropping the
   `steamloader.dylib` element (§1.4: DRM stub, inert).
4. Make the native client actually spawn `gameoverlayui` for the title — **spike S-3**.
5. Restore the Proton `GameOverlayActivated_t` key-up repair on the PE side of the **shim** (§4).

Effort: **2–4 weeks** if S-2/S-3/S-4 pass; the code is small, the integration surface is not.
Risk of turning out impossible mid-way: **low-to-moderate**. The graphics half is de-risked by §1–§3.
The two live risks are S-3 (client refuses to start an overlay instance for a Windows-platform app —
if it hard-refuses on a code path with no env override, this option degrades to (c)) and the
distribution problem in §5's caveat below.

**The caveat that must be stated plainly.** An ad-hoc-signed `wineloader` copy cannot be notarised
under our Developer ID *as CodeWeavers' binary* without re-signing their code, which is legally and
practically a different conversation; it will trip Gatekeeper for downloaded builds unless we sign it
with our own Developer ID (permitted — it is a copy we distribute, not a modification in place — but
it does mean shipping a derivative of CodeWeavers' binary, which is a licensing question, not a
technical one). It also breaks on every CrossOver update: the copy must be re-made and re-signed at
install/upgrade time, and `crossover-bridge-surface.md` §6 already prescribes exactly that
idempotent-reapply discipline for our other bottle edits.

### (a2) Reuse Valve's renderer, `dlopen`'d from our unixlib — **the fallback with a known ceiling**

**Verdict: feasible, degraded, and cheap.** No re-signing, no mirror root, no CrossOver-update
fragility, no licensing question. `shim_unix.so` calls `dlopen("…/gameoverlayrenderer.dylib")` after
setting `SteamOverlayGameId`, at `SteamAPI_Init` time.

What is lost is exactly the 15 dyld interposes (§3.4): cursor capture/release, mouse warping,
`CGGetLastMouseDelta`, `CFRunLoopRun`. **[I]** Expect: overlay draws correctly, opens and closes
correctly, mouse *position* is wrong or the game keeps stealing the cursor in relative-mouse titles.
For a menu/store/friends-list overlay in a windowed or cursor-visible title this may be entirely
acceptable; for a shooter it is not.

Effort: **3–7 days** to a first draw. Risk of impossibility mid-way: **low** — every step of the
loading half is already measured. The risk is that it *works but feels broken*, which is worse than a
clean failure, so gate it behind S-4 and S-5 and be willing to throw it away.

**[I]** A hybrid is available and probably the right end state: `dlopen` the renderer *and*
additionally insert a tiny dylib of our own at launch — ours, so we can sign it — carrying only the
15 interpose entries and forwarding to the renderer's own hooks. This still needs the entitlement
(any insertion does), so it does not escape (a1)'s cost; noted only because it decouples "insert
Valve's 497 KB binary" from "insert 4 KB of ours".

### (b) Reuse the PE `GameOverlayRenderer64.dll` inside the bottle, extending the shim

**Verdict: infeasible in practice. Do not attempt.** The reference bottle does contain a real
`GameOverlayRenderer64.dll` (`crossover-bridge-surface.md` ~line 559), but:

- **[I]** It would hook the *PE-side* D3D12 present, i.e. above D3DMetal, and then need to composite
  through D3DMetal into Metal — adding a translation layer rather than bypassing one, and needing a
  D3D12 render implementation that works under D3DMetal's partial feature coverage. (Basis: it is the
  Windows renderer; its counterpart on Linux is *not* what Proton uses — see §1.1's opcode analysis
  and the fact that the client's own overlay path on both Linux and macOS is the native renderer.)
- It expects the private client-side shm/Mach protocol from a real `steamclient64.dll`. Reproducing
  that in our **shim** means reimplementing the entire undocumented opcode stream *and* the PID/texture
  handshake *and* the Windows-named-object semantics on top of POSIX primitives, all against a moving,
  unversioned target — every one of which (a1) obtains for free by using the binary Valve compiled
  against it.
- The clean-bottle rule (`CONTEXT.md`) exists precisely so no Valve Windows binary is in play.

Effort: **many months**, with a high chance of never converging. Risk of impossibility mid-way:
**high**. Listed for completeness and to close it.

### (c) Write our own renderer and UI, sourced from `SteamClient.Overlay`

**Verdict: feasible, expensive, and strictly worse than (a1) unless (a1)'s S-3 fails.**
We already inventoried the CEF surface (`steamclient-js-api-macos.md` ~line 216:
`GetOverlayBrowserInfo`, `RegisterForOverlayActivated`, `RegisterForActivateOverlayRequests`,
`SetOverlayState`, `HandleProtocolForOverlayBrowser`, …). This is the honest DIY path: swizzle
`-[MTLCommandBuffer presentDrawable:]` ourselves, blit a texture we own, and fill that texture from
the Steam client's own overlay browser.

**[I]** `GetOverlayBrowserInfo` returning a browser handle plus the client's existing CEF chrome
pipeline suggests the pixels *are* obtainable without reverse-engineering the VGUI stream — but
nothing verified here says how, and the notification/friends/achievement chrome that makes the
overlay useful is VGUI, not CEF, so a large part of it would have to be reimplemented as our own UI.
(Basis: the method inventory and §1.1's finding that `gameoverlayui` is VGUI-based with CEF as a
texture source.)

Effort: **2–4 months** for something that renders a browser panel on Shift+Tab; **6+ months** for
anything resembling the real overlay. Risk of impossibility mid-way: **moderate** — the compositing
half is genuinely de-risked (it is the same swizzle (a1) relies on), the content half is not.

**Its one advantage:** it does not depend on the native client agreeing to run an overlay instance for
a Windows-platform app, which is (a1)'s single biggest unknown.

### (d) Out-of-process only — route `ActivateGameOverlayTo*` to the native Steam window

**Verdict: feasible, days of work, and it is what should ship first regardless of what else happens.**
No compositing. `ISteamFriends::ActivateGameOverlayToStore/ToUser/ToWebPage/InviteDialog` and
`ISteamUtils::IsOverlayEnabled` are currently `vt_unmapped` stubs
(`tools/shim/shim_vtables.h:1878-1888`, `:1968-1972`, `:3564-3594`). Forwarding them to the native
`steamclient.dylib` — which implements every one of them **[V]**: `strings steamclient.dylib` shows
`ActivateGameOverlay`, `ActivateGameOverlayToUser`, `ActivateGameOverlayToStore`,
`ActivateGameOverlayToWebPage`, `ActivateGameOverlayInviteDialog`,
`ActivateGameOverlayInviteDialogConnectString`, `ActivateGameOverlayRemotePlayTogetherInviteDialog` —
makes "buy DLC", "view profile", "invite friend" open the **native side** Steam window instead of
faulting or silently doing nothing.

**[I]** `IsOverlayEnabled()` must stay `false` in this mode. Titles use it to decide whether to pause
on overlay activation and whether to offer in-game purchase flows; returning `true` with no compositor
produces a game that pauses forever waiting for an overlay that never appears. (Basis: the Steamworks
contract for the method; also why (d) is a genuine feature and not a half-measure — it is the honest
answer.)

Effort: **2–5 days**, following the existing four-file pattern from issue #18. Risk: **negligible**.

### (e) The architecture the list missed: the Vulkan-layer path — **not available here**

Worth recording because it is the obvious question. **[V]** `steamclient.dylib` contains
`ENABLE_VK_LAYER_VALVE_steam_overlay_1` and the convar description
`Prevents loading of Vulkan layer if overlay disabled` — i.e. Valve's *Vulkan implicit layer* overlay
exists in this very binary's shared codebase. **[I]** It is dead weight on macOS as our stack is
configured: issue #19's whole point was that the MoltenVK path is what we get when we *bypass*
CrossOver's front door and that it does not work, while D3DMetal — the path that does work — is not
Vulkan at all. A Vulkan-layer overlay would only apply to titles that go through `winevulkan.so` →
MoltenVK, and Valve ships no macOS build of that layer in this install (no `.json` manifest or layer
dylib is present in the Steam MacOS directory [V, `ls`]). Closed.

**Ranked: (d) now → (a1) as the real target → (a2) as its fallback → (c) only if S-3 kills (a1) →
(b) never.**

---

## 6. What Proton does, and what Valve says out loud

### 6.1 On Linux, nothing PE-side composites the overlay

**[V]** Proton's `proton` launch script copies Valve's prebuilt PE overlay DLLs into the prefix
(`proton_11.0:1091-1107`, identical at `proton_9.0:951-967`):

```python
filestocopy = [("steamclient.dll","steamclient.dll"), ("steamclient64.dll","steamclient64.dll"),
               ("GameOverlayRenderer64.dll","GameOverlayRenderer64.dll"), ("SteamService.exe","steam.exe"),
               ("Steam.dll","Steam.dll")]
for (src,tgt) in filestocopy:
    srcfile = steamdir + '/legacycompat/' + src
```

**[V]** But `GameOverlayRenderer*.dll` has **no source and no build rule anywhere in the Proton
tree** (a full recursive listing of `proton_11.0` — 3764 paths — grepped for `overlay`
case-insensitively yields only vestigial PS3 SDK headers and SteamVR `IVROverlay`), and **[V]** the
`proton` script contains **zero** references to `LD_PRELOAD`, `VK_LAYER`, `VK_INSTANCE_LAYERS`,
`ENABLE_VK_LAYER`, or `SteamNoOverlayUIDrawing`. Proton neither sets, strips, nor filters the
preload; it inherits what the Steam client exported.

**[V]** What the Steam client exports on Linux is `gameoverlayrenderer.so`, appended to `LD_PRELOAD`
(`ValveSoftware/steam-for-linux` issue #4630, Valve-triaged and assigned to Plagman — credibility
high for the mechanism, though the wording is the reporter's). And Valve ships a second, decisive
piece: an **implicit Vulkan layer**, `VK_LAYER_VALVE_steam_overlay_64`, manifest pointing at
`ubuntu12_64/steamoverlayvulkanlayer.so`, with
`enable_environment: {"ENABLE_VK_LAYER_VALVE_steam_overlay_1": "1"}`. **That exact env-var string is
present in the macOS `steamclient.dylib` on this machine** [V, §5(e)] — the two platforms share the
codebase. Valve's own tracker describes the layer hooking `vkQueuePresentKHR`
(`steam-for-linux` issue #9120, credibility high — a reproducible report naming the file and entry
point). The two native halves talk in-process: `gameoverlayrenderer.so` `dlsym`s
`VulkanSteamOverlayPresent` / `…ProcessCapturedFrame` / `…SetWindowType` out of the layer [V, shipped
binary strings via `SteamDatabase/SteamTracking`].

Corroborating from the other side **[V]**: `vkd3d-proton`'s `libs/d3d12core/main.c:335-341` loads
`winevulkan.dll` *in preference to* `vulkan-1.dll` with the comment *"in order to bypass issues with
third-party overlays hooking the Vulkan loader"* — i.e. the PE-side hook point is one they
deliberately route around, while the host-side layer still gets the present.

**So: on Linux the overlay is composited by host-native code in the Wine process's address space,
hooking the host-native present that DXVK/vkd3d bottom out into.** The macOS analogue of that is
hooking Metal. **The analogy in the brief holds exactly**, and §1–§3 of this document show Valve has
already written the macOS half.

### 6.2 `lsteamclient`'s contribution is input repair, and it is bigger than one hack

**[V]** `lsteamclient/steamclient_main.c:528-549` (proton_11.0):

```c
if (win_msg->m_iCallback == 0x14b) /* GameOverlayActivated_t::k_iCallback */
{
    uint8_t activated = *(uint8_t *)win_msg->m_pubParam;
    if (activated)
    {
        SetEvent( steam_overlay_event );
        keybd_event( VK_LSHIFT, 0x2a, KEYEVENTF_KEYUP, 0 );
        keybd_event( VK_RSHIFT, 0x36, KEYEVENTF_KEYUP, 0 );
        keybd_event( VK_TAB,    0x0f, KEYEVENTF_KEYUP, 0 );
    }
    else
    {
        if (WaitForSingleObject( steam_overlay_event, 0 ) == WAIT_TIMEOUT)
        { /* "Spurious steam overlay deactivate event, skipping." */ }
        ResetEvent( steam_overlay_event );
    }
}
```

The named event is created at `steamclient_main.c:63` as
`"__wine_steamclient_GameOverlayActivated"` — and **[V]** Valve's Wine fork consumes it in four
other places: `dlls/winex11.drv/x11drv_main.c:748-752` opens it by NT path; `dlls/dinput/dinput_main.c:496`,
`dlls/hidclass.sys/pnp.c` and `dlls/xinput1_3/main.c:865,885` gate input on it
(`if (WaitForSingleObject(steam_overlay_event,0) == WAIT_OBJECT_0) memset(state, 0, sizeof(*state));`).

**This corrects `lsteamclient-mechanics.md` §9**, which lists the hack under "skip entirely" as if it
were a single self-contained oddity. It is one end of a **process-wide input gate**: `lsteamclient`
*sets* the event, and Wine's X11 driver, dinput, hidclass and xinput all *read* it to mute input
while the overlay is up. Reproducing overlay behaviour faithfully means reproducing that gate, and
CrossOver's Wine is not Valve's fork — it will not have `steam_overlay_event` consumers. **[I]** Our
options are to accept that gamepad and dinput keep feeding the game while the overlay is open, or to
mute at the **shim** boundary, which we cannot do for input that never crosses it. This is a real,
previously-unrecorded cost and it belongs in any overlay estimate. (Basis: the four consumer sites
above are in `ValveSoftware/wine`, not upstream; upstream `wine-mirror/wine` carries only the
`main.c:853` XInput hot-patch comment.)

That comment is itself worth quoting, because it is Valve stating the Windows design in their own
source **[V]**, `dlls/xinput1_3/main.c:874-875`:

```c
/* Some versions of SteamOverlayRenderer hot-patch XInputGetStateEx() and call
 * XInputGetState() in the hook, so we need a wrapper. */
```

### 6.3 Valve documents the macOS overlay as a dyld injection — and names our two entitlements

This is the single most useful external citation in this document, because it independently
predicts the measurement in §3. From
https://partner.steamgames.com/doc/store/application/platforms, verbatim **[V]**:

> Support for 10.15 (Catalina) requires adding the following entitlements to your build configuration
> `com.apple.security.cs.disable-library-validation` (allows loading the Steamworks SDK library and **overlay library**)
> `com.apple.security.cs.allow-dyld-environment-variables` (**enables the overlay library to be injected into the game process**)
> Note: Steam is not currently compatible with the `com.apple.security.app-sandbox` entitlement.

CrossOver's `wineloader` has the first and lacks the second [V, §3.1] — which is exactly why §3.2
measured what it measured. **[I]** CodeWeavers has no reason to carry the second entitlement; nothing
in CrossOver needs it. Adding it is a one-line change on their side, which makes "ask CodeWeavers"
a legitimate alternative to §5(a1)'s re-signed copy and probably the right first move
commercially. (Basis: the entitlement's sole documented purpose, and CrossOver already shipping the
harder-to-justify `disable-library-validation`.)

From https://partner.steamgames.com/doc/features/overlay **[V]**:

> The overlay supports games that use **DirectX 7 - 12, OpenGL, Metal, and Vulkan**.

> you'll need to make sure to call `SteamAPI_Init` **prior to initializing the OpenGL/D3D device**,
> otherwise it won't be able to hook the device creation.

> The Steam Overlay requires a game **consistently render frames**…

And from https://partner.steamgames.com/doc/api/ISteamUtils **[V]**, on `BOverlayNeedsPresent()`:

> it uses your **Present/SwapBuffers calls to drive its internal frame loop** and it may also need to
> Present() to the screen any time a notification happens… check for this periodically (roughly 33hz).

and on `IsOverlayEnabled()`:

> Checks if the Steam Overlay is running & the user can access it. **The overlay process** could take
> a few seconds to start & hook the game process, so this function will initially return false while
> the overlay is loading.

**[I]** Three consequences for the **shim**. First, `SteamAPI_Init`-before-device-creation is a real
ordering constraint we would inherit — but it is satisfied by construction if the renderer is
DYLD-inserted at process start (§5(a1)), and *not* necessarily satisfied by the `dlopen`-at-init
route (§5(a2)), which is another reason (a2) is the fallback. Second, `BOverlayNeedsPresent` and
`IsOverlayEnabled` must be **forwarded**, not stubbed — they are currently `vt_unmapped`
(`shim_vtables.h:3578-3580`), and a title that honours `BOverlayNeedsPresent` will not repaint for
notifications without it. Third, `IsOverlayEnabled()` returning false *initially and then true* is
the documented, expected shape; games are built for it.

### 6.4 The macOS overlay is not vestigial — Valve's release notes settle it

**[V]** From Valve's own news API
(`api.steampowered.com/ISteamNews/GetNewsForApp/v2/?appid=593110&count=200`), under the notes' own
`macOS` headings:

| Date | Line (verbatim) |
|---|---|
| **2026-05-05** | **"Improved performance of Steam Overlay in games using Metal."** |
| 2026-07-21 | "Fixed chat windows showing over fullscreen game windows when opened in the Steam overlay." |
| 2025-04-01 | "Added Steam Overlay support for games using HDR rendering." |
| 2025-04-29 | "Fixed crash initializing the Steam Overlay in some games when using Parallels." |
| 2025-01-21 | "Fixed mouse input to Steam Overlay not working for some multi-display configurations." |
| 2024-09-11 | "Fixed Steam Overlay crash in some games using Metal rendering" |
| 2023-04-26 | "Fixed Steam Overlay crash in some games using Metal graphics API" |

**A Metal-specific performance fix in May 2026 is only meaningful if the Metal overlay path is live
and in use.** Combined with the 2026-08-03 rebuild of the dylib and its arm64 slice (§1), the
"macOS overlay is a GL-era artifact" hypothesis is **disproved**. Note also 2025-04-01: HDR support
matches the `BGR10A2` and `RGBA16Float` pipeline states found in the binary [V, §1.1] — the binary
and the release notes corroborate each other.

**No Valve statement anywhere says the macOS overlay lacks Metal support**, and there is no public
macOS bug tracker analogous to `ValveSoftware/steam-for-linux` to consult. The one *unknown* the
external research could not close: whether the overlay works for **arm64-native** Metal titles as
distinct from x86-64/Rosetta ones. For us that is moot — the bottle is x86_64 under Rosetta [V, §2].

### 6.5 The protocol is not documented, and the public reimplementations do not speak it

**[V]** Nobody has published `rendermessages.h` or `shmemstream.{h,cpp}`. What exists:

- The **CS:GO 2019 source leak** contains Panorama's *producer* side —
  `panorama/renderer/d3d10d2dsurface.cpp` and `sdlopenglsurface.cpp`, both `#include
  "../../overlay/common/rendermessages.h"` — giving the `GameOverlayRender_SharedTex_%d_%d`
  `(textureID, targetOverlayPID)` key, a 16 MB ring, a `[ESurfaceCommand opcode][POD struct]` framing,
  and two of N opcodes. **This is leaked Valve code; it is a Rosetta stone for field ordering and
  must not be copied.** Credibility of the artefact: high; legal usability: none.
- `Mevasss/Steam-Overlay-External-Render` — the only public code that opens Valve's real objects
  (~15 stars, no README). Windows-only, and its reconstructed ring header disagrees with the leak on
  field types. Credibility **medium**.
- **Goldberg Steam Emu / gbe_fork** ship a `GameOverlayRenderer64.dll` that is **pure stubs plus
  ~1.4 MB of zero padding** "because some apps check the size of this file", and implement their own
  overlay with ImGui. **Zero IPC.** Credibility high (source read directly), and the finding is
  decisive: the most motivated reimplementers in the ecosystem did not attempt the protocol.

**Nothing at all is public about the macOS variant** — the Mach naming, the IOSurface-vs-scanline
choice, the Metal texture handoff. §1.2 of this document may be the most detailed public description
of it that exists, and it is still only names and imports, not semantics.

**[I] This is the strongest possible argument for architecture (a1)/(a2) over (b)/(c).** The
protocol is opaque, unversioned, macOS-undocumented, and Valve changes it whenever they like — the
2026-08-03 rebuild is proof they still touch it. Using Valve's own binary is not a shortcut; it is
the only sane relationship to have with that protocol.

### 6.6 The prior art that architecture (c) would build on

**[V]** `Nemirtingas/ingame_overlay` (actively maintained, last push 2026-08-19) advertises
*"Cross-platform support for Windows, Linux, and macOS. Renderer integration for OpenGL, Vulkan,
DirectX 9/10/11/12, and **Metal**"*, and its tree contains `src/MacOSX/MetalHook.mm` and
`src/MacOSX/NSViewHook.mm`. It is the library `gbe_fork` depends on. **[I]** If (c) is ever pursued,
this is the compositing half, already written, MIT-ish, and independent of Valve entirely — which
lowers (c)'s cost meaningfully and de-risks its *rendering* (though not its *content*) half.
(Basis: README claims plus the file listing; not built or tested here.)

---

## 7. Spikes that resolve the remaining unknowns

Each is a concrete experiment with a pass/fail oracle, in the house style (`macossteamplayresearch.md`
§7, issue #19's "Done when"). They are ordered so that a failure kills the cheapest thing first.

### S-1 — What gates the renderer's Metal hooks? (measured to be *not* graphics)

**Already partly run, and the result narrows the question.** [V] With the renderer
DYLD-inserted into a purpose-built x86_64 Metal program that creates an `MTLDevice`, a
`CAMetalLayer`, and runs three full `nextDrawable` → `presentDrawable:` → `commit` cycles, with
`SteamOverlayGameId=480`, `STEAM_OVERLAY_LOGGING=1` and `SteamNoOverlayUIDrawing` unset,
`/tmp/gameoverlayrenderer.97916.log` contains **only** the header
(`GameID = 480, AppID = 480, OverlayGameID = 480, PID: 97916 … Executable: metaltest`) and the module
list. **No `Hooking …` line, and no `Failed to hook …` line.** The same is true of the
`wineloader` run in §3.3 and the bare `dlopen` in §3.4.

**[I] Therefore the trigger is not "Metal is in use" — it is the client-side handshake.** The
plausible gate is the `SteamOverlayRunning_%llu` named semaphore and/or a reply on
`GameOverlayRender_PIDStream`, neither of which exists unless the real Steam client has decided to run
an overlay instance for that game id. (Basis: those objects exist in the renderer's string table [V];
the renderer's `Disabling overlay for N seconds (%d seconds since last frame from ui process was
seen)` log family [V] shows it tracks liveness of the UI process; and Metal exercise alone provably
does not fire the hook.)

**Run.** Attach `lldb` (or set a `dtrace`/`DYLD_PRINT_*` trace) to the `metaltest` harness above and
break on `class_replaceMethod` / `class_addMethod` — then repeat with each candidate gate satisfied by
hand: (i) `sem_open("SteamOverlayRunning_<gameid64>", O_CREAT)` pre-created; (ii) a fake
`GameOverlayRender_PIDStream` reader. Alternatively, skip straight to S-3 and observe the hook fire
for real.

**Oracle.** A `Hooking …` log line naming a concrete `AGX…`/`CAMetalLayer` class, or a
`class_replaceMethod` call in the debugger. FAIL — and this is the honest disproof of the whole
thesis — if the hook never installs even with a live client-spawned `gameoverlayui`.

**Why it matters.** This is the **one** unverified link in the chain from §1 to §5(a1). Everything
either side of it is measured.

### S-2 — Can CrossOver's front door be made to use our re-signed loader?

**Run.** With the mirror root at `$MIRROR`, invoke `CrossOver.app/.../bin/wine --bottle steam-shim`
with `CX_ROOT=$MIRROR` (and, as a second arm, `WINELOADER=$MIRROR/bin/wineloader` to confirm it is
ignored). Compare against the control run of `steamclient-shim-launch.sh`.

**Oracle.** The launched `Mars.exe` still reaches its main menu **and** `INSERTED-OK`-style evidence
appears — i.e. a marker dylib's constructor fires in the game process — **and** the CrossOver
per-title tweak database still loads (no `err:cxcompatdb:compatdb_init couldn't get path to JSON
database`, the exact symptom issue #19 traced the MoltenVK regression to). FAIL if D3DMetal is lost:
the game's log must still show `AMD Compatibility Mode` and `API d3d12`, not `[mvk-info]`.

**Why it matters.** This is the one place where (a1) can fail architecturally rather than
incidentally. If it fails, (a1) collapses into (a2) or (c).

### S-3 — Will the native client start `gameoverlayui` for a compat-tool-launched Windows title?

**Run.** With S-1 and S-2 passing, launch Mars through `Steam (macOS Play).app` with the overlay
env restored, and watch (i) `ps` for a `gameoverlayui` process, (ii)
`~/Library/Application Support/Steam/logs/gameoverlay_ui.txt` and `gameoverlay_renderer.txt` (both
filenames are in `steamclient.dylib` [V]), (iii) the client's own log for
`GameOverlay: started '%s' (pid %d) for game process %d`.

**Oracle.** A `gameoverlayui` process exists with `-pid` equal to the Wine process hosting
`Mars.exe`. FAIL if the client logs `GameOverlay: failed to execute process` or logs nothing at all.

**If it fails**, the next question is *why*, and the app-config keys found in §1.3 are the levers to
try in order: `allow_overlay`, `DisableOverlay`, `DisableOverlay_OSX`, `DisableOverlayInjection`,
`GameOverlay_CompatMode`, `GameOverlay_TestMode` (the last two are settable via `config/` per the
string forms `config/GameOverlay_CompatMode`, `config/GameOverlay_TestMode` [V]). A hard refusal keyed
on the app's platform, with no override, is the finding that would send us to (c).

### S-4 — Does the swizzled present actually see D3DMetal's frames?

**Run.** With S-1–S-3 passing, press Shift+Tab in Mars.

**Oracle.** The overlay draws over the game. Weaker but still decisive intermediate oracle if it does
not: `STEAM_OVERLAY_FRAME_TIME_LOGGING=1` [V, env var read by the renderer] produces per-frame lines,
proving the hook is on the hot path even if compositing is wrong.

**The specific failure to look for.** D3DMetal or DXMT caching an `IMP`, subclassing `CAMetalLayer`
privately, or presenting through a path that does not route through `-[MTLCommandBuffer presentDrawable:]`
(e.g. `MTLDrawable presentAfterMinimumDuration:` — note Valve hooks that one too [V]). If the hook is
installed but never fires, this is the reason, and the fix is CodeWeavers/Apple territory, not ours.

### S-5 — How bad is the cursor without the interposes?

**Run.** Only under (a2). Compare a windowed, cursor-visible title against a relative-mouse title with
the overlay open.

**Oracle.** In the relative-mouse title, does the OS cursor track the overlay UI, or does the game
keep warping it? FAIL (i.e. (a2) is not shippable as-is) if the overlay cannot be clicked.

### S-6 — Does the PE side need the `keybd_event` repair?

**Run.** With the overlay opening, check whether the game behaves as though Shift and Tab are stuck
down after the overlay closes.

**Oracle.** Reproduce Proton's symptom (`steamclient_main.c:528-550`); if present, port the repair to
the **shim**'s callback pump using Wine's input injection. This spike exists to confirm the repair is
*needed* before writing it — `lsteamclient-mechanics.md` §9 currently lists it under "skip entirely".

---

## 8. What could not be determined here

Stated plainly, because a labelled unknown is worth more than a guess.

1. **What gates the renderer's Metal hooks.** Measured [V]: exercising Metal — device, layer,
   `nextDrawable`, `presentDrawable:`, `commit` — inside a process with the renderer inserted and
   `SteamOverlayGameId` set produces **no** `Hooking …` line. So the gate is not the graphics API. The
   remaining hypothesis is the client-side handshake (`SteamOverlayRunning_%llu` /
   `GameOverlayRender_PIDStream`), which is **inference**, not fact. S-1/S-3. Everything in
   §5(a1)/(a2) rests on this single link.
2. **Whether the native client will run an overlay instance for a Windows-platform app.** §1.3's
   sequence is inference from string ownership, not from observation. S-3.
3. **Whether `CX_ROOT` redirection is sufficient and supported.** Read from `bin/wine` lines 713/806,
   never run. S-2.
4. **The exact content path for architecture (c).** Whether `SteamClient.Overlay.GetOverlayBrowserInfo`
   yields anything a foreign process can render from was not tested; only the method's existence is
   established (`steamclient-js-api-macos.md` ~line 216).
5. **Licensing.** Whether redistributing an ad-hoc- or Developer-ID-re-signed copy of CodeWeavers'
   `wineloader` is permitted by the CrossOver licence. This is not a technical question and it was not
   investigated. It could invalidate (a1) on grounds nothing in this document can measure.
6. **Whether any of this survives a CrossOver update.** CrossOver has already moved from 25.1.1 /
   wine-10.0 (the version the earlier research documents were written against) to 26.2 / wine-11.0
   during this project's life [V]. The mirror root must be rebuilt on every such move.

---

# Addendum — measured 2026-08-23, session of #22 and #24

Everything below was measured on this machine after the study above was written. Three of its
conclusions are **corrected**; two of its unknowns are **closed**; one new blocker is identified that
the study did not anticipate and that decides how, or whether, this can ship.

CrossOver 26.2.0.39821, wine-11.0-8723-g7e8a47752e3, bottle `steam-shim`.

## A1. Arming passes — the study's biggest risk is retired [V]

S-3 asked whether the native client will start `gameoverlayui` for a Windows-platform app. It does
better than that: it arms the overlay for a process it has **no relationship with**. `metalprobe`
(`tools/overlay-probe/`) is a plain unsigned Metal binary — not launched by Steam, never calling
Steamworks — and with `SteamOverlayGameId` set to a real appid, `DYLD_INSERT_LIBRARIES` pointing at
`gameoverlayrenderer.dylib`, and `SteamNoOverlayUIDrawing` unset, Shift+Tab draws the real overlay
over its `CAMetalLayer`.

This retires the risk that could have degraded (a1) to (c). Graphics and arming are both settled;
only injection remains.

## A2. (a2) is dead — load order, not interposition [V]

Same harness, same environment, only load time varies: inserted at launch the overlay draws;
`dlopen`'d it does not, with 13 of 15 interposes rebound making no difference. The interpose-recovery
mechanism itself works exactly as §3.4 predicted — parsing `__DATA,__interpose` and matching each
entry's dyld-bound `original` against `dlsym` identifies **15/15** and hands back Valve's own
replacements — it simply is not what `dlopen` loses. `CFRunLoopRun`/`InMode` could not be tested:
Valve's replacements do not pump a run loop they did not set up. Detail in #22.

## A3. §5(a1) step 1 re-signs the wrong binary [V] — correction

The study has us re-signing `bin/wineloader`. That is only the **first stage**. The binary that
becomes the game process is reached by a second exec and lives in the lib tree:

```
game process → /var/folders/…/winetemp-174243904-…/wineloader
inode 174243904 → …/CrossOver/lib/wine/x86_64-unix/wine
```

`bin/wineloader` is a different inode (174242871) and is irrelevant to injection. Note also
`CrossOver-Hosted Application/wineloader` is a **hard link to `bin/wineloader`** (same inode), so it
is not a third variant and not the differentiator it looked like.

## A4. Relocation preserves entitlements [V] — correction, and it is good news

The study's §3 reasoning assumed the relocated loader is a rewritten copy. It is not. The `winetemp`
path in `ntdll.so` (single xref to `/winetemp-%llu-%llu-%lu-%lu/` at `0x22648`) runs:

```
asprintf → strlcat → mkdir → symlink("<dir>/ntdll.so") → stat → link → symlink (fallback) → posix_spawn
```

`link()` is a **hard link**: same inode, therefore the same signature and entitlements as the source.
An earlier reading of "three different sha256s" as evidence of rewriting was wrong — the differing
hashes were three *different files* (stock, our re-signed copy, and a link to stock), not three
versions of one. **An entitled source yields an entitled game process.** This is what keeps (a1)
alive at all.

## A5. The front door is fully steerable — and it is not enough [V]

Three mechanisms, all confirmed:

- **`[Wine] BinPath`** is a bottle config key (`bin/wine:654`), takes a `:`-separated list
  (`cxwhich`, `bin/wine:25`), and steers `WINELOADER` — proven with a planted marker loader. `LibPath`
  is its sibling.
- **`DYLD_*` cannot be passed in from outside.** `/usr/bin/perl` is SIP-restricted and dyld
  *removes* `DYLD_*` from the environment, so nothing survives the front door from the caller's
  shell. `[EnvironmentVariables]` in `cxbottle.conf` sets them **inside** perl before the exec
  (`CXBottle.pm:9-29`); only `CX_BOTTLE` and `WINEPREFIX` are rejected.
- **`CX_ROOT` cannot be set from the environment** — `locate_cx_root` (`bin/wine:45-82`) computes it
  from `cxwhich($ENV{PATH}, $0)` and overwrites `$ENV{CX_ROOT}`. Only invoking a `wine` inside a
  mirror root steers it; the symlink-following loop short-circuits once `bin/cxmenu` exists beside it.

A mirror root was built and **`CX_LOG` confirms all three take effect**:

```
CX_ROOT    = …/steam-shim/drive_c/cxroot
WINELOADER = …/cxroot/bin/wineloader
WINESERVER = …/cxroot/bin/wineserver
```

**And the game still relocates from the stock loader.** So `ntdll` ignores both `CX_ROOT` and
`WINELOADER` and resolves the real loader **relative to its own path** — consistent with the
disassembly, which symlinks `<tempdir>/ntdll.so` → `dirname(source)/ntdll.so`.

**[I] That defeats the mirror-root family**, because `ntdll.so` cannot be given a mirror-side path:

| `ntdll.so` in the mirror | result |
|---|---|
| symlink | works, but resolves back into CrossOver's tree — stock sibling wins |
| copy | **SIGSEGV** (exit 139, no output) — signature verifies clean, so not a signing fault |
| hard link | `Operation not permitted` across the app bundle |

## A6. `codesign` on the shipped loader fails — sign a copy instead [V]

`codesign --force --sign -` directly on `lib/wine/x86_64-unix/wine` returns **`internal error in
Code Signing subsystem`**. The inode had 19 links — one real path plus 18 accumulated `winetemp`
dirs. The failure is atomic: signature, inode, size and validity all unchanged afterwards, and
CrossOver still runs. Signing a copy in `/tmp` (one link) succeeds and yields a correctly entitled
binary.

## A7. The blocker that decides shippability: App Management [V]

Installing that entitled binary over CrossOver's own fails:

```
cp /tmp/wine.entitled "$W"
→ Operation not permitted
```

Not POSIX permissions and not `schg` — `ls -lO` shows no flags, and even `touch` of a *new* file in
that directory fails. CrossOver.app is a signed, notarized third-party bundle
(`TeamIdentifier=9C6B7X7Z8E`), and since macOS 14 modifying another app's bundle requires the
**App Management** TCC grant for the writing process.

**[I] This reframes the whole approach.** Shipping the in-place re-sign means an installer that (1)
prompts the user to grant App Management over CrossOver, (2) rewrites a CodeWeavers binary inside
their signed bundle, and (3) silently re-does it after every CrossOver update. That is a great deal
of fragile, user-visible surface for a feature.

The study treated "ask CodeWeavers to add `com.apple.security.cs.allow-dyld-environment-variables`"
as step 0, a courtesy before the real engineering. On this evidence it is not a courtesy — it is the
only route that avoids a TCC prompt, a vendor-bundle modification, and per-update repair
simultaneously, and CrossOver already ships the harder-to-justify half
(`disable-library-validation`). Valve's own documentation names both entitlements as the macOS
overlay's requirement, so the ask has a citable rationale.

## A8. Where (a1) stands

Unresolved, by one test. With App Management granted, `cp /tmp/wine.entitled "$W"` and a launch would
prove or kill (a1) outright — everything else in the chain is measured and works. Until then:

- **(a1)** viable, injection unproven end-to-end, and its shipping story depends on the CodeWeavers ask.
- **(a2)** closed (A2).
- **(c)** unchanged, and now the fallback of record if (a1)'s injection cannot be made to ship.
- **(d)** unchanged and still worth shipping first (#23).

---

# Addendum 2 — measured 2026-08-24, re-test of (a2)

**A2 above is wrong, and A8's "(a2) closed" with it.** `dlopen` *is* a way in. The failure #22
recorded was one of load *time*, and the timing it tested is not the timing (a2) would have.

## B1. The renderer's log is opt-in — #22 ran blind [V]

`STEAM_OVERLAY_LOGGING` (and `STEAM_OVERLAY_LOGGING_FLUSH`) are `getenv` calls in the dylib
(`0x1e194`, `0x1e17e` in the x86_64 slice), gating `/tmp/gameoverlayrenderer.%d.log`. #22 concluded
the renderer "writes no log in either case" and therefore could not name the stage the `dlopen` path
reached. With the variable set, the log is written in every run and answers the question directly:

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

**[I]** Any future overlay run should set it. A mute failure was what made A2's negative look
structural.

## B2. `dlopen` before `NSApplication` hooks and arms — overlay confirmed drawing [V]

`tools/overlay-probe/metalprobe5.m` is `metalprobe` with the `dlopen` movable across startup by
`DLOPEN_WHEN`. Renderer, environment and render loop are otherwise identical; the only variable is
the call site.

| `DLOPEN_WHEN` | dlopen happens | `Hooking` lines | overlay |
|---|---|---|---|
| `ctor`   | `__attribute__((constructor))`, before `main` | 5/5 | ✅ draws |
| `main`   | first statement of `main`                     | 5/5 | ✅ draws |
| `nsapp`  | after `[NSApplication sharedApplication]`      | 0 | ❌ |
| `device` | after `MTLCreateSystemDefaultDevice()`        | 0 | ❌ |
| `layer`  | after the `CAMetalLayer` is created and attached | 0 | ❌ |
| `late`   | after the window is on screen (#22's row 2)   | 0 | ❌ |

`ctor` reproduced across three runs. The overlay was visually confirmed over `metalprobe5`'s layer
on Shift+Tab, and the log's `Enabling overlay` corroborates it.

**The gate is `NSApplication` instantiation**, not `dyld`'s interposition window, not the Metal
device, not the layer. Load before `NSApp` exists and attach installs the five `MTLCommandBuffer`
hooks; load after and attach still runs and still prints its module list, but hooks nothing.

## B3. The interposes are confirmed unnecessary [V]

`metalprobe5` links no `fishhook`, parses no `__DATA,__interpose`, and rebinds nothing — and the
overlay draws. §3.4's "precisely-known hole" and #21's GOT-rebinding plan are both moot for the
Metal path. The recovery code in `metalprobe3` remains valid and remains useful only for #21's
inserted-stub idea.

## B4. What this changes

(a2) needs **no entitlement, no re-signed loader, no mirror root, no App Management prompt, and no
modification of CrossOver.app** — the three things A5–A7 identified as the shipping blockers for
(a1) all belong to injection-at-launch, which this route does not use. It needs one thing instead:

> our unixlib must `dlopen` `gameoverlayrenderer.dylib` before `winemac.so` instantiates
> `NSApplication`.

That is the remaining unknown, and it is a Wine-internal ordering question rather than a macOS
platform one. `ntdll`'s unixlib initialises long before the graphics driver is loaded, so the
window exists on paper; it has not been measured. **Spike S-7.**

## B5. Standing corrections

- **A2 is withdrawn.** (a2) is not closed; #22's rows 2–5 establish only that *late* `dlopen` fails.
- **A8's ranking is superseded**: (a2) returns as the leading candidate precisely because it is the
  one route with no shipping-story cost, and (a1) becomes its fallback with S-2/App Management still
  in the way.
- Unchanged: **(d)** (#23) ships first regardless; **(c)** stays the fallback of record.
