# The Half A ↔ Half B seam: a compat-tool launch script, proven end-to-end on 480

Findings for [#12](https://github.com/Superd22/macos-steam/issues/12). Measured live on
2026-08-14, CrossOver 25.1.1 (`wine-10.0-8474`), Apple Silicon (M3 Pro), macOS 26.5.2,
bottle `shim-clean`, native macOS Steam.app running and **online**
(`BLoggedOn=1`, real server unlock time). **Zero Windows Steam processes** at any point
(the leftover `steam.exe -silent` in bottle `Steam-2` was quit first — see Provenance).

## What #12 is, and what this session did / did not resolve

#12 is the destination: **real Surviving Mars, installed through the native macOS client,
launched through CrossOver, an achievement firing, no Windows Steam.** It is a **HITL**
ticket — the final acceptance test requires a human to click Install/Play in the Steam UI,
download a ~10 GB Windows depot, and *play the game to an achievement*. An autonomous agent
cannot press Play and play Surviving Mars. So this session did the part that **is** an
engineering artifact, and hands the rest to a human runbook (below).

**Proven this session (autonomous):**

- The one **unbuilt artifact** the whole map converged on — the compat-tool **launch script**
  that ADR 0002 named as "the boundary with Half B (#12)" — is built: `tools/compat-tool/`.
- The **Half A ↔ Half B seam holds through the production launch vehicle.** Driving the launch
  script exactly as Steam's compat-tool contract invokes it —
  `steamclient-shim-launch.sh waitforexitandrun C:\harness.exe loop`, with **only Valve's
  `STEAM_COMPAT_APP_ID=480`** exported (never `SteamAppId` directly) — reproduces #11's
  `=== LOOP PASS ===` callback-for-callback: a real achievement on 480 unlocks
  (`unlock=1786659845`) and resets, byte-identical `1101/1102/1103` payloads.
- **Provenance (the #13 protocol):** remove the shim + delete the `SteamClientDll64` value →
  the same harness flips to `SteamAPI_Init() = 0`. The shim is what answered; nothing fell
  back to a Windows client (there was none).

**NOT resolved this session (needs the HITL Mars run):** the real depot Install via the UI,
the real Play button, **Mars' DRM/CEG** (480 has none; Mars is a retail DRM'd title — the
open item in the map's *Not yet specified*), the game-dir-outside-drive_c launch path (see
Risks), and web-profile / native-UI achievement visibility. #12 stays **open** until a human
completes the runbook.

## The launch script — what it wires, and why the wiring is exactly #11's

Steam (patched, `m_bCompatEnabled` on) invokes the tool as
`<commandline> <verb> <path-to-exe> [args...]` with the `STEAM_COMPAT_*` environment. The
script reconstructs the **proven** Half B shim environment (#11/#13) around that `.exe`:

| Wiring | Source | Why |
|---|---|---|
| `SteamClientDll64` registry value → our PE | #13 | the ONLY hook that makes `steam_api64.dll` load our `steamclient64.dll` (the beside-the-EXE path never fires) |
| `WINEDLLPATH` includes the shim dir | #8/#10 | so ntdll finds `steamclient64.so` beside the builtin-marked PE |
| `SteamAppId`/`SteamGameId` ← `STEAM_COMPAT_APP_ID` | #11 | gives the dylib's `ISteamUserStats` its app context; derived from Valve's contract var |
| exec `wineloader <exe>` | — | runs the title in the bottle; the game's `steam_api64.dll` does the rest |

`tools/compat-tool/` layout (mirrors ADR 0002's external-payload spec):

- `steamclient-shim-launch.sh` — the `commandline` target (this seam).
- `toolmanifest.vdf` — `version 2`, `commandline`, `use_sessions 1`.
- `compatibilitytool.vdf` — `to_oslist "macos"` + the static `app_mappings { "0" }`
  priority-100 catch-all (#17). Ships pointed at by `STEAM_EXTRA_COMPAT_TOOLS_PATHS`.

## Reproduce (the 480 seam proof)

```sh
cd tools/harness && make            # builds harness.exe + steam_api64.dll (+ steam_appid.txt)
cd ../shim && ./build.sh            # builds dist/steamclient64.{dll,so}
# native macOS Steam must be running AND online; NO Windows steam.exe anywhere:
ps aux | grep -i steam.exe | grep -v grep   # must be empty

BOTTLE="$HOME/Library/Application Support/CrossOver/Bottles/shim-clean"
cp tools/harness/build/harness.exe     "$BOTTLE/drive_c/"
cp tools/harness/build/steam_api64.dll "$BOTTLE/drive_c/"
cp tools/harness/build/steam_appid.txt "$BOTTLE/drive_c/"

STEAM_COMPAT_APP_ID=480 SHIM_BOTTLE=shim-clean SHIM_UNIX_LOG=/tmp/shim_unix.log \
  tools/compat-tool/steamclient-shim-launch.sh waitforexitandrun 'C:\harness.exe' loop
# expect: === LOOP PASS ===

# negative control:
rm "$BOTTLE"/drive_c/shim/steamclient64.*   # + delete HKCU\...\SteamClientDll64
# re-run -> SteamAPI_Init() = 0
```

## Risks the HITL Mars run must retire (carried into the runbook)

1. **DRM / CEG.** 480 is DRM-free. Surviving Mars (`3215050`) is a retail Steam-DRM title;
   once `Mars.exe` actually runs it may perform an ownership check / CEG decrypt the shim
   has not been asked for. This is the map's open *Ownership/DRM* item and the single biggest
   unknown left. Only a real Mars boot answers it.
2. **Game dir outside `drive_c`.** The native client installs the Windows depot into its
   **macOS** library (`~/Library/Application Support/Steam/steamapps/common/…`), not into a
   bottle. The launch script hands wineloader that unix path; `steam_api64.dll` sits beside
   the `.exe` there while our shim is registry-wired and `WINEDLLPATH`-found. Expected to
   work (wine runs unix-path exes), but untested for a Steam-installed title — verify.
3. **Bottle selection / provisioning.** `SHIM_BOTTLE` defaults to `steam-shim`; the runbook
   must create/choose the bottle the title launches in. `shim-clean` was this session's proof
   bottle.
4. **The `appid 0` catch-all must not route native/dual-platform titles** (map guard, ADR
   0002 open verification) — confirm at Mars-install time that only `display_status 14` titles
   route through us.

## HITL runbook — the real Surviving Mars end-to-end (a human drives this)

Preconditions: `tools/shim/build.sh` and `tools/compat-tool/` present; native macOS Steam
installed; **no Windows Steam** (`ps aux | grep -i steam.exe` empty — quit bottle `Steam-2`
if needed, as this session did).

1. **Flip the gate (resident):** launch `steam_osx` with
   `DYLD_INSERT_LIBRARIES=…/libcompat-enabler.dylib` (`tools/compat-enabler/`). Confirm
   `/tmp/compat-enabler.log` shows `patched 1 site(s)`. Keep Steam online.
2. **Register the tool:** launch with
   `STEAM_EXTRA_COMPAT_TOOLS_PATHS=<abs path to tools/compat-tool>`. Confirm in `compat_log`:
   `Processing local tool list … crossover-steam-shim` and
   `Mapping AppID 0 to tool "crossover-steam-shim" with priority 100`.
3. **Install:** find Surviving Mars (`3215050`) in the library; its `display_status` should
   read `14 → 9` (ready to install). Click **Install**; confirm the Windows depot downloads
   (byte size should match the depot oracle) into the macOS `steamapps` dir. Confirm a
   native/dual-platform control title (e.g. an owned macOS game) is **untouched** (Risk 4).
4. **Point `SHIM_BOTTLE`** at the CrossOver bottle you want Mars to run in (create one from
   `win10_64` if needed; it needs no Windows Steam — the shim is the whole client).
5. **Play:** click **Play**. Steam invokes `steamclient-shim-launch.sh waitforexitandrun
   <…>/Mars.exe`. Watch `/tmp/shim_unix.log` for `CreateInterface(...)` and `GetAppID() ->
   3215050`. If it faults instead — treat any "parked with a debugger thread" hang as a crash
   and disassemble the fault site (#11's sret lesson); a DRM/CEG demand will surface here.
6. **Fire an achievement:** play to a confirmed early Surviving Mars achievement. It should
   unlock (watch for `UserAchievementStored_t`).
7. **Verify visibility:** the achievement shows in the native macOS Steam client UI **and**
   on the web profile (`steamcommunity.com/profiles/…/stats/3215050`).
8. **Assert throughout:** `ps aux | grep -i steam.exe` stays empty (zero Windows Steam);
   `BLoggedOn=1` (online, not offline false-negative).

Done when steps 5–7 pass and the DRM question (Risk 1) is answered in writing.

---

# 2026-08-23 — the Play button works: Steam launches Mars through the tool

Second measurement pass for [#12](https://github.com/Superd22/macos-steam/issues/12), on
**CrossOver 26.2**, client build `1785799196`, bottle `steam-shim`, native macOS Steam
running and online. **Zero Windows Steam processes** before, during and after
(`ps aux | grep -i steam.exe` empty at every checkpoint).

Everything above this line was measured by invoking the launch script **by hand**. This pass
closes that gap: the launch is driven by **Steam's own `LaunchApp` action**, and the
difference immediately exposed two bugs the hand-driven proofs could never have found.

## Result

`steam://rungameid/3215050` (what the Play button issues) drives the full chain:

```
GameAction [AppID 3215050, ActionID 3] : LaunchApp changed task to CreatingProcess
  .../crossover-steam-shim/./steamclient-shim-launch.sh
  .../steamapps/common/Project Spark/Mars.exe
Game process added : AppID 3215050 "...steamclient-shim-launch.sh '...Mars.exe'", ProcID 39396
GameAction [AppID 3215050, ActionID 3] : LaunchApp changed task to Completed
```

and the shim answers for the real title:

```
CreateInterface("SteamClient020") -> ...      GetAppID() -> 3215050
SteamUser021  SteamUtils010  SteamInput006  SteamFriends017
STEAMAPPS_INTERFACE_VERSION008   STEAMUGC_INTERFACE_VERSION016
STEAMUSERSTATS_INTERFACE_VERSION012
BIsSubscribedApp(3889420) -> 1
```

Mars reaches gameplay. **Half A and Half B are joined through the product vehicle, driven by
Steam, with no hand-run command anywhere in the path.**

## Bug 1 — the manifest dropped `%verb%`, and the launch died in two seconds

Steam invoked the tool as `steamclient-shim-launch.sh <exe>` — **no verb**:

```
.../steamclient-shim-launch.sh\0.../Mars.exe\0
```

The verb is not something Steam always supplies; it is substituted into the manifest's
`commandline` where `%verb%` appears. Our `toolmanifest.vdf` read
`"commandline" "/steamclient-shim-launch.sh"`, so nothing was substituted. The script
unconditionally treated `$1` as the verb, shifted it away, found `$2` empty, and exited 2.

**What this looked like:** `LaunchApp … Completed`, `Game process added`, then
`Game process removed` two seconds later. Steam reported a *successful* launch. Nothing
appeared in any log, because the script's only diagnostics went to a stderr Steam detaches.
A silent, near-instant, success-shaped failure.

Both halves are fixed, deliberately redundantly:

- `toolmanifest.vdf` now reads `"commandline" "/steamclient-shim-launch.sh %verb%"`.
- The script no longer *assumes* `$1` is a verb — it **sniffs** it, consuming `$1` only when
  it is literally `run` or `waitforexitandrun`, and otherwise defaults to
  `waitforexitandrun` and logs that it did so.

The redundancy earned its keep within the same session: **Steam caches the manifest from
registration time**, so the corrected `commandline` did *not* take effect on the next launch
(the log still says `argv[1] is not a verb`). The launch succeeded anyway, because the script
tolerates it. A manifest edit needs a Steam restart to be picked up; a tool that only works
after a restart is a tool that fails on first install.

## Bug 2 — the launch script logged only to a stderr Steam throws away

Diagnosing bug 1 required reconstructing the argv from Steam's own console log, because the
script's `log()` wrote to stderr only. It now tees every line to `/tmp/shim-launch.log`
(`SHIM_LAUNCH_LOG` to override). This is the file to look at first when a Play click does
nothing — it is the only place the tool's own view of a launch is recorded.

Also fixed: the "native macOS Steam does not appear to be running" warning fired on **every**
launch, including this successful one. Steam hands the tool a scrubbed `PATH`, so the bare
`pgrep` was simply not resolvable. Now called as `/usr/bin/pgrep`. A check that cries wolf on
every run is worse than no check — it trains you to read past the one time it is right.

## Bug 3 — the deployed payload had silently drifted three commits behind

The live payload under `~/Library/Application Support/macos-steam-shim/` still held the
**2026-08-14** build: the pre-#19 launch script (raw `wineloader`, no CWD fix, no front door)
and pre-#18 shim binaries (before the full vtable transcription). It had been hand-assembled
once and never re-synced, so the registered production tool was one that #18/#19 had already
proven *would crash*, while the repo looked correct.

Deployment is now one reproducible command, `tools/installer/install.sh`, which builds what is
missing and copies the built artifacts into both shipped surfaces — the external payload dir
and the `Steam (macOS Play).app` launcher (previously hand-made and not in the repo at all).
`--uninstall` removes both; Valve's files are never touched, so there is nothing else to undo.

## The install-script evaluator fails, and it does not matter (yet)

Every launch routes Valve's `iscriptevaluator.exe` through our tool first and ends in:

```
src/clientdll/installscript_posix.cpp (525) : Assertion Failed:
  Standalone evaluator returned error code for app 3215050
```

`LaunchApp` proceeds to `Completed` regardless and the game runs, so this is **non-fatal** for
Mars. It is not understood, and a title whose install script actually matters may not be so
forgiving. Recorded, not resolved.

## What is still NOT proven — the honest gap

**No fresh Surviving Mars achievement has unlocked through this stack.**

On both of today's runs the shim logged `SetAchievement(...) -> 1` for
`FirstAnalyzedAnomaly`, `FirstRefueledRocket` and `FirstHarvest`. That is **not** evidence:
the web profile shows all three were unlocked on **2026-08-02**, before any of this existed.
The game re-asserts already-earned achievements on save load, and `SetAchievement` returns 1
for an achievement that was already set. It is a textbook false pass of exactly the kind this
map has been bitten by twice before — and the same trap the `atime` oracle and Steam's
offline mode each sprang earlier.

The write path *is* proven end-to-end on Spacewar 480 (#11), with a real unlock and a real
reset. What is unproven is that a **retail, DRM'd** title unlocks a **new** achievement
through the shim. Only a human playing Mars to one of its 61 still-locked achievements
answers that, and the oracle must be the **public web profile** (server-side record), not a
local read — a local cache cannot distinguish "stored on Valve's servers" from "written to
disk here".

**Deliberately not done:** setting an unearned Mars achievement to manufacture the proof. It
would write a permanent, publicly-visible falsehood to a real account's profile, and the only
way to undo it is `ResetAllStats`, which would destroy the four genuine achievements. The
proof is worth less than the account.
