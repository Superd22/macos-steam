# Seam spike — one real Steamworks call across the Wine boundary

> **Archived.** Question closed: the transport fork was decided and this spike was
> superseded wholesale by `src/shim`. **ADR 0001** holds the conclusion. Nothing here is
> rerun; it is kept as the evidence behind that ADR.
>
> One exception worth knowing about: `bridge_pe.c` holds the **only latency instrumentation
> in the repo** — ~40 ns for the bare seam, ~16–40 µs for `GetPersonaName`. `src/shim` has
> none. If seam cost ever needs re-measuring, extract that QPC loop rather than writing a
> new one.

Two stages, one throwaway harness (`attic/seam-spike/`), bottle `seamspike`.

- **Stage 1 (#8)** settled the *transport fork*: does a third-party unixlib even load
  in the shipped CrossOver, and does control cross `__wine_unix_call` into an unsigned
  `.so` in-process? PASS — a magic value round-tripped. That chose the unixlib (ADR 0001).
- **Stage 2 (#10)**, below, is the payload: put the **real** `steamclient.dylib` on the
  unix side, run #2's proven connection sequence **inside the Wine process**, and cross
  **one real Steamworks call** back with a correct value. This is what removes the last
  doubt under ADR 0001 — that the dylib might misbehave *specifically* inside Wine.

**Run on:** 2026-08-13, CrossOver 25.1.1 (Wine `wine-10.0-8474`), Apple Silicon, macOS 26.5.2,
native Steam.app running and **online** (asserted first with `native-probe/connprobe-arm64`:
`Steam_BConnected = TRUE`, `BLoggedOn = 1`).

## Stage 2 result: PASS

```
SPIKE2 OK: builtin loaded at 00006FFFFF2D0000       <- PE accepted as a builtin (high range)

--- steamclient.dylib, driven from inside Wine ---
  unix side       : Darwin pid 28781                 <- native macOS code ran, in-process
  dlopen          : ok
  CreateInterface : ok (SteamClient023)              <- modern iface served, inside Wine
  pipe / user     : 1 / 1
  Steam_BConnected: TRUE (live IPC)                  <- the honest online oracle: live link
  BLoggedOn       : 1
  *** SteamID     : 76561198014230730 ***            <- byte-identical to #2's standalone read
  PersonaName     : ²²²                              <- same filler rendering as #2, not a bug

--- round-trip latency (measured PE-side, QPC) ---
  seam noop RTT   : avg 0.040 us, min 0.000 us over 100k calls (echo ok)
  GetPersonaName  : avg 39.949 us, min 15.500 us over 10k calls -> "²²²"

SPIKE2 PASS
```

The one call that had to be unmistakably correct is `ISteamUser::GetSteamID()`. It returned
`76561198014230730`, **byte-identical to the value #2 read from a standalone native process**,
with `Steam_BConnected == TRUE` proving the value came from the live IPC link and not from
local cache in an offline client (map trap #1). The `²²²` persona rendering reproduces #2
exactly — the same account state, the same dylib, now hosted inside Wine.

**Provenance (negative control).** Re-run with `dist/` removed from `WINEDLLPATH`: the builtin
PE still loads, but the seam fails at `bridge_steam_init step -4, ntstatus 0xc0000135`
(`STATUS_DLL_NOT_FOUND`). The PE half holds no Steam knowledge of its own — every value in the
passing run demonstrably crossed the seam from the unix-side dylib.

## What this proves (the #10 questions, answered)

### `steamclient.dylib` behaves identically inside the Wine process
This was the one live risk ADR 0001 left open (its trigger 2: "the dylib proves unable to run
inside the Wine process specifically — symbol or CoreFoundation-runtime collisions, thread /
run-loop requirements a Wine process can't meet"). It does **not** happen. `dlopen`, the full
`CreateInterface → CreateSteamPipe → ConnectToGlobalUser` sequence, `Steam_BConnected`, and two
vtable reads all behave step-for-step as they did standalone in #2. **The unixlib transport is
confirmed end-to-end; the fallback triggers stay unfired.**

### Measured round-trip latency per call
- **Bare seam crossing (`__wine_unix_call`, no native work): ~40 ns average** over 100k calls,
  with per-call minima at/below the QPC tick — i.e. the transport itself is effectively free.
  The `#4` fear that a per-call boundary cost could disqualify a transport is dead: even
  loopback TCP's ~18 µs would have been fine, and the unixlib is ~450× cheaper than that.
- **A real Steamworks getter (`GetPersonaName`) across the seam: ~16 µs min, ~40 µs average**
  over 10k calls. That cost is the dylib's own work, not the seam (the seam is 40 ns of it). A
  game issuing even dozens of Steamworks calls per frame stays far inside a 16 ms frame budget.
  **The transport survives contact with a real game.**

### Which architecture each side is, and whether a boundary is crossed
All one arch: the PE is x86_64 (mingw-w64), the `.so` is x86_64 (`clang -arch x86_64`, running
under Rosetta 2), and `dlopen` binds the x86_64 slice of the universal `steamclient.dylib`.
**No architecture boundary is crossed** — the unix side reports the same process image the PE
runs in (`unix pid` is the host view of the Wine process). Consistent with #4 and #2: arm64
buys nothing for connecting, and the target arch stays a build-time switch (ADR 0001).

### The vtable layout reproduced by hand, and how brittle
Only the **leading** slots of three interfaces were transcribed (`steam_min.h`), each proven
against the live dylib in #2:
- `ISteamClient`: through `GetISteamFriends` (slot layout stable across `SteamClient017–023`).
- `ISteamUser`: `GetHSteamUser`(0), `BLoggedOn`(1), `GetSteamID`(2).
- `ISteamFriends`: `GetPersonaName`(0).

These leading layouts are stable across the shipped versions, so they are **not** brittle — the
brittleness #2/#3 warned about lives in `ISteamUserStats` (VERSION013 dropped
`RequestCurrentStats`, shifting every slot), which this spike **deliberately does not touch**.
`ISteamUtils::GetAppID` was also deliberately omitted: its slot is version-specific (a wrong
index segfaults with no clean error inside Wine, where we own no signal handler), and `GetAppID`
needs a real app context to return `3215050` — no game runs here. `GetSteamID` is the
unmistakable crossed value instead. **Achievement writing and the full stats vtable belong to
#11**, against a throwaway title.

## The loader hook used here vs. the production hook (do not conflate)

This spike loads its PE as **`bridgetest.dll` via the `0x40` builtin marker + `WINEDLLPATH`**
(the mechanism stage 1 proved), *not* as `steamclient64.dll` registered where the game looks.
That is on purpose: stage 2 isolates the **transport + dylib behavior**, not the hook. The
production hook — what makes the game load our PE in place of a real `steamclient.dll` — is
already settled by **#13**: the single load-bearing state is the registry value
`HKCU\Software\Valve\Steam\ActiveProcess\SteamClientDll64`; the beside-the-EXE local load
never fires in Mars' `steam_api64.dll`. #12 wires the two together (our PE, marker-stamped,
at the path that registry value points at, with its `.so` on the bottle's `DllPath`).

## Files

- `bridge_abi.h` — the flat, fixed-width struct + call-code contract shared by both halves.
- `bridge_unix.cpp` — unix half: hosts `steamclient.dylib`; entries noop / init / persona /
  shutdown behind the two `__wine_unix_call_funcs` arrays. **In C++ these arrays need explicit
  `extern` + `used`/`visibility("default")`** or file-scope `const` gives them internal linkage
  and `dlsym` can't find them — the one stage-2 build gotcha.
- `bridge_pe.c` — PE half: resolves the unixlib handle once, forwards call codes, times RTTs
  with `QueryPerformanceCounter`.
- `spike_main.c` — the trivial test exe standing in for the game. Gates PASS on
  `b_connected == 1` so an offline client can't produce a false pass.
- `steam_min.h` — the three interfaces' leading vtable slots (copied from `native-probe/`).
- `build.sh` / `run.sh [bottle]` / `patch_marker.py` — build, deploy-and-run, marker stamp.

## Reproduce

```sh
brew install mingw-w64                    # one-time
CXBIN="$HOME/Applications/CrossOver.app/Contents/SharedSupport/CrossOver/bin/cxbottle"
"$CXBIN" --bottle seamspike --create --template win10_64
# Launch native Steam.app and confirm it is ONLINE first:
( cd ../native-probe && ./connprobe-arm64 )     # expect Steam_BConnected = TRUE
./build.sh
./run.sh seamspike                        # expect: SPIKE2 PASS
```

## What stage 2 deliberately does NOT do

No achievement write (mutates the real account — #11). No `steamclient64.dll` registry hook
(#12/#13). No callback pump decode (#2 saw one delivered; threading model still open). One
call, correct answer, latency measured — the transport is now proven all the way to a live
Steamworks value, and #10 is done.
