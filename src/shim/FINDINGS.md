# The achievement shim — the full path crosses, a real achievement unlocks, no Windows Steam

Findings for [#11](https://github.com/Superd22/macos-steam/issues/11). Measured live on
2026-08-13, CrossOver 25.1.1 (`wine-10.0-8474`), Apple Silicon (M3 Pro), macOS 26.5.2,
bottle `shim-clean`, native Steam.app running and **online** (`Steam_BConnected = TRUE`,
`BLoggedOn = 1`). **Zero Windows Steam processes** at any point.

## Verdict: PASS

The #7 harness, driven through the shim under the production `SteamClientDll64` hook with no
Windows Steam, unlocks a **real achievement on the real account** and reproduces the
known-good Windows-Steam trace callback-for-callback:

```
SteamAPI_Init() = 1
BLoggedOn=1 SteamID=76561198014230730
CALLBACK UserStatsReceived_t   gameid=480 eresult=1 steamid=76561198014230730
SetAchievement("ACH_WIN_ONE_GAME") = 1
StoreStats() = 1
CALLBACK UserAchievementStored_t gameid=480 group=0 name="ACH_WIN_ONE_GAME" cur=0 max=0
CALLBACK UserStatsStored_t     gameid=480 eresult=1
VERIFY set:   achieved=1 unlock=1786655876   PASS      <- real unlock time, on the server
ResetAllStats(true) = 1
CALLBACK UserStatsReceived_t   gameid=480 eresult=1 steamid=76561198014230730
VERIFY reset: achieved=0                      PASS
=== LOOP PASS ===
```

Every achievement-critical line is **byte-identical** to
`instruments/harness/traces/2026-08-13-windows-steam-480-loop.txt` (produced against real Windows
Steam). Only wall-clock timestamps differ — init is ~4× slower through the seam (5.1 s vs
1.1 s), the callback intervals are comparable. The achievement genuinely unlocked (the
`UserAchievementStored_t` with a real unlock time is the server's acknowledgement) and was
reset in the same run, so the test is idempotent and burns nothing.

(Target is Spacewar 480, per [#7](https://github.com/Superd22/macos-steam/issues/7)'s
rescope: achievement e2e on Spacewar, Mars = boot+init proof.)

## What was built

`src/shim/` — a real `steamclient64.dll` (the file the game's `steam_api64.dll` loads),
in two halves crossing the #8/#10 unixlib seam:

- **`steamclient64.dll`** (PE, mingw, marker-stamped builtin): presents Valve's flat C
  exports (`CreateInterface`, `Steam_BGetCallback`, `Steam_FreeLastCallback`,
  `Steam_GetAPICallResult`, `Steam_ReleaseThreadLocalMemory`) and MSVC-ABI vtables for
  `ISteamClient`/`ISteamUser`/`ISteamUtils`/`ISteamUserStats`. Each method packs a
  fixed-layout params struct and forwards across `__wine_unix_call`. Zero Steam logic.
- **`steamclient64.so`** (x86_64 Mach-O, clang): hosts the real `steamclient.dylib`
  in-process and answers each seam call by making the actual Steamworks call in the native
  SysV ABI, casting the opaque handle back to a C++ class of pure virtuals (the #3 / Proton
  `u_iface` model).

One address space, one arch, so pointers and callback payloads pass verbatim; the seam only
bridges the **calling convention** (MSVC `this` in RCX → SysV virtual dispatch).

## The one bug that mattered: `GetSteamID`'s by-value `CSteamID` return (sret ABI)

Everything up to `GetSteamID` crossed on the first build — `CreateInterface`, pipe,
`ConnectToGlobalUser`, `GetAppID()=480`, all interface acquisition. Then `SteamAPI_Init`
appeared to "hang." It was not a hang: `steam_api64.dll` **page-faulted** dereferencing
`0x01100001033770CA` — *exactly our SteamID* (`0x01100001` universe/type + accountid
`0x033770CA` = 53965002) — and wine's crash handler attached a debugger that parked every
thread, which `sample` reads as an idle deadlock. **A wine unhandled fault masquerades as a
hang; treat any "parked with a debugger thread" state as a crash and disassemble.**

The faulting site in `steam_api64.dll` disassembles to the MSVC **sret** call sequence:

```
mov  0x10(%rcx),%r8    ; r8 = vtable slot 2 = GetSteamID
lea  0x478(%rsp),%rdx  ; rdx = hidden result buffer   <- sret pointer
mov  %rax,%rcx         ; rcx = this
call *%r8              ; GetSteamID(this=rcx, sret=rdx)
mov  (%rax),%rcx       ; expects rax == &buffer, derefs it   <- FAULT when rax = the value
```

`ISteamUser::GetSteamID()` returns `CSteamID` **by value**. Because `CSteamID` has
user-defined constructors, MSVC returns it via a **hidden sret pointer** (passed in RDX
after `this` in RCX) and expects the callee to fill it and **return that pointer in RAX**.
The dylib (SysV) returns the 8-byte value in RAX — which is why the seam value was always
correct (#10 read it fine) — but the PE thunk must not pass that value straight back. The
fix is one thunk:

```c
static uint64_t *iu_GetSteamID(struct w_iface *s, uint64_t *sret)
{ /* seam -> p.ret */ *sret = p.ret; return sret; }   /* write buffer, return it in RAX */
```

With that, `SteamAPI_Init` returns 1 and the whole path runs. **Any Steamworks method
returning `CSteamID`/`CGameID` by value needs this treatment on the PE side.** Proton hits
the same wall and marshals these specially.

## The Metal/MoltenVK stack is a red herring

Establishing the app context maps the whole Metal / MetalPerformanceShaders / MoltenVK stack
into the process, but no thread ever runs in it — the frameworks are loaded, not active, and
`SteamNoOverlayUIDrawing=1` changes nothing. It is a symptom of the app context activating,
not a problem. Ignore it.

## The callback pump — the predicted "hardest part" — needed no special machinery

#11 flagged the callback return path as the hardest part. It was not: because macOS is on
Linux's `pack(4)` side (#3 §5) and callbacks 1101/1102/1103 are conversion-free, and because
`CallbackMsg_t` is byte-identical macOS↔Windows x64, `Steam_BGetCallback` is a **direct
passthrough** — forward `(pipe, CallbackMsg_t*)` to the native flat function; the filled
message and its `m_pubParam` payload are consumed as-is by `steam_api64.dll`'s classic
`RegisterCallback`/`RunCallbacks` pump. No length translation, no struct converters, no
deferred-upcall queue. The `m_OutstandingCallbackThreadId` warning the connprobe emits does
**not** block delivery here. (This is only true for the achievement callbacks; interfaces
that hand Steam a PE function pointer would still need the deferred-upcall queue — none are
on this path.)

## MSVC-ABI facts learned (transcribe, don't assume — map trap #2)

- `steam_api64.dll` (Mars, 298 384 bytes) requests, in order: `SteamClient017` then
  `SteamClient020`; `SteamUtils010`; `SteamUser021`; `STEAMUSERSTATS_INTERFACE_VERSION012`;
  and later `SteamController008` / `SteamInput006` (both stubbed to NULL, harmless).
- **`ISteamUtils` VERSION010: `GetAppID` is vtable slot 9.** Init calls it; returning 480
  (from `SteamAppId=480` in the env, read by the dylib) clears the app-context check. A
  stubbed 0 makes init reject the context.
- `SteamInternal_FindOrCreateUserInterface` routes **through
  `ISteamClient::GetISteamGenericInterface` (slot 12)** — every user interface can be served
  from that one method; the typed getters are not required.
- `ISteamUserStats` VERSION012 achievement slots used, all confirmed live:
  `RequestCurrentStats`@0, `GetAchievement`@6, `SetAchievement`@7, `ClearAchievement`@8,
  `GetAchievementAndUnlockTime`@9, `StoreStats`@10, `GetAchievementDisplayAttribute`@12,
  `GetNumAchievements`@14, `GetAchievementName`@15, `ResetAllStats`@21.
- `ISteamUser` leading slots: `GetHSteamUser`@0, `BLoggedOn`@1, `GetSteamID`@2 (sret).

## Provenance (the #13 protocol, satisfied)

- **No client on disk**: `shim-clean` holds only the shim files this run planted.
- **Registry points only at the shim**: `SteamClientDll64 = C:\shim\steamclient64.dll`.
- **The shim self-identifies**: PE-side `OutputDebugStringA("shim: …")` + unix-side
  `/tmp/shim_unix.log`, both PID-stamped for the run.
- **No Windows Steam anywhere** during the run; native macOS Steam.app is the intended other
  half.
- **Removing the hook flips to the negative control**: with `SteamClientDll64` deleted and
  nothing else changed, `SteamAPI_Init() = 0` in **537 ms** — no connect, no achievement.
  The difference between fast-FATAL and a real unlock is the shim.
- **Online asserted**: `BLoggedOn=1` in the pass, so the read is the live server, not
  offline cache (map trap #1).

## Reproduce

```sh
cd instruments/harness && make && cd ../../src/shim
../native-probe/connprobe-x86_64          # assert Steam_BConnected = TRUE first
./build.sh
./run.sh loop                             # expect: === LOOP PASS ===
# negative control: delete HKCU\Software\Valve\Steam\ActiveProcess\SteamClientDll64 -> Init=0
```

## Scope notes

- The shim wires exactly the achievement path (5 flat exports, ~4 interfaces, ~20 real
  method bodies). All other vtable slots are numbered logging stubs that return 0 — safe on
  MS-x64 (caller-cleanup) and useful for discovering what `steam_api64.dll` actually calls.
- Not attempted here: Mars (`3215050`) boot+init in the clean bottle (a separate proof,
  needs the 9.5 GB install), and any interface that hands Steam a PE callback pointer.
