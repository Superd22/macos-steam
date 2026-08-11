# Native probe findings — issue #2 (go/no-go gate)

Machine: Apple Silicon (arm64), macOS 26.5.2. Steam.app running, **logged in and
online**. Steam client build dated 23 Jul 2026.

## Verdict: **GO**

A plain macOS process that Steam did **not** launch can `dlopen` Valve's native
`steamclient.dylib`, connect to the already-running Steam.app over live IPC, and read
back real account state — including a real achievement with its real unlock time.

```
Steam_BConnected                                  TRUE
BLoggedOn                                         true
SteamID                                           76561198014230730
PersonaName                                       ²²²
GetAchievement("PROBE_SLOT_CHECK_NOT_REAL")       ok=0            <- bogus name rejected
GetAchievementAndUnlockTime("BG3_Quest01")        ok=1 achieved=1
                                                  unlockTime=1692489287
                                                  (2023-08-19 23:54 UTC)
```

The bogus-name control matters: a binding that "succeeds" at everything proves nothing.
This one **discriminates** — it rejects the achievement that doesn't exist and returns a
correct unlock timestamp for the one that does, for an app (Baldur's Gate 3, `1086940`)
the account genuinely owns.

**arm64 and x86_64-under-Rosetta-2 behave identically, step for step**, including the
achievement read. No architecture is privileged. This extends #4's finding from *loading*
to the *full* read path.

No code-signing, entitlement, or sandbox check gates any of it. The dylib carries
`flags=0x0(none)`, and the probe is unsigned, unlaunched and Steam-unaware.

## Bootstrap required: none

Nothing had to be set up. No `SteamAppId`, no `steam_appid.txt`, no cwd, no
`SteamClientLaunch`, no `SteamPath`. The only requirements are:

- **Steam.app must be running.**
- **Steam must be online.** (See the confound below — this one cost a full misdiagnosis.)
- `SteamAppId` is needed *only* to give `ISteamUserStats` an app context for a specific
  title. Account-level state (`ISteamUser`, `ISteamFriends`) needs nothing at all.

## The offline-mode confound — read this before trusting any probe

**Steam in offline mode produces a convincing false NEGATIVE.** With the client offline,
the exact same code returns:

| Signal | Offline | Online |
| --- | --- | --- |
| `CreateSteamPipe` / `ConnectToGlobalUser` | `1` / `1` | `1` / `1` |
| `Steam_BConnected` | **false** | **true** |
| `BLoggedOn` | false | true |
| `Steam_BGetCallback` pumped 2s | 0 callbacks | callback delivered |
| `GetSteamID` | correct | correct |
| `GetPersonaName` | correct | correct |

Offline mode is indistinguishable from "the bridge is broken" unless you already know to
suspect it. Every future ticket that tests the bridge should **assert Steam is online
first**, or it will chase a phantom.

## A red herring, documented so nobody re-investigates it

A dyld interposer on `bootstrap_look_up` shows `Steam_CreateSteamPipe` performing exactly
one lookup, which **always fails**, online and offline alike:

```
bootstrap_look_up("com.valvesoftware.steam.other") -> kr=1102 (Unknown service name)
```

This is **not** the connection path. It fails identically in the fully working online case.
`com.valvesoftware.steam.other` is never registered by anything: `ipcserver`'s launchd
plist declares only `com.valvesoftware.steam.ipctool`, and `nm -u` finds no
`bootstrap_register` and no `bootstrap_subset` in `steam_osx`, `ipcserver`, or
`steamclient.dylib`. It is a legacy or optional probe whose failure is routine. The real
IPC goes elsewhere — most likely the `.ipctool` endpoint, which does resolve from an
unlaunched process.

**Do not treat a failed `.other` lookup as a symptom.** It is background noise.

## Vtable trap, hit for real

`STEAMUSERSTATS_INTERFACE_VERSION013` **drops `RequestCurrentStats`** — the entire vtable
shifts up by one relative to `VERSION012`. Calling slot 0 as `RequestCurrentStats` on a
v013 pointer actually lands in `GetStat`, which reads a garbage `pchName` and segfaults
inside `memmove`. Steam itself names the culprit if you look:

```
[S_API WARN] GetStat() failed, stat (null) does not exist
```

This is exactly the failure class #3's research warned about: a wrong vtable index does
not produce a clean error, it produces a corrupted call. The v013 layout is:

```
0 GetStat(int32*)   1 GetStat(float*)   2 SetStat(int32)   3 SetStat(float)
4 UpdateAvgRateStat 5 GetAchievement    6 SetAchievement   7 ClearAchievement
8 GetAchievementAndUnlockTime           9 StoreStats
```

Per-version vtable layouts must be transcribed, never assumed to carry over.

## Also confirmed

The full minimum-viable flat export set from #3 is present in the macOS dylib (38
exported symbols total): `CreateInterface`, `Steam_BGetCallback`,
`Steam_FreeLastCallback`, `Steam_GetAPICallResult`, `Steam_ReleaseThreadLocalMemory` —
plus `Steam_BConnected` / `Steam_BLoggedOn`, which are the honest connection oracle and
worth using in every later test.

`CreateInterface` serves all of `SteamClient017`–`023` as distinct pointers 8 bytes apart.

## Not covered here

Achievement **writing** (`SetAchievement` + `StoreStats`) was deliberately not attempted —
it would mutate the real account. That belongs to #11, against a throwaway title.

The callback pump was only observed delivering a callback, not decoded. Threading model
remains open.

## Reproducing

```sh
clang++ -std=c++17 -g -O0 -arch arm64  -o probe-arm64  probe.cpp
clang++ -std=c++17 -g -O0 -arch x86_64 -o probe-x86_64 probe.cpp
clang++ -std=c++17 -g -O0 -arch arm64  -o connprobe-arm64 connprobe.cpp
clang++ -std=c++17 -g -O0 -arch arm64  -o machprobe-arm64  machprobe.cpp
clang++ -std=c++17 -dynamiclib   -arch arm64 -o interpose.dylib interpose.cpp

# Steam must be running AND online.
SteamAppId=1086940 PROBE_ACHIEVEMENT=BG3_Quest01 ./probe-arm64 1086940
./connprobe-arm64      # Steam_BConnected — the honest check
```

Achievement API names can be recovered from
`~/Library/Application Support/Steam/appcache/stats/UserGameStatsSchema_<appid>.bin`.

Read-only with respect to Steam's data directories throughout.
