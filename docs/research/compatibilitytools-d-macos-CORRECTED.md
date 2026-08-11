# `compatibilitytools.d` on macOS — it works (corrected)

**Answer: YES.** The macOS Steam client discovers local compatibility tools, accepts
`to_oslist "macos"`, and exposes the registered tool through its own API — on the current
client. The earlier negative result in `compatibilitytools-d-macos.md` was an artefact of
testing the wrong directory.

Corrects [#5](https://github.com/Superd22/macos-steam/issues/5).

## The error

| | Path |
|---|---|
| Tested (wrong) | `~/Library/Application Support/Steam/compatibilitytools.d/` — the Steam **data** dir |
| Correct | `~/Library/Application Support/Steam/Steam.AppBundle/Steam/Contents/MacOS/compatibilitytools.d/` — the Steam **install** dir |

The install dir is where `steam_osx` and `steamclient.dylib` live, and is the true macOS
analogue of Linux's `~/.steam/root`. The dylib's `/compatibilitytools.d` string fragment is
appended to the *install* directory.

Found via an r/macgaming thread supplied by the user, which quoted a `compat_log.txt`
excerpt showing the full path.

## Environment

| | |
|---|---|
| Date | 2026-08-03 |
| Machine | Apple Silicon (arm64), macOS 26.5.2 |
| Steam bootstrap version | `1785187029` |
| Steam CEF | Chrome/126.0.6478.183 |

## Result

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
  string in `steamclient.dylib`.
- **Manifest version 2** with `commandline "/probe.sh %verb%"` loads cleanly.
- **`STEAM_EXTRA_COMPAT_TOOLS_PATHS`** is present in the current dylib — an additional
  scan root via env var. Untested.
- **Registration does not require `bCompatEnabled`.** The tool registered with the flag
  `false`, so the missing Steam Play settings page is not a prerequisite for registration.

## Still true from the superseded document

- The full compat-tool implementation is compiled into the macOS `steamclient.dylib`.
- The Steam Play **settings page is absent from the macOS UI bundle** — zero hits for
  `Steam_Settings_Compat_Title` / `_Enable` / `_Advanced_Title` / `_Default_Tool` /
  `_No_Default` outside the globally-shipped `public/` and `localization/`.
- Valve's **own** server-side mapping did stop. `Mapping AppID 0 to tool
  "proton_experimental" with priority 75` appeared on every startup until 2025-09-03 on
  client `1751405894`, and never on `1785187029`. Still a real observation — but far less
  load-bearing now, since `app_mappings` lets us supply our own.
- The CDP harness and the `atime` oracle are unaffected and reusable.

## Retracted

- "The scan never runs."
- "This is not a `to_oslist` rejection." — The `-compat-disable-filtering` experiment
  proved nothing; there was no tool anywhere the client looked.
- "`GetGlobalCompatTools()` is empty, therefore no tool can register."
- "The Install half cannot ride on Steam Play." — Not established.

## The real open question

**Registration is not launching.** The r/macgaming thread reports a tool registering
successfully while the Play button stays greyed out, becoming active only under
`@sSteamCmdForcePlatformType windows` + `config_refresh`, with no compatibility UI shown.

natbro's hypothesis — **unverified**, and worth checking against the binary — is that the
macOS client is "plumbed only enough for those log messages to happen", with the download
and SteamPlay wiring compiled into the Linux client only, including the default loading of
the appid **`819390`** SteamPlay 2.0 Manifest that carries Proton config and app overrides.

So the frontier is: *does Steam actually install a Windows depot and launch through the
registered tool on macOS, and where does the chain break?*

Prior art to mine: **https://github.com/natbro/kaon**, a shipped "macOS Steam Play-like
integration" by the same author.

## Machine state

The probe tool is **still installed**, with the `app_mappings` block **removed** so no real
game is mapped to the no-op `probe.sh`. It is an inert registered tool, kept so follow-up
work doesn't repeat the restart dance. `.cef-enable-remote-debugging` removed; Steam
relaunched with no flags.

Caveat: this lives inside the Steam app bundle, so the bootstrapper may wipe it on client
update. `BootStrapperInhibitAll=enable` in `Steam.cfg` is the known counter (it is what
Millennium's macOS installer uses).
