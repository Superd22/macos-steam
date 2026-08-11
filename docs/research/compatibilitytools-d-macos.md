> [!CAUTION]
> **SUPERSEDED — the conclusion below is WRONG.** This document tested
> `~/Library/Application Support/Steam/compatibilitytools.d` (the Steam **data** dir).
> The real scan root on macOS is the Steam **install** dir, inside the app bundle:
> `~/Library/Application Support/Steam/Steam.AppBundle/Steam/Contents/MacOS/compatibilitytools.d/`.
>
> At the correct path, on the same client `1785187029`, **discovery works**,
> `to_oslist "macos"` is accepted, and `GetGlobalCompatTools()` returns the tool.
> See `compatibilitytools-d-macos-CORRECTED.md` and issue #5's correction comment.
>
> The static analysis of the client binary and UI bundle in this document remains accurate
> and useful; only the discovery conclusion is retracted.

# Does `compatibilitytools.d` discovery work on the macOS Steam client?

**Answer: No.** The machinery is fully compiled into the macOS client, but the local
tool-list scan never runs, and the client's compat-tool list is empty — including
Valve's own tools.

Resolves [#5](https://github.com/Superd22/macos-steam/issues/5).

## Environment

| | |
|---|---|
| Date | 2026-08-03 |
| Machine | Apple Silicon (arm64), macOS 26.5.2 |
| Steam bootstrap version | `1785187029` (installed, current) |
| Steam CEF | Chrome/126.0.6478.183 |
| SharedJSContext | `PLATFORM=macos&ARCH=arm64` |

## What the client contains (static)

`steamclient.dylib` is a universal x86_64+arm64 binary built by the
`steam_rel_client_hotfix_osx` buildbot. It carries the **complete** compat-tool
implementation — this is not a stripped-down macOS build:

- Source paths: `clientdll/compatmanager.cpp`, `clientdll/compatmanager.h`
- Scan roots: `/compatibilitytools.d` (relative to Steam root),
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

Log strings that *would* have been emitted by a scan:

```
Processing local tool list at %s...
No local tool list detected at %s.
Loaded manifest for tool %s.
Ignoring tool %s as it's for a different target platform %s.
Ignoring tool %s as it's for a different target platform %s arm64.
```

**None of these were ever emitted.** Note the arch-aware variant — the filter compares
target platform *and* arch, which matters if this path is ever revived.

The UI side exists too: `SteamClient.Apps.GetAvailableCompatTools`, `SpecifyCompatTool`,
`SteamClient.Settings.GetGlobalCompatTools`, `SpecifyGlobalCompatTool`.

## What is missing from the UI

The Steam Play **settings page does not exist in the macOS UI bundle.** Sweeping the
whole app bundle (excluding `public/` and `localization/`, which ship globally for all
platforms) for the page's own string keys returns nothing:

```
Steam_Settings_Compat_Title, _Enable, _Advanced_Title, _Default_Tool, _No_Default  → 0 hits
```

Only the launch-label keys (`Settings_Compat_Launch_*`) are compiled in. So there is no
UI path to turn Steam Play on.

The per-game **Properties → Compatibility** tab component *does* exist, but it renders
nothing when the tool list is empty — the checkbox is behind `0 != a.length` and
`disabled: !bCompatEnabled || 0 === a.length`. An empty list means an empty tab, not a
visible-but-disabled one.

## The live experiment

Created `~/Library/Application Support/Steam/compatibilitytools.d/macos-bridge-probe/`
with `compatibilitytool.vdf` (`from_oslist "windows"`, `to_oslist "macos"`),
a version-2 `toolmanifest.vdf` (`commandline "/probe.sh %verb%"`), and a `probe.sh`
that logs its argv and environment.

Steam was restarted **four times** — twice plain, once with `-cef-enable-debugging`,
once additionally with `-compat-disable-filtering -compat-force-slr`.

Results, identical every time:

| Signal | Result |
|---|---|
| `compat_log.txt` | never written — stayed at 980 bytes across all four startups |
| `compatibilitytool.vdf` / `toolmanifest.vdf` atime | frozen at creation time; never read |
| `probe.sh` | never invoked |
| `SteamClient.Settings.GetGlobalCompatTools()` | `[]` |
| `SteamClient.Apps.GetAvailableCompatTools(0)` | `[]` |
| `GetAvailableCompatTools(3215050)` (Surviving Mars) | `[]` |
| `GetAvailableCompatTools(1086940)` (BG3) | `[]` |
| `settingsStore.settings.bCompatEnabled` | `false` |
| `bCompatEnabledForOtherTitles` / `strCompatTool` | `false` / `""` |

**atime is a trustworthy signal on this volume**: over the same window, `registry.vdf`,
`config.vdf`, `loginusers.vdf` and `libraryfolders.vdf` all had their atimes updated by
Steam. The probe files did not.

`-compat-disable-filtering` changed nothing, which separates the two hypotheses: this is
**not** a `to_oslist "macos"` rejection. The scan does not run at all. Had it run and
rejected, `Ignoring tool ... different target platform` would have appeared.

Likewise the empty list is not a consequence of `bCompatEnabled == false` — the UI calls
`GetAvailableCompatTools` unconditionally, and the flag only gates the checkbox.

## The regression signal

`compat_log.txt` shows the compat manager was alive on this exact machine as recently as
**2025-09-03**, receiving server-side mappings:

```
[2025-09-03 22:57:29] Client version: 1751405894
[2025-09-03 22:57:29] Mapping AppID 0 to tool "proton_experimental" with priority 75
```

The installed client is now `1785187029` and writes nothing to this log. Between client
`1751405894` and `1785187029`, the macOS client stopped receiving *any* compat-tool
mappings — Valve's own included. This matches the Nov 2024 community report of a
`to_oslist "macos"` tool that worked and then silently stopped being discovered.

## Not tested

`/usr/local/share/steam/compatibilitytools.d` and `/usr/share/steam/compatibilitytools.d`
require `sudo` (`/usr/local/share` does not exist on this machine). Given that
`GetGlobalCompatTools()` is empty and *no* scan log line is emitted for *any* root, a
system-wide path is very unlikely to differ. To close it out:

```sh
sudo mkdir -p /usr/local/share/steam/compatibilitytools.d
# copy the probe tool in, restart Steam, re-check GetGlobalCompatTools()
```

## Reproducing the probe

The CDP harness used here (`cdp.mjs`, ~20 lines, Node 24 built-in `WebSocket`):

1. `touch "~/Library/Application Support/Steam/Steam.AppBundle/Steam/Contents/MacOS/.cef-enable-remote-debugging"`, restart Steam

   > **CORRECTED on [#9](https://github.com/Superd22/macos-steam/issues/9).** This step
   > originally named the Steam **data** dir. That is wrong — the flag must go in the
   > **install** dir, exactly as with `compatibilitytools.d`. Proven with the `atime`
   > oracle: the data-dir file is never read and no port opens; the install-dir file
   > brings 8080 up within 2s. The check lives in `steamui.dylib`; the default port
   > `8080` and `--remote-debugging-port=%s` live in `steamclient.dylib`.

2. `curl -s http://127.0.0.1:8080/json/list` → pick the `SharedJSContext` target
3. `Runtime.evaluate` with `awaitPromise: true` against its `webSocketDebuggerUrl`

This is a cheap, fully AFK instrument for interrogating the client's own API and is
directly reusable for [#6](https://github.com/Superd22/macos-steam/issues/6).

**Cleanup performed:** probe directory removed, `.cef-enable-remote-debugging` removed,
Steam relaunched with no flags. `config.vdf` and `registry.vdf` were backed up and
verified to differ only by routine churn (CM server list, `SteamPID`) — no compat keys
were written.
