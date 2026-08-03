# CrossOver bridge surface — what this machine actually exposes

**Scope:** the CrossOver install and bottle on *this* Mac, and the viable transports for
getting a Windows PE DLL inside a bottle to native macOS code. Proton's own internals are
out of scope here (covered separately in `lsteamclient-mechanics.md`).

**Investigated:** 2026-08-02, on `Apple M3 Pro`, `macOS 26.5.2 (25F84)`, arm64.

---

## Summary (read this first)

1. **CrossOver 25.1.1 (build 25.1.1.38624), Wine `wine-10.0-8474-g427b76fe09f`.** Matching
   source tarball exists and is downloadable: `crossover-sources-25.1.1.tar.gz`, 178 MB,
   dated 2025-09-15.
2. **The entire Wine UNIX side of this install is `x86_64`-only Mach-O — no arm64 slice
   anywhere.** `wineloader`, `wineserver`, and every `lib/wine/x86_64-unix/*.so` are thin
   x86_64. There is no `aarch64-unix` / `aarch64-windows` tree. The whole Wine process tree
   therefore runs **under Rosetta 2**; there is no ARM64EC and no arm64 Wine here. The
   bottle is `WineArch = win64`, so Windows x86_64 PE code runs *natively as x86_64* inside
   a Rosetta-translated host process — Wine is not emulating x86 itself.
3. **Consequence: our native-side bridge code must be built `x86_64`, and that is fine.**
   The native macOS `steamclient.dylib` is universal `x86_64 + arm64`, and I verified
   empirically that an x86_64 (Rosetta) process can `dlopen()` it and resolve
   `CreateInterface`. **No process boundary is forced by architecture.** The running native
   Steam client is arm64 (`--annotation=platform=macosarm64`), but that is a separate
   process we talk to over Valve's own IPC, so its arch is not our problem.
4. **The unixlib route is live and usable in this build.** `__wine_unix_call` /
   `__wine_unixlib_handle` are exported from the shipped PE `ntdll.dll`;
   `get_builtin_unix_funcs` + `__wine_unix_call_funcs` exist in the shipped `ntdll.so`;
   the "Wine builtin DLL" PE marker is literally at file offset `0x40` of every shipped
   builtin; `WINEDLLPATH` (settable per-bottle via `cxbottle.conf` `[Wine] "DllPath"`) feeds
   the builtin search path; and `wineloader` carries
   `com.apple.security.cs.disable-library-validation`, so an **unsigned third-party `.so`
   can be dlopen'd into the Wine process**. CrossOver even ships a working worked example:
   `winelib.dll` (PE) + `winelib.so` (unixlib) exporting `winelib_execute_command` etc.
   **No Wine rebuild is required.** No SDK/headers are shipped, but none are needed — the
   ABI is two exported symbol arrays and a DOS-stub magic string.
5. **Transport verdict: unixlib first, TCP loopback as the portable fallback.** Unixlib is
   an in-process call (sub-microsecond, no arch crossing, no serialisation). TCP loopback
   measured **~18 µs round-trip** on this machine (essentially identical under Rosetta and
   native), which is too slow for hot Steamworks paths but fine for a control channel.

---

## 1. Wine version and provenance

```
$ '~/Applications/CrossOver.app/Contents/SharedSupport/CrossOver/CrossOver-Hosted Application/wineloader' --version
wine-10.0-8474-g427b76fe09f

$ '.../CrossOver-Hosted Application/wineserver' --version
Wine 10.0

$ /usr/libexec/PlistBuddy -c "Print :CFBundleShortVersionString" -c "Print :CFBundleVersion" \
    ~/Applications/CrossOver.app/Contents/Info.plist
25.1.1
25.1.1.38624
```

- `.../SharedSupport/CrossOver/README` line 3: `Version 25.1.1`, dated `September 12, 2025`.
- Bottle records the same: `cxbottle.conf` line 114 → `"Version" = "25.1.1.38624"`.
- App is built against `macosx15.1` SDK (`DTSDKName`), `LSMinimumSystemVersion = 10.15`.
- The git describe string `wine-10.0-8474-g427b76fe09f` means: base tag Wine 10.0, plus
  8474 commits — i.e. this tracks Wine's 10.x development branch, not the 10.0 release
  point. Treat it as "Wine 10.x mid-cycle plus CodeWeavers patches".

**Source availability — confirmed live, not assumed:**

```
$ curl -sS -I -L https://media.codeweavers.com/pub/crossover/source/crossover-sources-25.1.1.tar.gz
HTTP/2 200
last-modified: Mon, 15 Sep 2025 14:15:01 GMT
content-length: 178524083
```

The directory index itself returns 403, but direct filenames resolve. 25.1.0 (2025-08-12)
and 25.0.0 (2025-03-11) are also present. So a byte-matching source tree for the exact
installed build **is** obtainable — useful as a reference for the unixlib ABI even though
we do not need to rebuild.

---

## 2. Architecture — the big one

### What is actually on disk

```
$ file '.../CrossOver-Hosted Application/wineloader'
Mach-O 64-bit executable x86_64
$ file '.../CrossOver-Hosted Application/wineserver'
Mach-O 64-bit executable x86_64

$ lipo -archs .../lib/wine/x86_64-unix/{ntdll,win32u,winemac,ws2_32,winelib}.so
x86_64   (all five — thin, no fat header)

$ lipo -archs .../lib64/libcxfreetype.dylib .../lib64/libglib-2.0.dylib
x86_64   (both)

$ lipo -archs ~/Applications/CrossOver.app/Contents/MacOS/CrossOver
x86_64 arm64      # only the *GUI* app is universal
```

Directory layout under `.../SharedSupport/CrossOver/lib/wine/`:

