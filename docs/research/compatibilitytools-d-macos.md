---
status: current
re-verify-on: Steam client update — the scan root lives inside the Steam app bundle, so the bootstrapper can wipe a planted tool on upgrade
---

# `compatibilitytools.d` on macOS

**Answer: it works.** The macOS Steam client discovers local compatibility tools, accepts
`to_oslist "macos"`, and hands the registered tool back through its own API. Registration
does not require the compat gate to be on.

Resolves [#5](https://github.com/Superd22/macos-steam/issues/5).

The scan root is the Steam **install** dir, inside the app bundle:

```
~/Library/Application Support/Steam/Steam.AppBundle/Steam/Contents/MacOS/compatibilitytools.d/
```

Not the Steam **data** dir (`~/Library/Application Support/Steam/compatibilitytools.d/`). The
install dir is where `steam_osx` and `steamclient.dylib` live, and is the true macOS analogue of
Linux's `~/.steam/root`; the dylib's `/compatibilitytools.d` string fragment is appended to it.
Getting this wrong costs a whole round of research — see [Wrong turn](#wrong-turn-the-data-dir)
at the bottom.

## Environment

| | |
|---|---|
| Date | 2026-08-03 |
| Machine | Apple Silicon (arm64), macOS 26.5.2 |
| Steam bootstrap version | `1785187029` |
| Steam CEF | Chrome/126.0.6478.183 |
| SharedJSContext | `PLATFORM=macos&ARCH=arm64` |

## The live result

Fixture planted at `<install>/compatibilitytools.d/crossover-probe/`:

```
"compatibilitytools"
{
  "compat_tools"
  {
    "crossover-probe"
    {
      "install_path" "."
      "display_name" "CrossOver Probe"
      "from_oslist"  "windows"
      "to_oslist"    "macos"
      "unlisted"     "0"
    }
  }
  "app_mappings"
  {
    "3215050"
    {
      "appid" "3215050"  "platform" "macos"  "tool" "crossover-probe"
      "config" "none"    "comment" "Surviving Mars Relaunched"
    }
  }
}
```

plus a version-2 `toolmanifest.vdf` (`commandline "/probe.sh %verb%"`).

`compat_log.txt`, reproducible across two restarts:

```
[2026-08-03 18:19:15] Client version: 1785187029
[2026-08-03 18:19:15] Processing local tool list at /Users/david/Library/Application Support/Steam/Steam.AppBundle/Steam/Contents/MacOS/compatibilitytools.d/crossover-probe/compatibilitytool.vdf...
[2026-08-03 18:19:15] Registering tool crossover-probe, AppID 0
[2026-08-03 18:19:15] Recording non-user mapping for 3215050 at priority 100 to tool crossover-probe
[2026-08-03 18:19:15] Mapping AppID 3215050 to tool "crossover-probe" with priority 100
[2026-08-03 18:19:15] Loaded manifest for tool crossover-probe.
```

Client API, via the CDP harness against `SharedJSContext`:

| Call | Result |
|---|---|
| `SteamClient.Settings.GetGlobalCompatTools()` | `[{"strToolName":"crossover-probe","strDisplayName":"CrossOver Probe"}]` |
| `SteamClient.Apps.GetAvailableCompatTools(3215050)` | same |
| `SteamClient.Apps.GetAvailableCompatTools(0)` | same |
| `settingsStore.settings.bCompatEnabled` | `false` |

## Established

- **Local discovery works** on client `1785187029`, from the in-bundle install dir.
- **`to_oslist "macos"` is accepted.** Not `osx` — that yields
  `Ignoring tool ... for a different target platform osx.`
- **`app_mappings` works.** An undocumented `compatibilitytool.vdf` section mapping an
  appid to a tool at priority 100, with no console command needed. Confirmed by the
  `Recording non-user mapping ... at priority 100` line. `app_mappings` is present as a
  string in `steamclient.dylib`. Followed up in full in `app-mappings-self-contained.md`,
  which also establishes the `appid 0` global catch-all.
- **Manifest version 2** with `commandline "/probe.sh %verb%"` loads cleanly.
- **`STEAM_EXTRA_COMPAT_TOOLS_PATHS`** is present in the current dylib — an additional
  scan root via env var. Untested.
- **Registration does not require `bCompatEnabled`.** The tool registered with the flag
  `false`, so the missing Steam Play settings page is not a prerequisite for registration.

## What the client contains (static)

`steamclient.dylib` is a universal x86_64+arm64 binary built by the
`steam_rel_client_hotfix_osx` buildbot. It carries the **complete** compat-tool
implementation — this is not a stripped-down macOS build:

- Source paths: `clientdll/compatmanager.cpp`, `clientdll/compatmanager.h`
- Scan roots: `/compatibilitytools.d` (relative to the Steam **install** root),
  `/usr/local/share/steam/compatibilitytools.d`, `/usr/share/steam/compatibilitytools.d`
- Per-tool files: `%s/%s/compatibilitytool.vdf`, `/toolmanifest.vdf`
- vdf keys: `compat_tools`, `display_name`, `install_path`, `depotsize_mb`,
  `from_oslist`, `to_oslist`, `require_tool_appid`, `unlisted`
- Manifest keys: `commandline`, `%verb%`, `commandline_waitforexitandrun`,
  `commandline_getcompatpath`, `compatmanager_layer_name`;
  asserts `pTool->nToolManifestVersion == 2`
- Jobs: `CLoadLocalToolListJob::ThreadedListLocalToolManifests`,
  `CLoadLocalCompatibilityToolManifestJob`, `CRegisterCompatToolJob`,
  `CSpecifyAppCompatToolJob`
- Registry path: `Software\Valve\Steam\CompatToolMapping`
- Platform tokens present as exact strings: `macos`, `windows`, `linux`, `arm64`, `x86_64`
- Launch flags: `-compat-disable-filtering`, `-compat-force-slr`

The scan's own log family:

```
Processing local tool list at %s...
No local tool list detected at %s.
Loaded manifest for tool %s.
Ignoring tool %s as it's for a different target platform %s.
Ignoring tool %s as it's for a different target platform %s arm64.
```

Note the arch-aware variant — the filter compares target platform *and* arch.

The UI side exists too: `SteamClient.Apps.GetAvailableCompatTools`, `SpecifyCompatTool`,
`SteamClient.Settings.GetGlobalCompatTools`, `SpecifyGlobalCompatTool`.

## What is missing from the UI

The Steam Play **settings page does not exist in the macOS UI bundle.** Sweeping the
whole app bundle (excluding `public/` and `localization/`, which ship globally for all
platforms) for the page's own string keys returns nothing:

```
Steam_Settings_Compat_Title, _Enable, _Advanced_Title, _Default_Tool, _No_Default  → 0 hits
```

Only the launch-label keys (`Settings_Compat_Launch_*`) are compiled in. There is no UI path
to turn Steam Play on — which is why the gate is flipped in the binary instead
(`compat-vdf-platform-override.md`).

The per-game **Properties → Compatibility** tab component *does* exist, but it renders
nothing when the tool list is empty — the checkbox is behind `0 != a.length` and
`disabled: !bCompatEnabled || 0 === a.length`. An empty list means an empty tab, not a
visible-but-disabled one.

## Valve's own server-side mapping did stop

`compat_log.txt` shows the compat manager alive on this machine as recently as
**2025-09-03**, receiving server-side mappings:

```
[2025-09-03 22:57:29] Client version: 1751405894
[2025-09-03 22:57:29] Mapping AppID 0 to tool "proton_experimental" with priority 75
```

That line appeared on every startup on client `1751405894` and never on `1785187029`.
Somewhere between those builds the macOS client stopped receiving Valve's mappings, which
matches the Nov 2024 community report of a `to_oslist "macos"` tool that worked and then
silently stopped being discovered. Still a real observation — but it no longer gates
anything, because `app_mappings` lets a tool supply its own.

## Reproducing the probe

The CDP harness used here (`cdp.mjs`, ~20 lines, Node 24 built-in `WebSocket`):

1. `touch "~/Library/Application Support/Steam/Steam.AppBundle/Steam/Contents/MacOS/.cef-enable-remote-debugging"`
   — the **install** dir, same as `compatibilitytools.d`. Proven with the `atime` oracle: a
   data-dir file is never read and no port opens; the install-dir file brings 8080 up within
   2s. The check lives in `steamui.dylib`; the default port `8080` and
   `--remote-debugging-port=%s` live in `steamclient.dylib`. The `-cef-enable-debugging`
   launch flag is an equivalent, per-run alternative.
2. Restart Steam; `curl -s http://127.0.0.1:8080/json/list` within ~15s of UI paint — pick
   the `SharedJSContext` target
3. `Runtime.evaluate` with `awaitPromise: true` against its `webSocketDebuggerUrl`

A cheap, fully AFK instrument for interrogating the client's own API; it is what produced
`steamclient-js-api-macos.md`.

**atime is a trustworthy signal on this volume**: over the same window, `registry.vdf`,
`config.vdf`, `loginusers.vdf` and `libraryfolders.vdf` all had their atimes updated by
Steam while untouched files did not.

## What this did not settle

**Registration is not launching.** An r/macgaming thread reports a tool registering
successfully while the Play button stays greyed out, becoming active only under
`@sSteamCmdForcePlatformType windows` + `config_refresh`, with no compatibility UI shown.
Where that chain breaks is the subject of `macos-steamplay-chain.md`, and the gate that
turns it on is `compat-vdf-platform-override.md`.

## Machine state

The probe tool was left installed with its `app_mappings` block removed, so no real game is
mapped to the no-op `probe.sh`. `.cef-enable-remote-debugging` removed; Steam relaunched with
no flags. `config.vdf` and `registry.vdf` were backed up and verified to differ only by
routine churn (CM server list, `SteamPID`) — no compat keys were written.

Caveat: the tool lives inside the Steam app bundle, so the bootstrapper may wipe it on client
update. `BootStrapperInhibitAll=enable` in `Steam.cfg` is the known counter (it is what
Millennium's macOS installer uses).

---

## Wrong turn: the data dir

The first pass at this question, on the same client and the same day, concluded **"No —
discovery does not work on macOS."** It planted the probe tool in
`~/Library/Application Support/Steam/compatibilitytools.d/`, the Steam **data** dir, and
measured, across four restarts (twice plain, once with `-cef-enable-debugging`, once with
`-compat-disable-filtering -compat-force-slr`):

| Signal | Result |
|---|---|
| `compat_log.txt` | never written — stayed at 980 bytes across all four startups |
| `compatibilitytool.vdf` / `toolmanifest.vdf` atime | frozen at creation time; never read |
| `probe.sh` | never invoked |
| `SteamClient.Settings.GetGlobalCompatTools()` | `[]` |
| `SteamClient.Apps.GetAvailableCompatTools(0)` | `[]` |
| `settingsStore.settings.bCompatEnabled` | `false` |

Every one of those readings is real. They mean only that the client never looked in that
directory, which is exactly what a client scanning a different directory does.

Retracted with the conclusion:

- "The scan never runs." It runs, on every startup, over the install dir.
- "This is not a `to_oslist` rejection, because `-compat-disable-filtering` changed nothing."
  The flag experiment proved nothing — there was no tool anywhere the client looked, so
  filtering was never reached either way.
- "`GetGlobalCompatTools()` is empty, therefore no tool can register."
- "The Install half cannot ride on Steam Play." Never established.
- The framing of the vanished Valve mapping as a **regression** that had broken discovery.
  Two separate facts got fused: Valve's server-side mappings did stop, and the local scan
  works. Only the first is a change in the client.

The correct path came from an r/macgaming thread that quoted a `compat_log.txt` excerpt
showing the full scan path. The same install-vs-data-dir mistake was made a second time,
independently, for `.cef-enable-remote-debugging`, and corrected on
[#9](https://github.com/Superd22/macos-steam/issues/9) — which is the argument for stating
the rule once, at the top: **anything the client scans for lives in the install dir, inside
the app bundle.**
