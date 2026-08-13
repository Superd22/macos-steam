# Achievement test harness

Fast, idempotent achievement loop against **Spacewar (appid 480)** — the
reference instrument for the bridge ([#7](https://github.com/Superd22/macos-steam/issues/7)).
A Windows x86_64 console exe that drives Valve's `steam_api64.dll` through the
destination's exact call path and hex-dumps every callback; run against real
Windows Steam it produces the known-good trace the shim (#11) must reproduce.

## Build & run

```sh
make          # mingw cross-build + copies steam_api64.dll from the Mars install + writes steam_appid.txt(480)
cd build
"$HOME/Applications/CrossOver.app/Contents/SharedSupport/CrossOver/bin/wine" \
    --bottle Steam-2 "$(pwd)/harness.exe" loop
```

Windows Steam must be running **and online** in the bottle first (the harness
aborts on `BLoggedOn=false` — offline Steam is a convincing false negative, map
trap #1). No Spacewar install is needed: `steam_appid.txt` beside the exe makes
it init as 480 with nothing installed.

Modes: `loop` (default; set → verify → reset → verify — never burns the
achievement), `status` (read-only), `set [ACH]`, `reset`. `--md` switches to the
manual-dispatch pump (see findings — broken in this environment).

## Reference trace

`traces/2026-08-13-windows-steam-480-loop.txt` — full `loop` PASS against real
Windows Steam in bottle Steam-2, CrossOver 25.1.1. Set→`UserAchievementStored_t`
+`UserStatsStored_t(eresult=1)`→`achieved=1` with real unlock time→
`ResetAllStats(true)`→cleared. ~2 s end-to-end, repeatable (two consecutive
PASS runs).

## Findings (verified live, this environment)

- **Appid 480 is NOT restricted**: achievements set, store, callback, and reset
  fine. Schema has **5** achievements — the four documented ones plus
  `NEW_ACHIEVEMENT_0_4`. `ResetAllStats(true)` works.
- **Manual dispatch is broken here**: with `SteamAPI_ManualDispatch_Init`
  armed before `Init`, `GetNextCallback` never returns a single callback (Mars'
  SDK-era dll under CrossOver 25.1.1; same for appid 480 and 3215050). The
  classic `SteamAPI_RegisterCallback` + `RunCallbacks` pump works instantly.
  The shim must therefore serve the classic path; don't assume manual dispatch.
- **MSVC overload reversal confirmed live** (map trap #2): the plain
  `Run(void*)` virtual is at vtable **slot 1**; slot 0 is
  `Run(void*, bool, SteamAPICall_t)`.
- **`CSteamID` is pack(1)** — in `UserStatsReceived_t` it sits at offset **12**
  (immediately after the `EResult`, no padding), total payload 20 bytes.
- **`GetAchievement`'s out-param is a 1-byte bool** — it writes only one byte;
  read it through a byte, not an int.
- Mars' `steam_api64.dll` (298 384 bytes) requests
  `STEAMUSERSTATS_INTERFACE_VERSION012` / `SteamClient017|020` / `SteamUser021`
  and exports the full flat + manual-dispatch API. v012 = the vtable
  transcribed in #2 — `RequestCurrentStats` still exists.

`steam_api64.dll` is Valve's redistributable and is **not** committed; `make`
copies it from the Surviving Mars install in bottle Steam-2.