| dir | contents |
|---|---|
| `i386-unix/` | **empty** |
| `i386-windows/` | 827 entries (32-bit PE builtins) |
| `x86_64-unix/` | 34 `.so` (the entire UNIX side) |
| `x86_64-windows/` | 769 entries (64-bit PE builtins) |

There is **no** `aarch64-unix`, `aarch64-windows`, `arm64ec`, or similar directory anywhere
in the bundle (`find ~/Applications/CrossOver.app -type d -name "*arm64*" -o -name "*aarch64*"`
returns nothing).

### How execution actually stacks up

- `wineloader` is thin x86_64 → macOS runs it under **Rosetta 2**
  (`/Library/Apple/usr/libexec/oah/libRosettaRuntime` is present). Every Wine process —
  loader, wineserver, and all unixlibs — is a Rosetta-translated x86_64 process.
- The bottle is `WineArch = win64` (`cxbottle.conf:12`, `Template = "win10_64"`). Windows
  **x86_64 PE code executes as x86_64 machine code directly** in that process; Wine does no
  instruction emulation. The x86→arm64 translation is entirely Rosetta's, one layer below
  Wine.
- 32-bit Windows PEs run through Wine's **new WoW64** path (empty `i386-unix` + populated
  `i386-windows`; `ntdll.so` contains `"starting %s in experimental wow64 mode"`,
  `build_wow64_parameters`, `__wine_unix_call_wow64_funcs`). Not relevant to us —
  `steam_api64.dll` is x86_64.
- CrossOver's launcher is *written to support* an arm64 host build but this install has none:
  `.../CrossOver-Hosted Application/wine` line 655 —
  `my $host = -e "$ENV{CX_ROOT}/lib/wine/aarch64-unix/ntdll.so" ? "aarch64" : "x86_64";`
  Ditto `ntdll.so` contains the strings `/i386-windows`, `/x86_64-windows`, `/arm-windows`,
  `/aarch64-windows`. So a future CrossOver *could* ship an arm64 host. Plan for it; don't
  build for it yet.

### So which arch must our native code be?

**x86_64.** Anything loaded into the Wine process — a unixlib `.so`, an `LD_PRELOAD`-style
injection, anything using `__wine_unix_call` — must be x86_64 Mach-O.

Verified that the host toolchain can produce exactly that (cross-compiling on this arm64
Mac, no extra tooling):

```
$ clang -arch x86_64 -dynamiclib -o t.so t.c -Wl,-install_name,@rpath/t.so
$ file t.so
Mach-O 64-bit dynamically linked shared library x86_64
$ nm -gU t.so
0000000000001000 S ___wine_unix_call_funcs
0000000000001008 S ___wine_unix_call_wow64_funcs
$ codesign -dvv t.so
code object is not signed at all
```

### Is a process-boundary crossing unavoidable? — No.

The thing we must eventually call is
`~/Library/Application Support/Steam/Steam.AppBundle/Steam/Contents/MacOS/steamclient.dylib`,
which is `lipo -archs` → `x86_64 arm64`. Tested loading it from both arch flavours:

```
$ ./d_x86 ".../steamclient.dylib"
dlopen OK handle=0xf8d10 CreateInterface=0x10f300820      # x86_64 process, under Rosetta
$ ./d_arm ".../steamclient.dylib"
dlopen OK handle=0x6c350a40 CreateInterface=0x10c096038   # arm64 process, native
```

An x86_64 Rosetta process loads the native macOS steamclient and resolves its
`CreateInterface` factory. **Architecture does not force a helper process.** A unixlib
loaded into the Wine process can host `steamclient.dylib` directly.

Two caveats to carry forward:
- The *Steam client process itself* is arm64 (`Steam Helper` crashpad annotation
  `--annotation=platform=macosarm64`; `steam_osx` is universal). Whatever IPC
  `steamclient.dylib` uses to reach it must therefore already be arch-agnostic. Both sides
  are LP64 little-endian, so struct layouts are compatible, but this is Valve's private
  protocol and untested from an x86_64 client.
- `libsteam.dylib` in the same directory is **i386** (legacy leftover). Do not touch it;
  `steamclient.dylib` is the real target.
- Rosetta and native measure the same for syscalls, so there is no meaningful *performance*
  penalty to being x86_64 here — only a longevity one (see §6).

---

## 3. Unixlib reachability

### What is shipped

PE side — `lib/wine/x86_64-windows/ntdll.dll` exports (via string-table scan of the export
directory):

```
__wine_unix_call
__wine_unix_call_dispatcher
__wine_unixlib_handle
__wine_unix_spawnvp
__wine_syscall_dispatcher
wine_nt_to_unix_file_name
wine_unix_to_nt_file_name
```

UNIX side — `lib/wine/x86_64-unix/ntdll.so` contains:

```
__wine_unix_call_dispatcher   (exported symbol, confirmed via nm)
__wine_unix_call_funcs
__wine_unix_call_wow64_funcs
__wine_unixlib_handle
get_builtin_unix_funcs
find_builtin_dll / open_builtin_so_file / dlopen_dll / load_builtin
/i386-windows  /x86_64-windows  /arm-windows  /aarch64-windows
/x86_64-unix
Wine builtin DLL
%s found in WINEDLLPATH but not a builtin, ignoring
%s has prefer-native flag, ignoring builtin
cannot find builtin library for %s
invalid .so library %s, too old?      (legacy winelib path, needs __wine_spec_nt_header)
```

`get_builtin_unix_funcs` sits directly adjacent to `__wine_unix_call_funcs` /
`__wine_unix_call_wow64_funcs` and `"failed to load %s: %s"`, i.e. the standard
`NtQueryVirtualMemory(..., MemoryWineUnixFuncs, ...)` → `dlopen` → `dlsym` path is present
and unmodified.

