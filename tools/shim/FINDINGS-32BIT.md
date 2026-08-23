# The 32-bit shim — `steamclient.dll`, and why "could not sign in" was never about signing in

Findings for [#20](https://github.com/Superd22/macos-steam/issues/20). Measured live on
2026-08-23, CrossOver 25.1.1, Apple Silicon (M3 Pro), macOS 26.5.2, bottle `steam-shim`,
native Steam.app running and **online** (`Steam_BConnected = TRUE`, `BLoggedOn = 1`).
**Zero Windows Steam processes** at any point.

## Verdict: PASS on the reported bug. Among Us now fails later, for an unrelated reason.

The 32-bit harness, driven through the production compat-tool launch path, reproduces #11's
result on i386:

```
SteamAPI_Init() = 1
BLoggedOn=1 SteamID=76561198014230730
SetAchievement("ACH_WIN_ONE_GAME") = 1   StoreStats() = 1
CALLBACK 1103 UserAchievementStored_t  name="ACH_WIN_ONE_GAME"
CALLBACK 1102 UserStatsStored_t        gameid=480 eresult=1
VERIFY set:   achieved=1 unlock=1787506061   PASS      <- real server unlock time
VERIFY reset: achieved=0                     PASS
=== LOOP PASS ===
```

And **Among Us's own 32-bit `steam_api.dll`, against appid 945360**, initialises through the
shim and reads the real Among Us achievement list off the live server (`wins_skeld`,
`task_complete_hard`, …, 30 of them). The 64-bit path is unchanged: `=== LOOP PASS ===`.

Negative control (#13 protocol): remove `C:\shim\steamclient.dll` and delete
`SteamClientDll` → `SteamAPI_Init() = 0` in **426 ms**. The shim is what answers.

**The game itself still does not reach its menu**, but no longer for a Steam reason — see
"Where Among Us actually dies" below.

## The bug, and why the error message pointed the wrong way

Among Us is a **32-bit** title (`Among Us.exe` is PE32 i386; note `UnityCrashHandler32.exe`
and `Plugins/x86/`). It loads **`steam_api.dll`**, not `steam_api64.dll`, and that DLL looks
for a different file under a different registry value:

| Title bitness | Valve redistributable | looks for | registry value |
|---|---|---|---|
| 64-bit (Mars) | `steam_api64.dll` | `steamclient64.dll` | `SteamClientDll64` |
| 32-bit (Among Us) | `steam_api.dll` | `steamclient.dll` | `SteamClientDll` |

We shipped only the 64-bit pair, so `SteamAPI_Init` returned 0 and Among Us rendered that as
**"Could not sign in to your Steam account"** — a login-shaped message for what was really a
missing file. Nothing was wrong with the account, the connection, or the bridge.

The map's Machine facts said Mars "ships `steam_api64.dll` only — no 32-bit variant, so a
single 64-bit shim suffices." True of Mars, false of the library.

## What did NOT need doing (the ticket over-scoped it)

The ticket predicted a `ptr32<T>` conversion layer — the thing Proton spends a whole
generated wow64 surface on. **Not needed**, for two reasons that were already true:

1. **CrossOver is on new WoW64.** `lib/wine` has `i386-windows` and `x86_64-unix` but no
   `i386-unix`: 32-bit PE code runs inside a 64-bit unix process. The unix half stays x86_64
   and keeps hosting the x86_64 `steamclient.dylib`. **One `.so` serves both PE halves**,
   deployed under two basenames because ntdll derives the `.so` name from the builtin PE's.
2. **The seam ABI was already bitness-neutral.** Every params struct in `shim_abi.h` stores
   pointers as an explicit `uint64_t`, laid out widest-first, so a 32-bit PE zero-extends its
   pointers into fields at identical offsets — and zero-extension is the *correct value*, not
   a truncation, because the 32-bit PE's addresses are low addresses in that one 64-bit
   address space. All **159 sizes/offsets are identical** under `i686-mingw` and x86_64.
   `check_abi_layout.py` asserts this at build time so a future struct cannot quietly break it.

## What DID need doing

### 1. i386 thiscall is callee-cleanup — the "free stubs" property does not survive

#11 could fill every unused vtable slot with a 0-arg stub because MS-x64 is caller-cleanup.
On i386 a stub must pop *exactly* the bytes its caller pushed, or the caller's stack breaks —
with no error and no wrong answer, just a jump to garbage some frames later.

Proton already emits the number: `DEFINE_THISCALL_WRAPPER(<method>, <bytes>)`, `this`
included. `extract_vtables.py` reads it, plus the `__ASM_VTABLE` slot order and
`alloc_vtable`'s independent slot count as a cross-check; `gen_vtables.py` emits
`shim_vtables.h`. **37 interface versions, 1227 slots.** `verify_abi.py` disassembles the
i386 build and fails the build unless all **1295 entry points** pop what Proton says. It is
not a comment, it is a build step.

### 2. Vtables are per-VERSION, not per-interface

`vt_for_version()` matched on interface name and ignored the version number, so any
`SteamInput*` got one table. But `SteamInput006` (Mars) inserts
`SetInputActionManifestFilePath` at slot 2 and shifts every later slot against `SteamInput002`
(Among Us). One table for both calls the wrong method — map trap 2, exactly.

Worse, and only visible on i386: **the same method name can have a different signature across
versions.** `SteamInput002::Init(this)` vs `SteamInput006::Init(this, bExplicitlyCallRunFrame)`.
One C thunk cannot serve both — it would pop 4 bytes the 002 caller never pushed. `verify_abi.py`
caught this; it needs `iin_Init_002` / `iin_RunFrame_002`. Wiring is now resolved **by name
against each version's own table**, and a name absent from a version is reported, not dropped.

### 3. Native memory the game is handed BACK must be copied down

Direction matters. Anything the game passes **in** is a PE address that zero-extends and the
native side writes through it, on both bitnesses, for free. Anything the native side hands
**back by pointer** lives on the dylib's heap — above 4 GB on macOS (`0x7fd695b48ae0`,
straight out of our own log). A 64-bit PE holds that fine; **a 32-bit PE cannot hold it at
all.** Two new seam opcodes, `C_CopyMem` / `C_CopyStr`, copy the bytes down on the unix side
where the address is still a real pointer. Seven `const char *` returns needed it.

`CallbackMsg_t` is the one struct on this path that is not bitness-neutral, because it carries
a pointer (16 bytes on win32, 24 on x86_64). On i386 the shim hands the native side its own
64-bit struct, copies the payload into PE memory, and fills the caller's 32-bit one. The
payload *structs* need no conversion: Valve picks callback packing by **platform, not
bitness** (`PACK_SMALL` on macOS, `PACK_LARGE` on Windows), so a win32 game sees what a win64
game sees, and 1101/1102/1103 are byte-identical across all four ABIs anyway (#3 §6).

### 4. Wire the whole getter family, not the ones the first title happened to call

Only the `GetISteam*` slots Mars used were wired. Among Us asked for `GetISteamMatchmaking`,
got NULL, and stopped — a multiplayer game. `wire_getters()` now wires every `GetISteam*` slot
from the version's own table, telling the two shapes apart by Proton's byte count
(16 = `this,user,pipe,ver`; 12 = `this,pipe,ver`). Among Us acquires **25 interfaces**.

### 5. Three toolchain traps, each of which looks like a Steam failure

- **`libgcc_s_sjlj-1.dll`.** i686-mingw defaults to the SJLJ unwinder, so the DLL imported a
  runtime that is not in the bottle. The loader failed it with **err=126** and steam_api
  reported that, three layers away, as a failed sign-in. `-static -static-libgcc` fixes it;
  `build.sh` now fails the build if any mingw runtime import reappears. The 64-bit build never
  needed this, which is exactly why it was easy to miss.
- **`__thread` in a `LoadLibrary`'d DLL.** Unusable here: a thread that already existed when
  the DLL was loaded has no slot for its TLS block. Replaced with fixed `.bss` storage of
  **process** lifetime — `TlsAlloc` was tried and is worse, because freeing the block on
  `DLL_THREAD_DETACH` pulls memory out from under a string the game still holds.
- **Wine faults are reported as a bare absolute address**, and CrossOver strips the trace
  channels that would name the module (`+loaddll` produces nothing). The shim now logs its own
  base and, with `SHIM_PE_LOG` set, the whole module map — which is how `ucrtbase.dll+0x74666`
  was attributed at all.

## The harness had the same class of bug, twice

Both only visible on i386, both in code that **presents** a vtable to Steam:

1. `run_slot0`/`run_slot1`/`cb_get_size` were plain cdecl. MSVC calls them thiscall, so `self`
   arrived in ECX and the harness read `self->size` off `pvParam` — hexdumping garbage until
   it ran off the heap.
2. The two `Run` overloads were **swapped**. `CCallbackBase` declares `Run(void*)` then
   `Run(void*,bool,SteamAPICall_t)`, and MSVC emits same-name overloads in **reverse**
   declaration order — the same reversal `docs/research/steamworks-vtable-tables.md` documents
   for Steam's own interfaces. x86_64 forgave it (caller-cleanup, surplus register args simply
   ignored); i386 broke on RETURN from the callback, after its body had already run and
   printed, which is a maximally confusing place for a crash to appear.

**Rule:** anything that presents a vtable to Steam must present it thiscall, in MSVC's order —
test harnesses included.

## Where Among Us actually dies now

Not in Steam. `SteamAPI_Init` completes, all 25 interfaces resolve, and the only unmapped
calls are `Set_SteamAPI_CCheckCallbackRegisteredInProcess` and `SetWarningMessageHook` — both
of which hand Steam a **PE function pointer**, the deferred-upcall case #11 scoped out, and
neither is on the sign-in path.

The game then crashes in **Unity Addressables**, fetching its content catalog from
`unity3dusercontent.com` (`Player.log`: `TextDataProvider:Provide` →
`InternalOp:RequestOperation_completed` → `Crash!!!`), faulting inside `ucrtbase.dll+0x74666`
— a `strlen`-family scan over a heap pointer that is **not one of ours** (our string pool is
in the shim's `.bss` at base+; the faulting address is a game heap address, and its low 12
bits are identical across every run, so it is a real pointer, not garbage we produced).

That is a Unity/UnityWebRequest-under-CrossOver problem, not a bridge problem, and it wants
its own ticket. Retiring it needs a title whose first-run path does not depend on a remote
Addressables catalog.

## Reproduce

```sh
cd tools/shim && ./build.sh          # builds both bitnesses; fails on any ABI mismatch
cd ../harness && make all all32      # 32-bit harness uses Among Us's own steam_api.dll
../native-probe/connprobe-x86_64     # assert Steam_BConnected = TRUE first (map trap 1)

B="$HOME/Library/Application Support/CrossOver/Bottles/steam-shim"
cp build32/harness.exe "$B/drive_c/harness32.exe"
cp build32/steam_api.dll "$B/drive_c/"; printf 480 > "$B/drive_c/steam_appid.txt"

STEAM_COMPAT_APP_ID=480 SHIM_BOTTLE=steam-shim \
  tools/compat-tool/steamclient-shim-launch.sh waitforexitandrun 'C:\harness32.exe' loop
# expect: === LOOP PASS ===

# negative control: rm "$B/drive_c/shim/steamclient.dll" + delete SteamClientDll -> Init = 0
```

`SHIM_PE_LOG=<path>` dumps the PE side, including the module map and every unmapped vtable
slot by name — the fastest way to find what a new title wants.

## Adding a title

1. `strings -a <steam_api[64].dll> | grep -E '^(STEAM|Steam)[A-Za-z]*_?[A-Z_]*(INTERFACE_VERSION)?_?[0-9]{3}$' | sort -u`
   — mind the irregular suffixes (`..._VERSION_005`, `..._V003`), which a tighter pattern drops.
2. Add any new versions to `tools/shim/interface-versions.txt`.
3. `./build.sh --regen-vtables` (needs network + `gh`), then `./build.sh`.
