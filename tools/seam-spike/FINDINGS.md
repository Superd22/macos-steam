# Seam spike — the transport fork, settled by evidence (#8, stage 1 of #10)

**What this is.** A minimal, self-contained proof that a Windows PE DLL inside the
shipped CrossOver bottle can reach native macOS code **in-process**, through Wine's
`__wine_unix_call` unixlib seam, with an **unsigned third-party `.so`** and **no Wine
rebuild**. It is the smoke test #4 said had to be built before the transport could be
chosen ("whether CrossOver's builtin loader has any additional gate beyond the
`Wine builtin DLL` marker … that is the first thing to build").

**Run on:** 2026-08-13, CrossOver 25.1.1 (Wine `wine-10.0-8474`), Apple Silicon, macOS 26.5.2.

## Result: PASS

```
SPIKE OK: builtin loaded at 00006FFFFF2D0000     <- PE accepted as a builtin (high address range)
SPIKE OK: unix call returned
  magic_out    = 5ead5e20be7cebcf (expect 5ead5e20be7cebcf)   <- 0x5EA151DE5EA151DE ^ 0xC0FFEE0DDBA11
  unix_pid     = 3999 (win pid 32)                             <- unix side ran; same process, two pid views
  unix_sysname = Darwin
SPIKE PASS
```

The magic XOR round-trips, and the unix half returns its real macOS `getpid()` and
`uname().sysname` — so control genuinely crossed into native code and came back with a
value only native code could produce.

## What it proves

1. **No loader gate beyond the `0x40` marker.** A mingw-built PE with `"Wine builtin DLL\0"`
   stamped at file offset `0x40` is loaded **as a builtin** (address `0x6FFFFF2D0000`, the
   builtin range; the trace says `build_module … : builtin`). CrossOver adds no allowlist,
   signature check, or other gate. This closes the one residual unknown from
   `crossover-bridge-surface.md` §3.
2. **An unsigned x86_64 `.so` loads into the Wine process and is called.** `wineloader`'s
   `disable-library-validation` entitlement holds in practice: our ad-hoc/unsigned dylib
   `dlopen`s and its `__wine_unix_call_funcs[0]` runs. No rebuild of Wine, no CrossOver
   source, no SDK — just the two exported symbol arrays.
3. **It is a real in-process call, not IPC.** The unix side sees the same process the PE
   runs in (PE-side Wine pid 32 vs host pid 3999 are two views of one process image). No
   socket, no serialisation, no second process.

Together these dissolve every reason the ticket kept the native-helper option alive:
the arch boundary does not force a process (#4), the callback direction is a poll the game
drives (#3 §6), and the unixlib is buildable against the shipped CrossOver with no rebuild.

## The deployment rule this pinned down (and corrects)

`lsteamclient-mechanics.md` §1.2 (reading upstream wine-10.0) said the `.so` is found by
rewriting `foo.dll` → `<dll_path>/x86_64-**unix**/foo.so`. **That is not what CrossOver's
loader does for a builtin found via `WINEDLLPATH`.** Here it derives the `.so` as
`<WINEDLLPATH-entry>/<name>.so` — the **same directory** as the builtin PE on the DllPath,
`.dll` → `.so`, no `-windows` → `-unix` substitution. Concretely, the working layout is:

- builtin PE at a real path the PE loader already searches (this spike: `system32`; in
  production: the game dir, or the path `SteamClientDll64` points at);
- the `.so` beside the PE **inside a `WINEDLLPATH` directory** (this spike: `dist/`, added
  to `WINEDLLPATH`; per-bottle in production via `cxbottle.conf [Wine] "DllPath"`).

A bare `LoadLibrary("name.dll")` of an unregistered name **never probes `WINEDLLPATH`** — it
only walks the Windows DLL search path. So the PE must exist as a real marked file where the
loader already looks; `WINEDLLPATH` governs `.so` discovery, not PE discovery. (CrossOver's
own builtins keep the `.so` in a sibling `x86_64-unix/` dir because ntdll has that path
compiled in; third-party modules reached via `WINEDLLPATH` do not get that substitution.)

## Files

- `bridge_unix.c` — unix half: exports `__wine_unix_call_funcs` / `_wow64_funcs`; entry 0
  XORs a magic and returns `getpid()`/`uname`.
- `bridge_pe.c` — PE half: `NtQueryVirtualMemory(MemoryWineUnixFuncs)` on its own image base
  to get the unixlib handle, then `__wine_unix_call`. Step-numbered error returns.
- `spike_main.c` — console exe: `LoadLibrary` the builtin, call across, print PASS/FAIL.
- `patch_marker.py` — stamps `"Wine builtin DLL\0"` at file `0x40`.
- `build.sh` — mingw for PE, `clang -arch x86_64` for the `.so`, into `dist/`.
- `run.sh [bottle]` — deploys into a bottle and runs (default bottle `seamspike`).

## Reproduce

```sh
brew install mingw-w64                    # one-time; no Windows toolchain ships on this Mac
"$HOME/Applications/CrossOver.app/Contents/SharedSupport/CrossOver/bin/cxbottle" \
    --bottle seamspike --create --template win10_64    # throwaway bottle
./build.sh
./run.sh seamspike                        # expect: SPIKE PASS
```

## What this spike deliberately does NOT do

It does not touch `steamclient.dylib`. Loading the real Valve dylib **inside the Wine
process** (as opposed to the standalone probe #2 already ran) and making one real Steamworks
call across this seam — `CreateInterface(SteamClient020)` → `CreateSteamPipe` →
`ConnectToGlobalUser` → `BConnected`, Steam running and **online** — is stage 2, and remains
the body of #10. This spike removes the transport risk from under it; #10 now only has to
prove the dylib behaves in the Wine process environment.
