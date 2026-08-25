# The 32-bit shim — `steamclient.dll`, and why "could not sign in" was never about signing in

Findings for [#20](https://github.com/Superd22/macos-steam/issues/20). Measured live on
2026-08-23, CrossOver 25.1.1, Apple Silicon (M3 Pro), macOS 26.5.2, bottle `steam-shim`,
native Steam.app running and **online** (`Steam_BConnected = TRUE`, `BLoggedOn = 1`).
**Zero Windows Steam processes** at any point.

## Verdict: PASS. Among Us reaches its account-setup screen; the Steam path is done.

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

Among Us itself now boots to its **date-of-birth age gate** (`DOBEnterScreen`), blocked only
on a human entering a date. Getting there took two more fixes found after the first write-up
of this document — both ours, both i386 pointer-width — see "Two bugs this document originally
got wrong" below.

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
  the DLL was loaded has no slot for its TLS block, so reads come back as garbage. Replaced
  with explicit `TlsAlloc`/`TlsGetValue`, where the slot is allocated on first touch whenever
  the thread was born. **This did not fix the Unity crash**, though the first version of this
  document and the code comment both said it did; see below.
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

## Two bugs this document originally got wrong

The first version of this write-up said Among Us died in **Unity Addressables** fetching its
content catalog, "a Unity/UnityWebRequest-under-CrossOver problem, not a bridge problem", and
recommended splitting it out. That was wrong. Both remaining blockers were ours, and both were
the same i386 pointer-width mistake wearing different clothes.

### 1. The crash: a native pointer formatted with `%s`

The evidence was already on disk and went unread. **All seven** Unity crash dumps carry the
same frame chain, across three different builds of the shim:

```
0x7A2C4666 (ucrtbase)      <- fault
0x7A2B4943 (ucrtbase)
0x7A2B6758 (ucrtbase)
0x7A26F596 (ucrtbase)
0x74045B54 (steamclient)   <- OURS. base 0x74030000, so RVA 0x15B54
0x7617DE59 (gameassembly)
```

`GameAssembly` called *us*, and we called the CRT. Disassembling RVA `0x15B54` lands on the
instruction immediately after `call ___stdio_common_vsprintf`, inside the shim's own
`vsnprintf`. And the last line the unix half ever wrote was `GetCurrentGameLanguage() -> english`.

Three sites logged the **raw native pointer** rather than the copied-down string:

```c
dbg("shim: GetCurrentGameLanguage() -> %s", p.ret ? (const char*)(uintptr_t)p.ret : "(null)");
```

`native_str()` (§3 above) exists precisely to copy that memory down, and the `return` on the
next line used it — but the log line did not. On x86_64 the cast is a no-op, which is why it
survived the entire 64-bit spike. On i386 it truncates a `0x7fce8305...` macOS heap address to
32 bits and `%s` walks strlen into whatever is there.

Two aggravating factors worth remembering:

- **`dbg()` formats unconditionally.** It calls `vsnprintf` before it ever checks whether
  `SHIM_PE_LOG` is set, so this faulted on every run, logged or not. A diagnostic that is
  supposed to be inert when switched off was the thing doing the crashing.
- **Fixing the harder bug first hid the easier one.** The TLS rework landed in the same commit,
  aimed at this same crash, and was credited with fixing it. It never touched it: the DLL that
  still crashed at 19:53 already contained the `TlsAlloc` code. One line of source held two
  independent i386 pointer-width bugs, and repairing the subtle one left the blunt one in place.

**Rule:** on a bitness seam, a value that needs conversion to be *returned* needs the same
conversion to be *logged*. Log lines are code.

### 2. The hang: EOS wanted a Steam encrypted app ticket

With the crash gone the game reached its menu and then sat on "loading" until
`EOSManager:ShowTimeout()` fired. Two lines explain it:

```
/tmp/au2.pe.log:  shim: SteamUser021 slot 20 RequestEncryptedAppTicket (unmapped)
Player.log:       [Network] > [EOSManager] > Auth with Steam
```

**Among Us does not use Steam for its account.** It authenticates to Epic Online Services, and
EOS's "Auth with Steam" path asks Steam for an **encrypted app ticket**. Left as a numbered
stub, `RequestEncryptedAppTicket` returned 0, no `SteamAPICall_t` was ever issued, the
`EncryptedAppTicketResponse_t` never arrived, and the game waited for a callback that could not
come. A stub that returns 0 is not a no-op for an **async** method: the caller does not read a
wrong answer, it waits forever for a right one.

Slots 20 and 21 are now wired (`iu_RequestEncryptedAppTicket` / `iu_GetEncryptedAppTicket`).
`steam_ifaces.h`'s `ISteamUser` is transcribed out to slot 21 to place them; SteamUser021 has
**no overloaded method names**, so Proton's MSVC order is also the native Itanium order, and
slots 0-6 already agreed with it — that cross-check is what makes the transcription safe
(map trap 2). Both buffers belong to the game, so they are PE addresses that zero-extend
across the seam; the copy-down path is not involved.

The async round-trip works end to end:

```
RequestEncryptedAppTicket(cb=0) -> call=8863564181403634078
GetEncryptedAppTicket(max=1024) -> 1 (159 bytes)
```

### The strongest end-to-end proof the bridge has had

Epic's live backend then decrypted that ticket and read the right account out of it:

```
identityProviderId: steam  accountId: 76561198014230730
errors.com.epicgames.eos.auth.user_not_found  (HTTP 404)
```

That is **not** a failure. It is a third party's servers cryptographically validating a Steam
ticket our shim produced, resolving it to the correct SteamID, and reporting only that no Epic
account is linked to it yet. Hence `DOBEnterScreen`: the game is asking for a date of birth so
it can create one. No mock, no Windows Steam, no replayed capture could produce that response.

Addressables, meanwhile, never was the problem — `Player.log` now reads
`Addressables - We have the catalog cached so we don't need to download it again`.

## Reproduce

```sh
cd src/shim && ./build.sh          # builds both bitnesses; fails on any ABI mismatch
cd ../harness && make all all32      # 32-bit harness uses Among Us's own steam_api.dll
../native-probe/connprobe-x86_64     # assert Steam_BConnected = TRUE first (map trap 1)

B="$HOME/Library/Application Support/CrossOver/Bottles/steam-shim"
cp build32/harness.exe "$B/drive_c/harness32.exe"
cp build32/steam_api.dll "$B/drive_c/"; printf 480 > "$B/drive_c/steam_appid.txt"

STEAM_COMPAT_APP_ID=480 SHIM_BOTTLE=steam-shim \
  src/compat-tool/steamclient-shim-launch.sh waitforexitandrun 'C:\harness32.exe' loop
# expect: === LOOP PASS ===

# negative control: rm "$B/drive_c/shim/steamclient.dll" + delete SteamClientDll -> Init = 0
```

And Among Us itself, through the same launch path:

```sh
AU="$HOME/Library/Application Support/Steam/steamapps/common/Among Us/Among Us.exe"
rm -f "$B/drive_c/steam_appid.txt"          # let the title supply its own appid
SHIM_UNIX_LOG=/tmp/au.unix.log SHIM_PE_LOG=/tmp/au.pe.log \
STEAM_COMPAT_APP_ID=945360 SHIM_BOTTLE=steam-shim \
  src/compat-tool/steamclient-shim-launch.sh waitforexitandrun "$AU"
# expect in /tmp/au.unix.log:
#   RequestEncryptedAppTicket(cb=0) -> call=<nonzero>
#   GetEncryptedAppTicket(max=1024) -> 1 (159 bytes)
# expect on screen: the DOBEnterScreen age gate, and NO directory created under
#   "$B/drive_c/users/crossover/AppData/Local/Temp/Innersloth/Among Us/Crashes"
```

`SHIM_PE_LOG=<path>` dumps the PE side, including the module map and every unmapped vtable
slot by name — the fastest way to find what a new title wants. Read it for `(unmapped)` lines
**even when nothing has crashed**: a stubbed *async* method is an invisible hang, not a wrong
answer (§2 above).

## Adding a title

1. `strings -a <steam_api[64].dll> | grep -E '^(STEAM|Steam)[A-Za-z]*_?[A-Z_]*(INTERFACE_VERSION)?_?[0-9]{3}$' | sort -u`
   — mind the irregular suffixes (`..._VERSION_005`, `..._V003`), which a tighter pattern drops.
2. Add any new versions to `src/shim/interface-versions.txt`.
3. `./build.sh --regen-vtables` (needs network + `gh`), then `./build.sh`.
4. Run it with `SHIM_PE_LOG` set and grep the log for `(unmapped)`. Wire anything the title
   actually calls — and treat every unmapped method that returns a `SteamAPICall_t` as a
   blocker regardless of how harmless it looks, because the game will wait on its callback
   forever rather than fail. `RequestEncryptedAppTicket` is the worked example: one stubbed
   async slot, and a fully working game sits on a loading screen.

Titles do not necessarily use Steam for their *account*. Among Us authenticates to Epic Online
Services and only borrows Steam for identity, so its blocker was in `ISteamUser`, nowhere near
the achievement path this shim was built for.
