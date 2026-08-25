# Clean bottle & provenance: proving the shim is what answered

Findings for [#13](https://github.com/Superd22/macos-steam/issues/13). Everything below was
measured live on CrossOver 25.1.1, bottle `shim-clean`, against Mars'
`steam_api64.dll` (298 384 bytes, the exact DLL the destination title ships).

## The bottle

```sh
"$HOME/Applications/CrossOver.app/Contents/SharedSupport/CrossOver/bin/cxbottle" \
    --create --bottle shim-clean --template win10_64 \
    --description "Clean win64 bottle, no Windows Steam — shim provenance testing (#13)"
```

Same `win10_64` template as `Steam-2`, so the only variable between the two bottles is
the presence of Windows Steam. Baseline state, verified:

- **Zero Valve registry keys** in `user.reg` and `system.reg`. (The one grep hit for
  "steam" is `[Software\CrossOver\SuppressAltLoader] "steam"=dword:0` — CrossOver's own
  alt-loader suppression list, present in every new bottle, not Steam state.)
- **No Steam files** anywhere under `drive_c/`.

Keep it that way: the bottle's value *is* its emptiness. Any Steamworks state a test needs
gets written deliberately, per-run, and torn back down (see protocol below).

## How `SteamAPI_Init` actually resolves steamclient64.dll (measured, not read)

Instrument: `attic/shimprobe/` — a decoy `steamclient64.dll` that logs `DllMain` attach and
every `CreateInterface(name)` request to `shimprobe.log` beside itself, then returns NULL.
Two copies planted: one **beside the EXE**, one at `C:\FakeSteam\`. Registry state varied
between runs; S_API diagnostics captured via `WINEDEBUG=+debugstr`.

| Run | Registry state | Beside-EXE decoy | Registry-pointed decoy | Outcome |
|---|---|---|---|---|
| Negative control | none | not loaded | n/a | `Init()=0`; S_API: "did not locate a running instance" → "Could not determine Steam client install directory" |
| A | `ActiveProcess`: pid=4660 (dead), ActiveUser=1, Universe, SteamClientDll64→`C:\FakeSteam` | **not loaded** | **loaded**, `CreateInterface(SteamClient017)` then `(SteamClient020)` | init proceeds to interface factory |
| B | pid + SteamClientDll64 only | not loaded | loaded | same |
| C | pid=**0** + SteamClientDll64 | not loaded | loaded | same |
| D | **SteamClientDll64 only** | not loaded | **loaded** | same |
| E | empty `ActiveProcess` key | not loaded | (value absent) | back to negative-control failure |
| E2 | none, `SteamAppId=480`+`SteamGameId=480` in env | not loaded | n/a | negative-control failure |

Three conclusions, two of them corrections to what reading the binary suggested:

1. **The minimum load-bearing state is exactly one registry value:**
   `HKCU\Software\Valve\Steam\ActiveProcess\SteamClientDll64` (REG_SZ, full path).
   No `pid`, no `ActiveUser`, no `Universe`, no HKLM `InstallPath` — none of the keys the
   real client writes ([crossover-bridge-surface.md](crossover-bridge-surface.md) §"What
   this bottle currently has") are needed to get the DLL loaded and `CreateInterface`
   called. (They may matter *later* — connect-to-user, DRM — that's #12's territory.)
2. **⚠️ The "local steamclient64.dll next to the EXE" path never fires.** #4 read the
   string table ("Loaded local '%s' OK") and called the beside-the-EXE drop the cheapest
   hook. Empirically that branch is unreachable in this DLL: the beside-EXE decoy was
   ignored in *every* run — with no registry, with full registry, with a dead pid, and
   with `SteamAppId` in the environment. **Plant the shim via the registry value, not
   beside the game's EXE.** (Caveat: the harness loads `steam_api64.dll` explicitly via
   `LoadLibrary`, where a real game imports it; both from the same directory. If Mars
   itself ever behaves differently, re-test — but the registry hook works regardless.)
3. **`SteamAPI_IsSteamRunning` gates nothing we need and validates nothing.** A dead
   pid — or *no* pid — still gets the registry-pointed DLL loaded. The feared
   auto-launch of a real client is also a dead end in this bottle: with no `SteamExe` /
   `InstallPath` to launch, init just fails.

## Negative control (re-runnable)

With no shim and no registry state, the #7 harness in `shim-clean` fails **cleanly**:
every export resolves, then `SteamAPI_Init() = 0`, FATAL, no crash, nothing spawned,
nothing found outside the bottle. S_API's own diagnostics confirm where it gave up
(instance lookup, then install-directory lookup). This is the baseline every positive
result must be able to flip back to.

```sh
cd instruments/harness && make && cd build
"$HOME/Applications/CrossOver.app/Contents/SharedSupport/CrossOver/bin/wine" \
    --bottle shim-clean "$(pwd)/harness.exe" status   # expect: SteamAPI_Init() = 0, FATAL
```

## Provenance protocol — what to assert so a pass cannot be the real client

A test run in any bottle proves "the shim answered" by asserting all of:

1. **No client on disk**: `find "$BOTTLE/drive_c" -iname '*steam*'` returns only the
   shim artifacts the test planted itself (plus CrossOver's `SuppressAltLoader` noise in
   the registry). In `shim-clean` this holds by construction.
2. **Registry points only at the shim**: `SteamClientDll64` is the sole Valve value, and
   its path is the shim. Nothing else Steam-shaped exists for `steam_api64.dll` to fall
   back to — the negative control proved absence fails, so presence-of-shim is the only
   difference between FAIL and PASS.
3. **The shim self-identifies**: the shim logs its load and each `CreateInterface`
   request with its own PID (the shimprobe mechanism). A pass without the shim's log
   lines for *that run's* PID did not come from the shim.
4. **No Steam process anywhere**: during the run, `ps aux | grep -i steam.exe` on the
   host shows no Windows Steam under any wineserver — and the *native* macOS Steam.app
   is fine to have running (it's the destination's other half); what must be absent is
   any **Windows** Steam process.
5. **Removing the shim flips the run back to the negative control.** The cheapest,
   strongest check: delete/rename the shim DLL, re-run, expect `Init()=0` FATAL again.

## Bottle hygiene

- `Steam-2` (real Windows Steam installed) is for producing known-good reference traces
  only — never for shim testing; `SteamAPI_Init` there auto-launches the real client via
  its registry keys.
- After a test session in `shim-clean`, tear down what it planted:
  `wine --bottle shim-clean reg delete "HKCU\\Software\\Valve" /f` and remove planted
  files, returning to zero Valve keys. Verify: `grep -c Valve user.reg system.reg` → 0.
