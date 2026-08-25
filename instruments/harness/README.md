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
achievement), `status` (read-only), `set [ACH]`, `reset`, and `overlay [WHICH]`
(below). `--md` switches to the manual-dispatch pump (see findings — broken in
this environment).

## Overlay mode (#23)

```sh
                ../shim/run.sh overlay all     # expect IsOverlayEnabled() = 0
SHIM_OVERLAY=1  ../shim/run.sh overlay all     # expect 1 iff injection armed
SHIM_OVERLAY=1  ../shim/run.sh overlay store   # just one slot
```

`WHICH` is `all` (default) or one of `store web friends user invite remoteplay
connect protocol`.

**Why a fake title tests this better than a real one.** The overlay slots are
calls the *game* makes, and a game only makes them when a player clicks
something. Four of the twelve — `RemotePlayTogetherInviteDialog`, the
connect-string invite, `RegisterProtocolInOverlayBrowser` and
`SetOverlayNotificationInset` — are not called by any title we own, so a
real-title test cannot reach them at all. The harness calls every one on demand,
through Valve's own `SteamAPI_ISteamFriends_*` flat functions, which do the MSVC
thiscall dispatch into the shim's generated vtable internally. A wrong slot or a
wrong `ret N` surfaces here as a crash or garbage, not as a subtly wrong pixel
three layers away.

**What it cannot show** is the overlay compositing: this is a console exe with
no swapchain, so the visible outcome is the degraded one — the native macOS
Steam window coming to the front on the right page. Two things cover the rest:

- `IsOverlayEnabled()` is checked in the direction that can only be wrong.
  `true` with `SHIM_OVERLAY` off is a hard FAIL (a title that pauses on
  activation would hang forever); `false` with it on is a legitimate outcome —
  injection lost its race, or there was no swapchain to hook — and is reported,
  not failed.
- Seeing a store page drawn *in* the overlay needs a real title. **Surviving
  Mars** is the one to use, and not by guesswork: it exports
  `SteamActivateGameOverlayToWebPage`, `...ToStore`, `...ToInvite` and
  `SteamIsOverlayEnabled` as *Lua* functions (`HGE::l_Steam*`), and
  `ModTools/Src/CommonLua/Core/lib.lua:2016` branches `OpenUrl()` on
  `SteamIsOverlayEnabled()` — overlay page if true, external browser if false.
  So both branches of the load-bearing predicate are visibly different, on
  demand, from the game's own console. (Among Us's IL2CPP metadata names all
  twelve, but that is Steamworks.NET's whole binding surface, not evidence of a
  call site.)

After a run, `/tmp/shim_unix.log` should show a `slot=NN fn=0x...` line per
activation with distinct, non-null `fn` values, and **no** `(unmapped)` line for
any `SteamFriends`/`SteamUtils` overlay method.

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