### The mechanism is demonstrably in use by a third-party-shaped module

CrossOver ships its own non-upstream unixlib pair, which is a working template:

- `lib/wine/x86_64-windows/winelib.dll` — PE builtin. Imports `NtQueryVirtualMemory` and
  `__wine_unix_call_dispatcher` from `ntdll.dll`. Exports:
  `winelib_execute_command`, `winelib_chdir`, `winelib_path_unix_to_windows`,
  `winelib_get_home_directory`, `winelib_get_dpi`, `winelib_notify_on_idle`,
  `winelib_wait_child_pipe`, `winelib_wait_child_pipe_setup`,
  `winelib_alt_loader_setup` / `_cleanup`.
- `lib/wine/x86_64-unix/winelib.so` — the matching unixlib. `nm -gU` shows **exactly two**
  exported symbols:
  ```
  0000000000001020 S ___wine_unix_call_funcs
  0000000000001070 S ___wine_unix_call_wow64_funcs
  ```

That is the entire UNIX-side ABI surface. `otool -L` on `ws2_32.so` shows unixlibs link only
`@rpath/ntdll.so` and `/usr/lib/libSystem.B.dylib`; `otool -hv` shows filetype `DYLIB`.

### The PE side must be marked as a Wine builtin

The marker is a plain string at file offset `0x40` (in the DOS stub) of every shipped builtin:

```
$ python3 -c "print(open('.../x86_64-windows/ws2_32.dll','rb').read()[0x40:0x80])"
b'Wine builtin DLL\x00...\x00t be run in DOS mode.\r\r\n$\x00...'
```

Trivial to reproduce by patching a mingw- or Rust-built `x86_64-pc-windows-gnu` cdylib.

### The search path is configurable per bottle, no repackaging of CrossOver required

`.../CrossOver-Hosted Application/wine` lines 645–712 build `WINEDLLPATH` like this:

```perl
my $dll_path = expand_string($cxconfig->get("Wine", "DllPath") || <default …/wine/$host-windows>);
...
$ENV{WINEDLLPATH} = $dll_path;
```

and `etc/CrossOver.conf` lines 269–280 document `DllPath` as *"a list of directories
containing builtin Wine dlls… this setting overrides WINEDLLPATH. To use WINEDLLPATH, set
this field to `${CX_ROOT}/lib/wine:${WINEDLLPATH}`"*. `cxbottle.conf` also has an
`[EnvironmentVariables]` section (line 325 in `Steam-2/cxbottle.conf`) for arbitrary env
vars in the Wine environment. So we can point Wine at our own directory containing
`<ourdir>/x86_64-windows/steamclient64.dll` and `<ourdir>/x86_64-unix/steamclient64.so`
without touching the CrossOver bundle.

### Code signing does not block us

```
$ codesign -dvvv --entitlements - '.../CrossOver-Hosted Application/wineloader'
Identifier=com.codeweavers.CrossOver.wineloader
Format=Mach-O thin (x86_64)
CodeDirectory v=20500 … flags=0x10000(runtime)
Authority=Developer ID Application: CodeWeavers Inc. (9C6B7X7Z8E)
  com.apple.security.cs.allow-unsigned-executable-memory  = true
  com.apple.security.cs.disable-executable-page-protection = true
  com.apple.security.cs.disable-library-validation         = true
```

Hardened runtime is on, but **library validation is explicitly disabled**, so an
ad-hoc-signed or entirely unsigned third-party `.so` can be `dlopen`'d into the Wine
process. (x86_64 code is also exempt from the arm64 mandatory-signature rule.)

### No SDK is shipped

`find ~/Applications/CrossOver.app \( -name "*.h" -o -name "*.def" -o -name "*.a" \)`
returns only GStreamer and PyObjC headers. There is no `winternl.h`, no
`wine/unixlib.h`, no import libraries, no `include/` tree, no developer package.

### Verdict

**Feasible without rebuilding Wine.** The required contract is small and fully observable
from the shipped binaries:

1. PE `steamclient64.dll` built for `x86_64-pc-windows-gnu`, with `Wine builtin DLL\0`
   written at offset `0x40`.
2. UNIX `steamclient64.so`: an x86_64 Mach-O `DYLIB` exporting exactly
   `__wine_unix_call_funcs` and `__wine_unix_call_wow64_funcs` (arrays of
   `NTSTATUS (*)(void *args)`).
3. Both placed in `<dir>/x86_64-windows/` and `<dir>/x86_64-unix/` respectively, with
   `<dir>` added to `Wine/DllPath` in the bottle's `cxbottle.conf`.
4. PE side calls
   `NtQueryVirtualMemory(GetCurrentProcess(), module_base, MemoryWineUnixFuncs, &handle, …)`
   then `__wine_unix_call(handle, code, args)`. Copy the two struct/enum definitions from
   `crossover-sources-25.1.1.tar.gz` (`wine/include/wine/unixlib.h`,
   `wine/include/winternl.h`) rather than guessing.

**If we did rebuild:** download `crossover-sources-25.1.1.tar.gz` (178 MB), which contains
the full Wine tree plus CodeWeavers patches. That would let us add a `dlls/steamclient/`
directory the upstream way (winebuild handles the builtin marker and the `.so`/`.dll` pair
automatically). Cost: a full Wine build, plus we would then be shipping *our own* Wine
rather than using the user's CrossOver, plus re-doing it on every CrossOver release. Not
worth it given (1)–(4) above are achievable externally.

**Residual unknown (not testable here — no mingw or Rust Windows target installed on this
machine):** whether CrossOver's builtin loader has any additional gate beyond the
`Wine builtin DLL` marker (e.g. a signature or an allowlist). Nothing in the `ntdll.so`
strings suggests one, but this needs a hello-world PE+`.so` pair to confirm. That is the
first thing to build.

