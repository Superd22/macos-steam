# `compat.vdf` / `SetAppPlatformOverride` on macOS Steam

Resolves [#16](https://github.com/Superd22/macos-steam/issues/16). Client build `1785187029`, macOS 26.5.2, Apple Silicon.

**Verdict, in two parts:**

1. **As shipped, the entire Steam compat subsystem is off on macOS** — gated by one boolean latched in
   `CCompatManager`'s constructor from a literal comparison against `"linux"`. No configuration of any
   kind reaches it. `compat.vdf` is never even parsed.
2. **That gate is one 4-byte instruction, and flipping it delivers everything.** A patched
   `steamclient.dylib` gives a working **per-app platform override** *and* routes launch through a
   registered compat tool with Valve's full environment contract. Both were demonstrated end to end.

---

## 1. The `compat.vdf` contract

Addresses are file offsets in the named slice of
`Steam.AppBundle/Steam/Contents/MacOS/steamclient.dylib` (`lipo -thin <arch>`; slice vmaddr == file
offset). The dylib is stripped; functions were located by cross-referencing their VPROF name literals.
**Analysis below is x86_64 unless stated. Steam runs arm64 (`LSArchitecture=arm64`) — patch the arm64
slice, see §3.**

### Path

`CCompatManager::LoadPlatformOverrideCache` @ `0x6a7ed4`, `FlushPlatformOverrideCache` @ `0x6a86c0`:

```
"%s/%s/%u/config/%s"    fmt      @ 0x17018dc
    arg1 = Steam base dir
    arg2 = "userdata"            @ 0x15fecb0
    arg3 = %u  <- accountid, from  *(m_pClientCtx + 0x300) + 0x27a
    arg4 = "compat.vdf"          @ 0x17018ef
```

→ `<SteamDir>/userdata/<accountid>/config/compat.vdf`

### Schema

Root key `"platform_overrides"` (`0x1701925`); one subkey per appid; two fields, `"dest"` (`0x1701938`)
and `"src"` (`0x170193d`).

```
"platform_overrides"
{
	"<appid>"
	{
		"dest"		"<platform>"
		"src"		"<platform>"
	}
}
```

In memory: `CUtlHashMap<uint32, CCompatManager::PlatformOverrides_t>` (the mangled name
`11CUtlHashMapIjN14CCompatManager19PlatformOverrides_tE...` survives in the binary). Element stride
`0x20`: appid `u32` at `+0`, `char* dest` at `+8`, `char* src` at `+0x10`. `Flush` (`0x6a87f7`,
`0x6a8832`) writes `dest` then `src`, so the file round-trips.

### Semantics — `dest` is the HOST, `src` is what the host is presented AS

`GetSystemConfigurationForApp` (`0x5a6488`) returns the *system* configuration to use **for one app**.
It is seeded from the real `GetSystemConfiguration()` at `0x5a64ce`, so the `oslist` it reads and
rewrites is the **host's**, not the app's:

```c
if (!compatMgr->BIsCompatEnabled())              // vtable slot 0  -> §2
    goto skip;                                    // <-- stock macOS always takes this
if (bCallerFlag) goto skip;

const char *host = kv.GetString("oslist", "");    // "macos" on this machine
if (compatMgr->BIsCompatibilityToolEnabled(appid))
    kv.SetString("oslist", <tool's from_oslist>); // Steam Play path
else if (GetAppPlatformOverride(appid, &dest, &src)
         && *dest && *src
         && V_strnicmp(host, dest, INT_MAX) == 0)
    kv.SetString("oslist", src);                  // compat.vdf path
```

So an entry reads: **"when the host is `dest`, tell this one app the host is `src`."** For a Windows
title on macOS that is `dest "macos"`, `src "windows"`.

This is confirmed by Valve's own writer, not just by reading: `YldCheckIfAppNeedsPlatformCompatibility`
calls `SetAppPlatformOverride(appid, dest = <host oslist>, src = <tool/app platform>)`, and once the
gate is open Steam writes the file itself in exactly that shape (see §4). It also reconciles the
`additional_dependencies` schema, where `src_os`/`dest_os` mean game-platform/host-platform.

The match is **full-string** case-insensitive, so a dual-platform `"windows,macos"` host string could
never match a single-token `dest`. Entries with `dest == src`, or either empty, are rejected by `Load`
with a log line to `compat_log.txt`:

```
platform override cache: ignore bad entry %u "%s" "%s"     @ 0x170196b   (appid, src, dest)
```

That line is the reliable "was the file parsed?" oracle — see §5.

`oslist` is the right lever precisely because `GetSystemConfigurationForApp` feeds both
`CApplicationManager::YieldingPopulateAppStates` (→ `display_status`, depot selection) and
`CUserAppInfo::GetAvailableLaunchOptions` (→ launch). One rewrite moves install *and* launch.

---

## 2. The gate: `m_bCompatEnabled`

`CCompatManager` carries one byte at `this + 0x7b0`, set in its constructor
(x86_64 `0x69d73a`–`0x69d764`, arm64 `0x726338`–`0x726350`):

```c
m_bCompatEnabled = ( V_stricmp( GetSystemConfiguration().GetString("oslist",""), "linux" ) == 0 );
```

Host `oslist` is `"macos"` here — confirmed by `compat_log.txt` rejecting every Proton/SLR tool as
*"a different target platform linux"* while accepting `crossover-probe` (`to_oslist "macos"`).

An exhaustive scan of every instruction touching displacement `0x7b0` finds two writers in each slice:
the constructor's `sete`/`cset`, and one that stores **0** while logging *"Disabling compatibility
layer."* **Nothing ever sets it to 1 at runtime.**

Everything hangs off that byte:

| Site | Stock macOS |
|---|---|
| vtable slot 0 `BIsCompatEnabled()` (x86_64 `0x69fe94`) — literally `return m_bCompatEnabled` | `false` |
| `GetSystemConfigurationForApp` outer gate `0x5a6504` | **both** branches skipped |
| `BIsCompatibilityToolEnabled` `0x6a2925` | early-returns `false` |
| tool→oslist transform `0x6a2425` | early-returns `false` |
| `YldCheckIfAppNeedsPlatformCompatibility` | never reaches `SetAppPlatformOverride` |

`YldRegisterTool` does **not** check it — which is why [#5](https://github.com/Superd22/macos-steam/issues/5)
and [#15](https://github.com/Superd22/macos-steam/issues/15) saw registration, manifest loading,
`GetAvailableCompatTools()` and mapping all succeed while nothing downstream ever moved. It also
explains [#9](https://github.com/Superd22/macos-steam/issues/9): `RunGame` reached `CreatingProcess`
and died at `AppError_46 "OS Error 0"` because Steam exec'd the PE directly, the tool never being
consulted.

`RemoteStorage_AppPlatformOverrideBackupComplete_t` is **not** a Steam Cloud gate — it belongs to
`CSpecifyAppCompatToolJob`, which reaches the override step only through `SetAppPlatformOverride`. It
never completes because it is never started.

---

## 3. Crossing the gate

### The patch (arm64 — the slice Steam actually runs)

```
726338:  adrp x1, 0x168d000
72633c:  add  x1, x1, #0x2a8      ; "linux"   <-- patch site
726340:  mov  w2, #0x7fffffff
726344:  bl   <strncasecmp>
726348:  cmp  w0, #0x0
72634c:  cset w8, eq
726350:  strb w8, [x19, #0x7b0]   ; m_bCompatEnabled
```

`"macos"` sits at `0x168d2a2`, `"linux"` at `0x168d2a8` — 6 bytes apart, so the whole patch is one
immediate:

```
  file offset (arm64 slice 0x72633c + fat offset 0x1A08000) = 0x212A33C
  21 a0 0a 91   ->   21 88 0a 91          ; add x1, x1, #0x2a2  -> "macos"
```

Semantics are preserved rather than forced — compat is enabled iff the host is macOS — so the
"Disabling compatibility layer" path stays coherent. (The blunt alternative on x86_64:
`sete 0x7b0(%rbx)` = `0f 94 83 b0 07 00 00` → `movb $1, 0x7b0(%rbx)` = `c6 83 b0 07 00 00 01`,
byte-for-byte the same length.)

### Signing

`steam_osx` and `steamclient.dylib` both sign with `flags=0x0(none)` — **no hardened runtime, no
library validation** — and the bundle has `Sealed Resources=none`. An ad-hoc re-signed dylib loads
fine. `codesign -f -s -` alone fails (`main executable failed strict validation`) and silently leaves
Valve's stale signature; you must `codesign --remove-signature` **first**, then ad-hoc sign.

### Defeating the bootstrapper — it checks sizes only

```
[21:43:38] Verifying installation...
[21:43:38] Verifying file sizes only
[21:43:38] Verification complete
```

The 4-byte edit preserves size, but re-signing does not: Valve signs with 4 KB hash pages (6523
hashes), `codesign` ad-hoc uses 16 KB (1631), so the file shrinks by **156,704 bytes** and Steam
restores the original from its local package cache. **Pad the file back to the exact original byte
count** and verification passes. It still loads: the kernel hashes only up to `codeLimit`, and the
padding lands after the last slice, outside every mapped segment. `codesign -v` calls the padded file
"failed strict validation" — dyld does not care.

No `BootStrapperInhibitAll` is needed; the patch simply has to be re-applied after each client update.

### Shippability

Workable but not clean: offsets are build-specific (pattern-scan for the
`adrp/add → bl strncasecmp → cset → strb [xN, #0x7b0]` shape rather than hardcoding), the patch strips
Valve's signature, and it must be re-applied on every client update. **`DYLD_INSERT_LIBRARIES` is the
better delivery vehicle and is untested** — hardened runtime is off, so `DYLD_*` is honoured, and an
injected dylib patching memory at load touches none of Valve's files.

---

## 4. What the patch delivers

### Per-app platform override — measured

| appid | title | `compat.vdf` / mapping | before | after |
|---|---|---|---|---|
| 3215050 | Surviving Mars: Relaunched | override | 14 | **9** |
| 945360 | Among Us | *none, then tool-mapped* | 14 | **19 → 11** |
| 236850 | Europa Universalis IV | *none* | 9 | 9 |

Among Us with **no** entry stayed at `14` while Surviving Mars flipped — the host platform did **not**
change globally, independently confirmed by `compat_log` still rejecting Proton as *"a different target
platform linux"*. This is the genuine per-app override that map trap 3 assumed did not exist.

Depot selection followed: Surviving Mars moved `9,675,067,551 → 10,103,246,511` bytes once the override
applied, while EU4 stayed on its native set.

### Steam writes `compat.vdf` itself

Mapping `crossover-probe` to Among Us made `SetAppPlatformOverride` + `Flush` fire for real, producing:

```
"platform_overrides"
{
	"945360"
	{
		"dest"		"macos"
		"src"		"windows"
	}
}
```

Valve's own writer, in the orientation described in §1. (`Flush` rewrites the whole file from the
in-memory map, so hand-written entries for other appids are replaced.)

### Launch routes through the tool, with the full contract

`GetLaunchOptionsForApp(945360)` began returning a launch option — a Windows option on a macOS host —
and `RunGame` invoked the tool three times: twice for `iscriptevaluator.exe`, then the real thing:

```
argv[0]=waitforexitandrun
argv[1]=…/steamapps/common/Among Us/Among Us.exe
cwd:     …/steamapps/common/Among Us

STEAM_COMPAT_APP_ID=945360
STEAM_COMPAT_DATA_PATH=…/steamapps/compatdata/945360
STEAM_COMPAT_INSTALL_PATH=…/steamapps/common/Among Us
STEAM_COMPAT_CLIENT_INSTALL_PATH=…/Steam.AppBundle/Steam/Contents/MacOS
STEAM_COMPAT_LIBRARY_PATHS / _MOUNTS / _SHADER_PATH / _TOOL_PATHS   all set
STEAM_DYLD_INSERT_LIBRARIES=…/steamloader.dylib:…/gameoverlayrenderer.dylib
SteamAppId=945360   SteamGameId=945360
```

That is Valve's compat-tool interface, complete, on macOS — exactly what a CrossOver wrapper needs.
Steam also tracked the session (`OnAppLifetimeNotification: release session(s) for appID 945360`), so
playtime and Play/Stop state come for free.

---

## 5. Method notes

### ⚠️ `atime` is NOT a valid oracle for a freshly-planted file

The map lists `atime` as a trustworthy "did Steam read this?" test. **It failed here.** After writing
`compat.vdf` (mtime `21:07:57`, atime `21:07:58`), an explicit read did **not** advance atime —
relatime behaviour, since atime was already newer than mtime and recent. Every freshly-written probe
file is in exactly that state. Use a log-line oracle instead: plant a deliberately-invalid entry whose
rejection Steam logs, and grep for it. The map's atime claim holds only for long-lived files Steam
wrote itself.

### The negative control that made §2 airtight

Before patching, a planted `compat.vdf` containing `480 {dest=windows, src=windows}` — invalid by
construction — produced **no** `ignore bad entry` line across startup, app queries, install-manager
evaluation and a `SpecifyCompatTool` call. After patching, the same file produced it immediately:

```
[21:49:14] platform override cache: ignore bad entry 480 "windows" "windows"
```

### Levers that do nothing

- **`-compat-disable-filtering` / `-compat-force-slr`** — the only two `-compat*` switches in the
  binary. Help strings: *"Disable filtering of normally unlisted runtimes"* and *"Force enable/disable
  using SLR 1.0 ... over legacy LD_* setup"*. Neither touches `0x7b0`.
- **`@sSteamCmdForcePlatformType linux` at startup** — `GetSystemConfiguration()` *does* honour the
  convar (`0x5a6774`–`0x5a67c5`) and it *is* the gate's input, but the value is latched in the
  constructor and the runtime form is console-only. **Untested idea:** because the byte is write-once,
  booting with host `linux` to latch it `true` and then reverting the convar might work without any
  patching — the "latch and release" route. It would be far more shippable. Its cost is a window in
  which Steam believes it is a Linux host and recomputes depots library-wide.

---

## 6. Consequence for the map

Half A is **alive**, not dead — but it costs a patched client.

- **Install**: a per-app override exists and works, so the global `@sSteamCmdForcePlatformType windows`
  convar (disqualified by map trap 3 for re-platforming dual-platform titles) is no longer needed.
- **Launch**: Steam drives it, hands the tool the `.exe` under `waitforexitandrun` with the full
  `STEAM_COMPAT_*` contract, and tracks the session — so playtime, Play/Stop and process lifetime are
  inherited rather than lost.

What remains open: whether the install actually pulls the Windows depot set end to end; whether
`DYLD_INSERT_LIBRARIES` or latch-and-release can replace the binary patch as a delivery vehicle; and
how the patch is maintained across client updates.
