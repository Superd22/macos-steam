# Native probe findings — issue #2 (go/no-go gate)

Machine: Apple Silicon (arm64), macOS 26.5.2. Steam.app running, logged in.
Steam client build dated 23 Jul 2026.

## Verdict

**Half the gate passes, half fails — and the failing half is the half that matters.**

Loading Valve's native `steamclient.dylib` from a process Steam did not launch works
completely and is not gated by code signing, entitlements, or the sandbox. But the
resulting client engine is **local and disconnected**: it never attaches to the
running Steam.app. Everything answerable from on-disk config succeeds; everything
requiring live IPC returns nothing.

This is a **false-positive trap**. A shallower probe would have declared success:
`CreateSteamPipe` returns a pipe, `ConnectToGlobalUser` returns a user, and
`ISteamUser::GetSteamID` returns a real, correct SteamID64. All three are lies.

## What works

| Step | Result |
| --- | --- |
| `dlopen` `steamclient.dylib` | OK — no library-validation flag (`codesign` reports `flags=0x0(none)`) |
| `dlsym CreateInterface` | OK |
| `CreateInterface("SteamClient017".."023")` | All seven served, distinct pointers 8 bytes apart |
| `CreateSteamPipe()` | returns `1` |
| `ConnectToGlobalUser(pipe)` | returns `1` |
| `GetISteamUser` / `GetISteamFriends` / `GetISteamUserStats` | all return non-null |
| **arm64 vs x86_64** | **identical behaviour on both**; the x86_64 build under Rosetta 2 matches arm64 step for step |

## What fails

| Signal | Value | Reading |
| --- | --- | --- |
| `Steam_BConnected(user, pipe)` | **false** | no live IPC |
| `Steam_BLoggedOn(user, pipe)` | false | ditto |
| `Steam_BGetCallback` pumped 2s | **0 callbacks** | nothing on the wire |
| `ISteamFriends::GetPersonaName()` | `²²²` (0xB2 filler) | IPC reply buffer never filled |
| `ISteamUserStats::RequestCurrentStats()` | **SIGSEGV** | disconnected engine, not a vtable error |

`ISteamUser::GetSteamID()` returns `76561198014230730` — which is exactly the first
entry in `~/Library/Application Support/Steam/config/loginusers.vdf`. It is read from
local config, not from the running client. That is why it looks convincing and means
nothing.

## The failing mechanism, precisely

`steamclient.dylib` imports `bootstrap_look_up` and carries three service-name literals:
`com.valvesoftware.steam`, `com.valvesoftware.steam.other`, `com.valvesoftware.steam.ipctool`.

A dyld interposer on `bootstrap_look_up` shows that `Steam_CreateSteamPipe` makes
**exactly one lookup**, and then silently falls back to a local engine:

```
bootstrap_look_up("com.valvesoftware.steam.other") -> kr=1102 (Unknown service name)
```

Nothing on this system ever checks that name in:

- `ipcserver`'s launchd plist (`~/Library/Application Support/Steam/com.valvesoftware.steam.ipctool.plist`)
  declares exactly one `MachServices` entry: `com.valvesoftware.steam.ipctool`.
- `nm -u` on `steam_osx`, `ipcserver` and `steamclient.dylib` shows **no**
  `bootstrap_register` and **no** `bootstrap_subset` anywhere. `ipcserver` is the only
  `bootstrap_check_in` caller. So no Valve component registers `.other` dynamically, and
  no bootstrap subset namespace is in play — this is not a "you must be a child of Steam
  to see the namespace" situation.
- `launchctl print gui/$UID` lists only `com.valvesoftware.steam.ipctool` among Valve
  endpoints.

From an unlaunched process, direct `bootstrap_look_up` results are:

```
com.valvesoftware.steam          kr=1102 Unknown service name
com.valvesoftware.steam.ipctool  kr=0    SUCCESS  port=0x1e03
com.valvesoftware.steam.other    kr=1102 Unknown service name
```

So the one endpoint that **is** reachable is exactly the one the dylib does not ask for.

## Things ruled out

- **Code signing / entitlements / sandbox.** Nothing rejects us. We get a port for
  `.ipctool` from an unsigned, unlaunched, Steam-unaware process.
- **Bootstrap namespace isolation.** No `bootstrap_subset` call exists in any Valve binary.
- **Environment bootstrap.** `SteamAppId`, `SteamGameId`, `SteamClientLaunch`, `SteamPath`
  and `SteamRealm` (tried `Global`/`global`/`1`/`GLOBAL`) change nothing — the same single
  `.other` lookup happens in every case. Steam's own child processes carry almost no Steam
  env either (`STEAM_CLIENT_CONFIG_FILE`, `STEAM_APP_BUNDLE_PATH`), so env is not the key.
- **Redirecting to the registered port.** Forcing the failed lookup to return the live
  `.ipctool` port yields `kr=0` and a valid port, but `BConnected` stays false — different
  protocol on the other end, or a further handshake is required.
- **Vtable transcription error.** The crash is in the stats path only, and `GetSteamID`
  (slot 2) returns a correct value, so the layouts are right.

## What this does not yet answer

**What condition makes `com.valvesoftware.steam.other` exist.** Every Steam-launched
native macOS game must clear this somehow. Untested and the obvious next experiment:
re-run `machprobe` **while a native macOS Steam game is actually running**, to see whether
Steam registers the endpoint lazily, only while serving a game.

## Reproducing

```sh
clang++ -std=c++17 -g -O0 -arch arm64 -o probe-arm64 probe.cpp
clang++ -std=c++17 -g -O0 -arch arm64 -o connprobe-arm64 connprobe.cpp
clang++ -std=c++17 -g -O0 -arch arm64 -o machprobe-arm64 machprobe.cpp
clang++ -std=c++17 -dynamiclib -arch arm64 -o interpose.dylib interpose.cpp

./machprobe-arm64                                              # which names resolve
DYLD_INSERT_LIBRARIES=$PWD/interpose.dylib ./connprobe-arm64    # which name is asked for
```

Read-only with respect to Steam's data directories throughout.