---

## 4. Alternative transports

### (a) TCP loopback socket from Winsock inside the bottle

- **Works?** Yes. `lib/wine/x86_64-unix/ws2_32.so` is present and is a thin unixlib over BSD
  sockets in the *same* process, so a PE `connect()` to `127.0.0.1` reaches any native macOS
  listener. (The bottle's own Steam.exe demonstrably networks through this path.)
  `ws2_32.dll` advertises `AF_INET`, `AF_IPX`, `AF_IRDA` — note **`AF_UNIX` is not
  supported** (`grep AF_UNIX`/`sun_path` on both `ws2_32.so` and `ws2_32.dll` → no hits), so
  loopback TCP is the only socket option.
- **Arch boundary for free?** Yes — sockets are arch-agnostic. The native helper can be
  arm64 native, which is the one real advantage of this route.
- **Latency:** measured on this machine, 20 000 32-byte ping-pongs with `TCP_NODELAY`:
  ```
  x86_64 (Rosetta):  TCP loopback RTT: 18.25 us/call ;  same-proc pipe: 0.59 us/call
  arm64 native:      TCP loopback RTT: 18.88 us/call ;  same-proc pipe: 0.42 us/call
  ```
  **~18 µs per round trip** either way. At 60 fps that is ~0.1 % of a frame per call — fine
  for tens of calls per frame, bad for hundreds, and hopeless for anything the game polls in
  a tight loop.
- **Breaks on CrossOver update?** Essentially never. Winsock is Wine's most stable surface.
- **Verdict: viable fallback / bootstrap.** Also the right choice for the control channel
  (handshake, helper lifecycle) even if the hot path goes through a unixlib.

### (b) File / named-pipe path through `dosdevices`

- The bottle already maps the whole host filesystem: `dosdevices/z: -> /` and
  `dosdevices/y: -> /Users/david`, plus `c: -> ../drive_c`. So a PE can open
  `Z:\tmp\anything` and hit a real macOS file. `ntdll.dll` also exports
  `wine_nt_to_unix_file_name` / `wine_unix_to_nt_file_name` for path translation.
- **Windows named pipes do NOT work for this.** Wine's `\\.\pipe\…` namespace lives inside
  `wineserver`, not in the macOS filesystem; a native macOS process cannot open one. And
  `AF_UNIX` is absent from ws2_32 (see (a)), so a UNIX-domain socket at a `Z:`-visible path
  is not reachable from the PE side either.
- What is left is **shared files / mmap'd files**, which means polling or filesystem events.
- **Arch boundary for free?** Yes.
- **Latency:** file-based signalling is 10s–100s of µs plus polling jitter; strictly worse
  than (a) with more failure modes.
- **Breaks on update?** Very unlikely — `z:`/`y:` are CrossOver defaults.
- **Verdict: not a transport.** Useful only for out-of-band things: dropping a socket-port
  file, config, or logs where both sides can see them.

### (c) Wine unixlib (`__wine_unix_call`) — see §3

- **Works?** Yes, mechanism fully present, library validation disabled, `DllPath`
  configurable per bottle, and CrossOver's own `winelib.dll`/`winelib.so` is a live example.
- **Arch boundary for free?** No — and this is the key constraint: the unixlib **must be
  x86_64**, because that is what the Wine host process is. Verified above that x86_64 is
  sufficient to load `steamclient.dylib`.
- **Latency:** an in-process call through a dispatcher — **sub-microsecond**, same order as
  the 0.42–0.59 µs same-process pipe number above, and realistically much less since there
  is no syscall. This is the only option that can carry a high-frequency Steamworks vtable.
- **Breaks on update?** Moderate risk. The unixlib ABI (`__wine_unix_call_funcs` shape,
  `MemoryWineUnixFuncs` info class) has been stable since Wine 7 but *is* internal and has
  changed before. A CrossOver major bump could require a rebuild of just the `.so` — cheap,
  but it will need testing per release. Also: if CrossOver ever ships an `aarch64-unix`
  tree, we would need an arm64 `.so` too (§2).
- **Verdict: primary transport.**

### (d) CrossOver-specific native interop

Two real, shipped mechanisms:

- **`__wine_unix_spawnvp`**, exported from the PE `ntdll.dll`. Used by CrossOver's own
  `lib/wine/x86_64-windows/cxnative.exe` (its import table lists
  `__wine_unix_spawnvp`, `wine_get_unix_file_name`, and `winelib.dll!winelib_chdir`) to run
  arbitrary native macOS commands from inside the bottle. This is exactly what we need to
  **launch a native helper process** from the PE side. `winemapi.dll` uses it too.
- **`winelib.dll`** (§3): `winelib_execute_command`, `winelib_wait_child_pipe`,
  `winelib_wait_child_pipe_setup`, `winelib_path_unix_to_windows`,
  `winelib_get_home_directory`. Reachable from any PE via
  `LoadLibrary("winelib.dll")` + `GetProcAddress` (no import lib is shipped, so
  `GetProcAddress` it is). `winelib_wait_child_pipe*` in particular suggests a
  ready-made parent/child pipe channel to a native process.
- Also present: `CX_LAUNCH_NOTIFY_SOCKET` (string in `winelib.dll`) — CrossOver's own
  launch-notification socket. Internal; don't build on it.
- **Arch boundary for free?** `__wine_unix_spawnvp` spawns a normal macOS process, which can
  be arm64 native. So yes, for the *spawned helper*.
- **Latency:** spawn cost only (milliseconds, once). The subsequent channel is whatever you
  build on top — i.e. falls back to (a) or a pipe.
- **Breaks on update?** These are CodeWeavers-specific, undocumented, and could be renamed
  or removed with no notice. `__wine_unix_spawnvp` is upstream-Wine-ish and safer than
  `winelib.dll`.
- **Verdict: use `__wine_unix_spawnvp` for helper lifecycle if we ever need a helper
  process; do not depend on `winelib.dll` for data transport.**

### Recommended shape

| concern | choice |
|---|---|
| hot Steamworks calls | unixlib (c), x86_64 `.so`, `steamclient.dylib` loaded in-process |
| helper process launch (if needed) | `__wine_unix_spawnvp` (d) |
| control / fallback / debugging | TCP loopback, 127.0.0.1 only (a) |
| config, port handoff, logs | `Z:` path (b) |

---

## 5. What would make the PE DLL load

### The exact lookup `steam_api64.dll` performs

Reverse-read from
`…/Steam-2/drive_c/Program Files (x86)/Steam/steamapps/common/Project Spark/steam_api64.dll`
(the shipped Valve `steam_api64.dll`, pdb path
`c:\buildslave\steam_rel_client_win64\build\src\steam_api\win64\Release\steam_api64.pdb`).
Contiguous string block at offsets 158840–160700, in order:

```
158840  steamclient.dll
158856  steamclient64.dll
159416  CreateInterface
159432  HKEY_LOCAL_MACHINE
159464  HKEY_CURRENT_USER
159496  HKEY_CLASSES_ROOT
159528  SteamClientDll64
159552  Software\Valve\Steam\ActiveProcess
159592  [S_API] SteamAPI_Init(): Loaded local '%s' OK.
159648  [S_API] SteamAPI_Init(): SteamAPI_IsSteamRunning() did not locate a running instance of Steam.
159744  [S_API] SteamAPI_Init(): Could not determine Steam client install directory.
159824  [S_API] SteamAPI_Init(): Sys_LoadModule failed to load: %s
159888  [S_API] SteamAPI_Init(): Loaded '%s' OK.  (First tried local '%s')
160016  [S_API FAIL] SteamAPI_Init() failed; unable to locate interface factory in %s.
160096  SteamClient017
160144  SteamClient020
160160  [S_API FAIL] SteamAPI_Init() failed; connect to global user failed.
160232  [S_API FAIL] SteamAPI_Init() failed; create pipe failed.
160304  [S_API FAIL] SteamAPI_Init() failed; no appID found.
160480  SteamAppId
160496  SteamGameId
160520  SteamOverlayGameId
160568  steam_appid.txt
160584  steam://run/%u
160624  InstallPath
160640  Software\Valve\Steam
160664  \steam.exe
```

So the sequence a shim must satisfy is:

1. Try to load `steamclient64.dll` **from the game's own directory** ("Loaded local '%s' OK").
   *This is the cheapest hook we have: dropping our shim next to the game EXE short-circuits
   everything below.*
2. `SteamAPI_IsSteamRunning()` must return true.
3. Read `HKCU\Software\Valve\Steam\ActiveProcess` → `SteamClientDll64` (REG_SZ, full path)
   and load it. Fallback: `HKLM/HKCU\Software\Valve\Steam` → `InstallPath`, append
   `\steam.exe` / the dll name.
4. `GetProcAddress(hmod, "CreateInterface")`, then request `SteamClient020` (and
   `SteamClient017` for older games).
5. `CreateSteamPipe()` then `ConnectToGlobalUser()` must both succeed.
6. AppID must come from the environment (`SteamAppId` / `SteamGameId`) or `steam_appid.txt`.

Confirmed byte-exact via `re.finditer` on the DLL: `SteamClientDll64` @ 159528,
`Software\Valve\Steam\ActiveProcess` @ 159552, `steamclient64.dll` @ 158856,
`InstallPath` @ 160624, `Software\Valve\Steam` @ 160640. No UTF-16 variants exist —
all ANSI.

### What this bottle currently has

`~/Library/Application Support/CrossOver/Bottles/Steam-2/user.reg` lines 981–1004
(HKEY_CURRENT_USER):

```
[Software\\Valve\\Steam] 1785689428
"AlreadyRetriedOfflineMode"=dword:00000000
"AutoLoginUser"="davidhaverson"
"CompletedOOBEStage1"=dword:00000001
"GPUAccelWebViews"=dword:00000000
"Language"="english"
"Rate"="30000"
"Restart"=dword:00000000
"RunningAppID"=dword:00000000
"SourceModInstallPath"="C:\\Program Files (x86)\\Steam\\steamapps\\sourcemods"
"StartupModeTmp"=dword:00000007
"StartupModeTmpIsValid"=dword:00000000
"SteamExe"="c:/program files (x86)/steam/steam.exe"
"SteamPath"="c:/program files (x86)/steam"
"SuppressAutoRun"=dword:00000000

[Software\\Valve\\Steam\\ActiveProcess] 1785704753
"ActiveUser"=dword:00000000
"pid"=dword:00000000
"SteamClientDll"="C:\\Program Files (x86)\\Steam\\steamclient.dll"
"SteamClientDll64"="C:\\Program Files (x86)\\Steam\\steamclient64.dll"
"Universe"="Public"
```

Note the forward slashes and lowercase in `SteamExe`/`SteamPath` — that is what real Windows
Steam writes; reproduce it verbatim.

Per-app keys exist under `[Software\\Valve\\Steam\\Apps\\<appid>]` with
`"Installed"`, `"Running"`, `"Updating"` dwords and `"Name"` — e.g. lines 1043–1048:

```
[Software\\Valve\\Steam\\Apps\\3215050]
"Installed"=dword:00000001
"Name"="Surviving Mars: Relaunched"
"Running"=dword:00000000
"Updating"=dword:00000000
```

`system.reg` lines 86893–86906 (HKEY_LOCAL_MACHINE) and 92630–92653
(`Wow6432Node`, the one 32-bit code sees):

```
[Software\\Valve\\Steam]
"BetaName"=""
"ClientLauncherType"=dword:00000000
"InstallPath"="C:\\Program Files (x86)\\Steam"
"SteamPID"=dword:00000000
"Universe"="Public"
"Version"=hex(b):00,61,44,6a,00,00,00,00

[Software\\Wow6432Node\\Valve\\Steam]
"BetaName"=""
"ClientLauncherType"=dword:00000000
"InstallPath"="C:\\Program Files (x86)\\Steam"
"Language"="english"
"SteamPID"=dword:00000000
"Universe"="Public"
"Version"=hex(b):00,61,44,6a,00,00,00,00
```

Also present, for completeness: an Uninstall entry with
`"Publisher"="Valve Corporation"`, `"UninstallString"="C:\\Program Files (x86)\\Steam\\uninstall.exe"`
(`system.reg` ~91707).

**Caveat about this bottle:** it contains a *real* Windows Steam install
(`drive_c/Program Files (x86)/Steam/` with `steam.exe` 5.7 MB, `steamclient.dll` 21 MB,
`steamclient64.dll` 26 MB, `tier0_s64.dll`, `vstdlib_s64.dll`, `GameOverlayRenderer64.dll`),
so these keys were written by the real client. Our shim must **synthesise** the same keys in
a bottle with none of those files. Everything above is a REG_SZ / REG_DWORD we can write
ourselves; nothing here is generated by the Steam binary in a way we can't reproduce.

Current state shows Steam *not running* in the bottle: `ActiveProcess\pid = 0`,
`ActiveUser = 0`, `SteamPID = 0`, `RunningAppID = 0`.

### Running-Steam markers

- The primary marker `SteamAPI_IsSteamRunning()` uses is
  `HKCU\Software\Valve\Steam\ActiveProcess\pid` — it must be non-zero and name a live
  process. Under Wine, "live process" means a live *Wine* process, which our shim can
  arrange (a keepalive PE — note CrossOver even ships `cxkeepalive.exe`) — or, more simply,
  set `pid` to the game's own PID.
- `ActiveUser` must be non-zero for a logged-in user; `Universe` = `"Public"`.
- There is no filesystem lock file in the bottle. No `.crash`/`.pid` marker files under
  `drive_c/Program Files (x86)/Steam/`.
- The interface names the macOS side can serve are a good match: scanning
  `steamclient.dylib` (54 MB) yields `SteamClient006`…**`SteamClient023`**,
  `SteamUser004`…`SteamUser023`, `SteamFriends001`…`018`, `SteamUserStats001`…`013`,
  `SteamApps001`…`009`, `SteamRemoteStorage001`…`016`, `SteamUtils001`…`011`,
  `SteamInput001`…`007`. The `SteamClient020` the PE `steam_api64.dll` asks for **is
  present** in the native macOS client library.

---

## 6. Risks

**CrossOver auto-updates.**
`SUFeedURL = https://www.codeweavers.com/xml/versions/cxmac.xml` (Sparkle) in
`CrossOver.app/Contents/Info.plist`, signed with `SUPublicEDKey`. Current user prefs
(`defaults read com.codeweavers.CrossOver`): `SUEnableAutomaticChecks = 1`,
**`SUAutomaticallyUpdate = 0`**, `SULastCheckTime = 2026-08-02 09:28:29 +0000`. So updates
are *checked* automatically but *applied* only on user confirmation — we get a warning
before the ground moves. Still:
- Anything we write **inside `CrossOver.app`** is destroyed by an update (and would break
  the bundle's code signature immediately anyway — `Sealed Resources version=2 rules=13
  files=6574`). **Never write into the app bundle.** Keep our `x86_64-windows/` +
  `x86_64-unix/` pair in our own directory and reach it via `cxbottle.conf` `DllPath`.
- A CrossOver major version bump can change the Wine base (e.g. 10.x → 11.x) and with it the
  unixlib ABI. Pin a known-good CrossOver version in testing, verify the `wineloader
  --version` string at runtime, and fail loudly rather than silently on mismatch.
- Bottles also carry `"Version" = "25.1.1.38624"` in `cxbottle.conf` and get auto-upgraded
  by CrossOver (the `wine` script runs a `--ref-dir` updater against the managed prefix,
  lines 567–621). That updater rewrites bottle config — our `[Wine] "DllPath"` and
  `[EnvironmentVariables]` entries may not survive a bottle upgrade. Make the setup
  re-appliable and idempotent.

**DLL overrides in the bottle.**
`user.reg:1139 [Software\Wine\DllOverrides]` already has ~100 global entries plus
per-application `[Software\Wine\AppDefaults\<exe>\DllOverrides]` blocks. There is currently
**no** override for `steamclient`/`steamclient64`, and no `AppDefaults\steam.exe` block.
Our builtin `steamclient64.dll` would need `"steamclient64"="builtin"` (probably scoped to
the game exe via `AppDefaults`) so Wine prefers ours over any native DLL the game ships or
that a real Steam install leaves behind. Note `ntdll.so` carries
`"%s has prefer-native flag, ignoring builtin"` — a PE with the prefer-native flag set will
defeat us, so make sure ours does not have it. Also note the override precedence trap: if a
`steamclient64.dll` exists next to the game EXE, `steam_api64.dll` loads it *directly by
path* (step 1 in §5) and Wine's override machinery is bypassed entirely — which is a feature
for us, not a bug.

**Code signing / hardened runtime / notarisation.**
- Good news: `wineloader` has `com.apple.security.cs.disable-library-validation` (plus
  `allow-unsigned-executable-memory` and `disable-executable-page-protection`), so an
  unsigned x86_64 `.so` loads fine. This is the single biggest reason the unixlib route is
  practical on macOS at all.
- If we instead ship a **separate native helper**, it will be Gatekeeper-scrutinised on
  first launch if it arrives with a quarantine xattr. Distributing it unsigned means users
  hit "cannot be opened". Signing with a Developer ID + notarising is the clean answer; an
  ad-hoc signature plus removing the quarantine attribute works for local development.
- **Do not attempt to inject into the running Steam client.** macOS 26 has SIP + hardened
  runtime + library validation on Steam.app; `task_for_pid` on another user process requires
  a `com.apple.security.cs.debugger` entitlement and TCC approval. Any design that needs
  code inside Steam.app's address space should be considered dead on macOS.
- TCC attribution: Wine processes are children of `CrossOver.app`, so filesystem-access
  prompts are attributed to CrossOver. A helper we spawn via `__wine_unix_spawnvp` inherits
  the same responsible process, which is convenient (no new prompts) but means our helper's
  disk access is governed by CrossOver's TCC grants.

**macOS 26 specifics.**
- **Rosetta 2 is on a published deprecation clock and this entire design sits on top of it.**
  macOS 26 Tahoe is the last release supporting Intel Macs; Rosetta 2 remains usable on
  Apple Silicon through macOS 27, with general support ending in macOS 28. macOS 26.4
  already shows users a warning banner when they launch a Rosetta-dependent app. Apple has
  said a *subset* of Rosetta will persist beyond that specifically for older unmaintained
  **games** — which is arguably exactly this use case — but that is a promise about games,
  not about a general-purpose x86_64 translation layer, and CrossOver is not a game.
  **Mitigation:** keep the native-side bridge's arch a build-time switch, not a baked
  assumption; the CrossOver launcher already probes for `lib/wine/aarch64-unix/ntdll.so`, so
  the day CodeWeavers ships an arm64 host we need an arm64 `.so` and nothing else. Design the
  unixlib so it can be compiled both ways from day one.
- The wineloader binary is thin x86_64 with no arm64 slice, so if Rosetta goes away this
  CrossOver version stops running entirely — that is CodeWeavers' problem to solve, but our
  release matrix must track it.

**Other.**
- The `Steam-2` bottle currently has a real Windows Steam installed. Our Level-B shim assumes
  the opposite. Testing must happen in a **clean bottle** with no `Program Files (x86)\Steam`,
  or the real `steamclient64.dll` will keep winning the lookup.
- `dosdevices` currently has stale volume mappings (`d:` → `/Volumes/calibre-8.7.0`,
  `e:`–`i:` → ejected volumes, registered as `"floppy"` in
  `system.reg [Software\Wine\Drives]`). Harmless, but don't rely on drive letters other than
  `c:`, `y:`, `z:`.
- No mingw-w64, no zig, and no Rust `x86_64-pc-windows-*` std are installed on this machine
  (`rustc --print target-list` lists the targets but `$(rustc --print sysroot)/lib/rustlib`
  contains only `aarch64-apple-darwin`). Building the PE half requires installing a
  toolchain first — the UNIX half builds today with stock `clang -arch x86_64`.

---

## Sources

### Local paths inspected (all read-only)

CrossOver install:
- `/Users/david/Applications/CrossOver.app/Contents/Info.plist`
- `/Users/david/Applications/CrossOver.app/Contents/MacOS/CrossOver`
- `/Users/david/Applications/CrossOver.app/Contents/SharedSupport/CrossOver/README`
- `/Users/david/Applications/CrossOver.app/Contents/SharedSupport/CrossOver/etc/CrossOver.conf` (lines 240–320)
- `/Users/david/Applications/CrossOver.app/Contents/SharedSupport/CrossOver/CrossOver-Hosted Application/wine` (Perl launcher; lines 645–720)
- `/Users/david/Applications/CrossOver.app/Contents/SharedSupport/CrossOver/CrossOver-Hosted Application/wineloader`
- `/Users/david/Applications/CrossOver.app/Contents/SharedSupport/CrossOver/CrossOver-Hosted Application/wineserver`
- `/Users/david/Applications/CrossOver.app/Contents/SharedSupport/CrossOver/CrossOver-Hosted Application/cxnativeopen`
- `/Users/david/Applications/CrossOver.app/Contents/SharedSupport/CrossOver/lib/wine/{i386-unix,i386-windows,x86_64-unix,x86_64-windows}/`
- `.../lib/wine/x86_64-unix/{ntdll,win32u,winemac,ws2_32,winelib,winecoreaudio}.so`
- `.../lib/wine/x86_64-windows/{ntdll.dll,ws2_32.dll,winemac.drv,winelib.dll,cxnative.exe}`
- `.../lib64/` (all `.dylib`), `.../lib64/apple_gptk/external/D3DMetal.framework`

Bottle (`Steam-2`), read-only:
- `~/Library/Application Support/CrossOver/Bottles/Steam-2/cxbottle.conf` (lines 12, 114, 118, 318–328)
- `~/Library/Application Support/CrossOver/Bottles/Steam-2/user.reg` (lines 981–1060, 1070–1129, 1139+)
- `~/Library/Application Support/CrossOver/Bottles/Steam-2/system.reg` (lines 86893–86925, 91707, 92630–92653)
- `~/Library/Application Support/CrossOver/Bottles/Steam-2/dosdevices/`
- `~/Library/Application Support/CrossOver/Bottles/Steam-2/drive_c/Program Files (x86)/Steam/`
- `~/Library/Application Support/CrossOver/Bottles/Steam-2/drive_c/Program Files (x86)/Steam/steamapps/common/Project Spark/steam_api64.dll`

Native Steam:
- `/Applications/Steam.app/Contents/MacOS/steam_osx`
- `~/Library/Application Support/Steam/Steam.AppBundle/Steam/Contents/MacOS/steamclient.dylib`
- `~/Library/Application Support/Steam/Steam.AppBundle/Steam/Contents/MacOS/libsteam.dylib`

Scratch test artifacts (throwaway, outside the repo):
- `/private/tmp/claude-501/-Users-david-perso-macos-steam/a317f159-e923-49b7-afee-c9c66ff7d444/scratchpad/{t.c,t.so,d.c,d_x86,d_arm,bench.c,bench_x86,bench_arm}`

### Commands run

```sh
# version / provenance
/usr/libexec/PlistBuddy -c "Print :CFBundleShortVersionString" -c "Print :CFBundleVersion" \
    ~/Applications/CrossOver.app/Contents/Info.plist
'.../CrossOver-Hosted Application/wineloader' --version      # -> wine-10.0-8474-g427b76fe09f
'.../CrossOver-Hosted Application/wineserver' --version      # -> Wine 10.0
strings -a .../lib/wine/x86_64-unix/ntdll.so | grep -iE 'wine.*[0-9]+\.[0-9]+'

# architecture
uname -m; sysctl -n machdep.cpu.brand_string; sw_vers
file  '.../CrossOver-Hosted Application/wineloader'
file  '.../CrossOver-Hosted Application/wineserver'
lipo -archs .../lib/wine/x86_64-unix/{ntdll,win32u,winemac,ws2_32,winelib}.so
lipo -archs ~/Applications/CrossOver.app/Contents/MacOS/CrossOver
lipo -archs '.../Steam.AppBundle/Steam/Contents/MacOS/steamclient.dylib'
find ~/Applications/CrossOver.app -maxdepth 6 -type d \( -name '*arm64*' -o -name '*aarch64*' \)
ls -la /Library/Apple/usr/libexec/oah
otool -hv .../lib/wine/x86_64-unix/ws2_32.so
otool -L  .../lib/wine/x86_64-unix/ws2_32.so

# unixlib mechanism
nm -m  .../lib/wine/x86_64-unix/ntdll.so   | grep -iE 'unix_call|dlopen'
nm -gU .../lib/wine/x86_64-unix/winelib.so
nm -gU .../lib/wine/x86_64-unix/winecoreaudio.so
strings -a .../lib/wine/x86_64-windows/ntdll.dll | grep -E '^__wine|Unix'
python3  # byte-context dumps around 'Wine builtin DLL', '/x86_64-unix', 'WINEDLLPATH',
         # 'get_builtin_unix_funcs', 'invalid .so library' in ntdll.so; DOS-stub read at 0x40
find ~/Applications/CrossOver.app \( -name '*.h' -o -name '*.def' -o -name '*.a' \)   # no SDK

# code signing
codesign -dvvv --entitlements - '.../CrossOver-Hosted Application/wineloader'
codesign -dvvv --entitlements - ~/Applications/CrossOver.app
spctl -a -vvv ~/Applications/CrossOver.app
defaults read com.codeweavers.CrossOver | grep -iE 'SU|update'

# steam_api64 lookup sequence
python3   # re.finditer for SteamClientDll64 / ActiveProcess / InstallPath, and a printable
          # string dump of offsets 158700-161200 in steam_api64.dll
grep -n -A20 -i Valve system.reg ; grep -n -A25 '^\[Software\\\\Valve' user.reg

# empirical tests
clang -arch x86_64 -dynamiclib -o t.so t.c -Wl,-install_name,@rpath/t.so ; nm -gU t.so
clang -arch x86_64 -o d_x86 d.c ; clang -arch arm64 -o d_arm d.c
./d_x86 '.../steamclient.dylib' ; ./d_arm '.../steamclient.dylib'
clang -arch x86_64 -O2 -o bench_x86 bench.c ; clang -arch arm64 -O2 -o bench_arm bench.c
./bench_x86 ; ./bench_arm
ps aux | grep -i '[s]team'
```

### URLs

- CrossOver source tarball for the exact installed build (HTTP 200, 178,524,083 bytes,
  Last-Modified 2025-09-15):
  <https://media.codeweavers.com/pub/crossover/source/crossover-sources-25.1.1.tar.gz>
  (directory index <https://media.codeweavers.com/pub/crossover/source/> returns 403; direct
  filenames work. 25.1.0 and 25.0.0 also verified present.)
- [Source Code | CrossOver Mac and Linux | CodeWeavers](https://www.codeweavers.com/crossover/source)
- [CrossOver (software) — Wikipedia](https://en.wikipedia.org/wiki/CrossOver_(software))
- Sparkle update feed used by this install: <https://www.codeweavers.com/xml/versions/cxmac.xml>
- Rosetta 2 deprecation timeline:
  - [What's new for enterprise in macOS Tahoe 26 — Apple Support](https://support.apple.com/en-us/124963)
  - [macOS Tahoe 26.4 Displays Warnings for Apps That Won't Work After Rosetta 2 Support Ends — MacRumors](https://www.macrumors.com/2026/02/16/macos-tahoe-26-4-rosetta-2-warnings/)
  - [macOS Tahoe 26.4 warns if your apps won't work when Rosetta 2 disappears — AppleInsider](https://appleinsider.com/articles/26/02/16/macos-tahoe-264-warns-if-your-apps-wont-work-when-rosetta-2-dies)
  - [macOS Rosetta transition (end of life) — PaperCut](https://www.papercut.com/kb/Main/macos-rosetta-transition-end-of-life/)
