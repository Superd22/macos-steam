# Where the SteamPlay chain breaks on macOS

**Scope:** what the native macOS Steam client actually implements of the Steam Play /
compatibility-tool pipeline, and where the chain from *"tool is registered"* to *"Steam installs a
Windows depot and launches it through the tool"* is broken. The `compatibilitytools.d` discovery
question is settled in `compatibilitytools-d-macos.md` (and its correction); this document starts
downstream of registration.

**Investigated:** 2026-08-03.

Resolves [#15](https://github.com/Superd22/macos-steam/issues/15) (rescoped 2026-08-03).

> **Filename note.** This file was commissioned as `macos-compat-manager-regression.md`. The
> "regression" premise did not survive contact with the evidence — there is no regression in
> discovery — so it is filed under the rescoped title, as the ticket permits.

**Primary sources.** Static inspection of the shipped macOS client at
`~/Library/Application Support/Steam/Steam.AppBundle/Steam/Contents/MacOS/`, bootstrap version
`1785187029`: `strings`, `otool`, and arm64 disassembly of `steamclient.dylib` (all addresses below
are virtual addresses in the **arm64** slice). Valve's own delivered data from
`appcache/appinfo.vdf`, parsed with a binary-VDF reader. Valve's client changelogs via the
first-party news feed for app `593110`. Valve's `steam-compat-tool-interface.md` from
`gitlab.steamos.cloud/steamrt/steam-runtime-tools`. `natbro/kaon` at commit `2c5f864`. SteamDB
pages for app `891390` via Wayback captures (SteamDB mirrors PICS verbatim; its live API is
Cloudflare-gated).

---

## Summary (read this first)

1. **Registration is not the broken link — it works today.** On client `1785187029` a local tool at
   `Steam.AppBundle/Steam/Contents/MacOS/compatibilitytools.d/` is discovered, accepted with
   `to_oslist "macos"`, and surfaced through `GetGlobalCompatTools()`. §1.
2. **The scan root is the Steam *install* directory, not the data directory.** The client builds
   `<install>/compatibilitytools.d` plus two `/usr/[local/]share/steam/` roots plus
   `$STEAM_EXTRA_COMPAT_TOOLS_PATHS` (colon-separated). #5's original probe was planted in
   `~/Library/Application Support/Steam/compatibilitytools.d`, which **is not a scan root at all**.
   That is the whole explanation for the original negative result. §2.
3. **Valve's own tool list was never going to work on macOS, and never did.** App `891390`
   ("SteamPlay 2.0 Manifests") is present in this client's `appinfo.vdf`, but all 19 of its
   `compat_tools` have `to_oslist "linux"`, all 85 `app_mappings` have `platform "linux"`, and the
   literal string `macos` occurs **zero** times in the whole app. SteamDB captures show this has
   been true continuously since at least 2021. There is nothing to "restore". §3.
4. **Appid `819390` does not exist.** The manifest app is `891390`. natbro's number is a typo. §3.
5. **The manifest app is never processed on macOS at all** — not processed-and-rejected. The client
   holds `891390`'s data but its `CCompatManager::m_vecManifestAppIDs` is empty, so the
   per-manifest loop never runs and no `Ignoring tool …` line is ever emitted. §4.
6. **Everything downstream of registration *is* compiled into `steamclient.dylib`**: the full
   `STEAM_COMPAT_*` launch environment (16 variables), `%verb%` / `waitforexitandrun` /
   `getcompatpath`, `compatmanager_layer_name`, and the entire platform-override subsystem
   (`SetAppPlatformOverride`, `LoadPlatformOverrideCache`,
   `RemoteStorage_AppPlatformOverrideBackupComplete_t`). natbro's "compiled into the Linux client
   only" is **not** supported by the binary. §5.
7. **What is genuinely missing is the UI, not the engine.** The Steam Play settings page is absent
   from the macOS UI bundle, and `app_change_compat_tool` / `config_refresh` appear **nowhere** in
   the entire Steam installation. §6.
8. **The mapping-priority ladder is now known exactly**, from disassembly: **250** for a user
   mapping read from `Software\Valve\Steam\CompatToolMapping`, **100** for an `app_mappings` entry
   in a local `compatibilitytool.vdf`, **≤75** (hard-clamped) for a default/global AppID-0 mapping.
   Writing `CompatToolMapping` into `config.vdf` therefore outranks every other source. §7.
9. **The single highest-confidence lever is `@sSteamCmdForcePlatformType windows` in
   `steam_dev.cfg`** — the only mechanism anyone has demonstrated that makes the macOS client
   install a Windows depot. It carries two known costs (it disables client self-update, and it is
   global). §8.

**One-line answer to the ticket.** The chain does not break at registration and it does not break
at the launch engine; it breaks at **depot platform selection and Play-button enablement**, both of
which are driven by UI/config that the macOS client does not expose — and Valve's server-side tool
list is a red herring that was never macOS-capable.

---

## Environment

| | |
|---|---|
| Date | 2026-08-03 |
| Machine | Apple Silicon (arm64), macOS 26.5.2 |
| Steam bootstrap version | `1785187029` (installed, current) |
| `steamclient.dylib` | universal x86_64 + arm64, 54 MB, built by `steam_rel_client_hotfix_osx` |
| Installed apps | 2 (`1086940`, `3187030`) — **no** Proton, **no** `891390` |
| `steam_dev.cfg` | not present |

---

## 1. Registration works — the corrected experiment

The original #5 probe was planted in the Steam **data** directory and never read. Re-run at the
in-bundle path on the *same* client build, it works, reproducibly across two restarts
(`logs/compat_log.txt`):

```
[2026-08-03 18:19:15] Client version: 1785187029
[2026-08-03 18:19:15] Processing local tool list at /Users/david/Library/Application Support/Steam/Steam.AppBundle/Steam/Contents/MacOS/compatibilitytools.d/crossover-probe/compatibilitytool.vdf...
[2026-08-03 18:19:15] Registering tool crossover-probe, AppID 0
[2026-08-03 18:19:15] Recording non-user mapping for 3215050 at priority 100 to tool crossover-probe
[2026-08-03 18:19:15] Mapping AppID 3215050 to tool "crossover-probe" with priority 100
[2026-08-03 18:19:15] Loaded manifest for tool crossover-probe.
```

and the client's own API agrees:

| Call | Result |
|---|---|
| `SteamClient.Settings.GetGlobalCompatTools()` | `[{"strToolName":"crossover-probe","strDisplayName":"CrossOver Probe"}]` |
| `SteamClient.Apps.GetAvailableCompatTools(3215050)` | same |
| `SteamClient.Apps.GetAvailableCompatTools(0)` | same |
| `settingsStore.settings.bCompatEnabled` | `false` — **and registration happened anyway** |

Established by this: `to_oslist "macos"` is accepted; `app_mappings` inside a local
`compatibilitytool.vdf` works with no console command; a version-2 manifest with
`commandline "/probe.sh %verb%"` loads; `bCompatEnabled` does not gate registration.

**Retracted from #5:** "the local scan never runs". It runs; it was pointed at the wrong directory.
Everything else #5 established still stands.

---

## 2. The scan roots, exactly

`CCompatManager` builds its root list in the function at `0x7337e8`. Verified by disassembly:

| Order | Root | Evidence |
|---|---|---|
| 1 | `<Steam install dir>/compatibilitytools.d` | `0x733ac8` appends the 21-byte literal `/compatibilitytools.d` (`0x16a8a2c`) to a base string |
| 2 | `/usr/share/steam/compatibilitytools.d` | literal at `0x16a89bb`, pushed at `0x733824` |
| 3 | `/usr/local/share/steam/compatibilitytools.d` | literal at `0x16a89e1`, pushed at `0x7338e0` |
| 4 | `$STEAM_EXTRA_COMPAT_TOOLS_PATHS` | `getenv` at `0x733990`, split on `:` (literal `0x166f39d`) |

On macOS the install dir is `Steam.AppBundle/Steam/Contents/MacOS`, which the live log in §1
confirms empirically. Root (1) is **not** `~/Library/Application Support/Steam` — the data
directory is never scanned. Note that root (2)'s literal is the storage for root (1)'s suffix:
`/compatibilitytools.d` at `0x16a89cb` is a tail pointer into `/usr/share/steam/compatibilitytools.d`
at `0x16a89bb`.

Valve's own `steam-compat-tool-interface.md` documents only
"`~/.steam/root/compatibilitytools.d`" and hedges with "various locations such as" — it does not
enumerate the roots and never mentions the macOS bundle layout.

### The platform filter is an exact match

`CCompatManager::YldRegisterTool` (`0x72f31c`, name string at `0x16a7adf`) does, at `0x72f3d4`:

```
x0 = tool->to_oslist        (from the tool struct at +0x78, defaulted to "")
x1 = <current platform string>
bl  _strcmp                 ; lazy-ptr 0x18fd7d0 = _strcmp
cbz w8, accept              ; equal → continue
                            ; else → "Ignoring tool %s as it's for a different target platform %s."
```

then a secondary `V_stristr(…, "arm64")` branch (`0x72f3f8`, lazy-ptr `0x18fca10` =
`__Z9V_stristrPKcS0_`) feeding the arch-aware variant of the same message.

**Consequence:** `to_oslist` must be *exactly* `macos`. This is why the community report that
`to_oslist "osx"` yields `Ignoring tool crossover as it's for a different target platform osx.` is
correct [unverified community claim, but mechanically consistent with the disassembly].

`STEAM_EXTRA_COMPAT_TOOLS_PATHS` is **present in the binary but untested here.** It is also absent
from Valve's spec document and from `natbro/kaon` (0 hits in that repo).

---

## 3. Valve's own tool list: `891390`, all-Linux, always

App `891390` "SteamPlay 2.0 Manifests" **is** in this macOS client's `appcache/appinfo.vdf` (957
apps cached; `891390` present, `1493710`/`1070560`/`1628350` present). Parsed straight out of
Valve's delivered binary VDF:

```
appinfo.common.name       = "SteamPlay 2.0 Manifests"
appinfo.common.type       = "Config"
appinfo.common.oslist     = "linux"
appinfo.common.section_type = "ownersonly"
```

| Field | Distinct values across the whole app |
|---|---|
| `extended.compat_tools.*.to_oslist` | `"linux"` × 19 — **no other value** |
| `extended.compat_tools.*.from_oslist` | `"windows"` (proton_*) / `"linux"` (steamlinuxruntime_*) |
| `extended.app_mappings.*.platform` | `"linux"` × 85 — **no other value** |
| occurrences of the literal `macos` | **0** |

Example, verbatim:

```
proton_experimental: appid 1493710, depotid 1493711, require_tool_appid 4183110,
  display_name "Proton Experimental", depotsizemb 666,
  from_oslist "windows", to_oslist "linux"
```

**Appid `819390` does not exist** — it is not in the 957-app cache, and no such Valve app is known.
The manifest app is `891390`. The `819390` in the ticket and in natbro's write-up is a digit
transposition.

**This was never different.** Wayback captures of SteamDB's `891390` info page — SteamDB mirrors
PICS verbatim — show `to_oslist "linux"` for every tool and `common.oslist "linux"` continuously
from 2021-09-22 through 2026-05-18, including captures on **2025-08-28 and 2025-09-16**, which
bracket the date this machine last logged a `proton_experimental` mapping (2025-09-03). Pre-2023
captures render OS as SVG icons; those tables contain only `octicon-windows` and `octicon-linux`,
no Apple icon.

> **Caveat, stated plainly.** SteamDB's per-change diff list is AJAX-only and unarchived, and its
> live API is Cloudflare-gated. A `macos` value that appeared *and* vanished entirely inside the
> 2025-09-16 → 2025-11-22 gap cannot be formally excluded. The state is identical on both sides of
> that gap.

**Conclusion.** Reviving Valve's global list on macOS is not a lever, because there is nothing in it
a macOS client could accept — and Proton is a Linux runtime we have no use for anyway. The empty
`GetGlobalCompatTools()` observed in #5 was fully explained by this alone.

---

## 4. The manifest app is never *processed*, not processed-and-rejected

`CCompatManager` keeps a member `m_vecManifestAppIDs` at `+0x308` (name literal at `0x16a6a55`,
recovered from the class's `CValidator` block at `0x728700`). The loop at `0x72a15c`
(`compatmanager.cpp:3328`, from the yield string at `0x16a84c3`) walks it and calls the
tool-list processor at `0x72e7a0` once per appid.

That processor logs `Processing tool list from AppID %u` (`0x72e870`) **whenever** it successfully
retrieves the app's Extended section — the section fetch at `0x72e83c` passes `w2 = 3`
(`k_EAppInfoSectionExtended`), and only a null KeyValues result (`cbz` at `0x72e858`) suppresses
the line.

Neither `Processing tool list from AppID …` nor any `Ignoring tool … different target platform
linux.` line has *ever* appeared in this machine's `compat_log.txt` — not in the 2025 era, not in
today's successful run.

**Inference (high confidence, not directly observed):** `m_vecManifestAppIDs` is empty on macOS.
The client never enumerates `891390` as a manifest app, even though it has its data cached. This is
consistent with natbro's claim that Steam Play "forces the automatic installation of the SteamPlay
2.0 Manifests" only on Linux/SteamOS builds — and with the fact that `891390` is **not installed**
on this machine (only 2 appmanifests exist in `steamapps/`).

**Open question — the 2025 `priority 75` line.** This machine logged
`Mapping AppID 0 to tool "proton_experimental" with priority 75` on seven startups between
2025-07-10 and 2025-09-03, all on client `1751405894`, with no preceding `Registering tool` or
`Processing tool list` line. Given §3, it cannot have come from `891390` passing a `to_oslist`
filter. From disassembly, `AppID 0` + priority clamped to 75 is the **default/global tool** path
(`0x733dfc`: `if (priority >= 76) priority = 75`, then record with `w1 = 0`). No mapping is
persisted anywhere on disk today — `config.vdf`, `registry.vdf` and all 27 `userdata/` trees
contain no compat keys. **I could not determine the source of those 2025 lines and am not going to
guess.** It does not affect any conclusion below.

---

## 5. What is compiled *in* — more than expected

The claim that the SteamPlay wiring "is compiled into the Linux client only" is **not supported by
the macOS binary.** All of the following are present in `steamclient.dylib`:

**Full launch environment (16 variables):**

```
STEAM_COMPAT_APP_ID              STEAM_COMPAT_LIBRARY_PATHS      STEAM_COMPAT_SHADER_PATH
STEAM_COMPAT_CLIENT_INSTALL_PATH STEAM_COMPAT_MEDIA_PATH         STEAM_COMPAT_TOOL_MAPPINGS
STEAM_COMPAT_CONFIG              STEAM_COMPAT_MOUNTS             STEAM_COMPAT_TOOL_PATHS
STEAM_COMPAT_DATA_PATH           STEAM_COMPAT_PROTON             STEAM_COMPAT_TRANSCODED_MEDIA_PATH
STEAM_COMPAT_INSTALL_PATH        STEAM_COMPAT_PROTON_SUPPRESS    STEAM_COMPAT_LAUNCHER_SERVICE
STEAM_EXTRA_COMPAT_TOOLS_PATHS
```

**Launch-through-tool plumbing:** `%verb%`, `waitforexitandrun`, `commandline_waitforexitandrun`,
`commandline_getcompatpath`, `compatmanager_layer_name`, `SetProtonEnvironment`, `/compatdata`,
`%s/legacycompat/steamclient64.dll`-adjacent paths (`%s/legacycompat/iscriptevaluator.exe`,
`%s/legacycompat/evaluatorscript_%i.vdf`), plus runtime diagnostics that only make sense if the
launch path executes: `Warning: no session for AppID %u: STEAM_COMPAT_LIBRARY_PATHS,
STEAM_COMPAT_TOOL_PATHS (possibly others) cannot be updated for tool %u "%s".`

**The entire platform-override subsystem** — this is the machinery that makes Steam download a
depot for a platform other than the host:

```
CCompatManager::SetAppPlatformOverride       CCompatManager::GetAppPlatformOverride
CCompatManager::LoadPlatformOverrideCache    CCompatManager::FlushPlatformOverrideCache
CCompatManager::PlatformOverrides_t          m_mapAppPlatformOverride
RemoteStorage_AppPlatformOverrideBackupComplete_t
RemoteStorage_AppPlatformOverrideRestoreComplete_t
platform override cache: ignore bad entry %u "%s" "%s"
Shutting down: forcing platform override cache flush to disk.
@sSteamCmdForcePlatformType   @sSteamCmdForcePlatformBitness
```

**Verdict:** the engine is there. natbro's assessment that the path is "plumbed only enough for
those log messages to happen" is [unverified community claim] and, on the evidence of the binary,
**too pessimistic**. What has not been shown is that anything *calls* this machinery on macOS.

---

## 6. What is genuinely missing

| Missing thing | Evidence |
|---|---|
| Steam Play **settings page** | Sweeping the macOS UI bundle (excluding globally-shipped `public/` and `localization/`) for `Steam_Settings_Compat_Title`, `_Enable`, `_Advanced_Title`, `_Default_Tool`, `_No_Default` → **0 hits**. Only `Settings_Compat_Launch_*` labels are compiled in. (From #5, unchanged.) |
| Per-game **Compatibility tab** content | The component exists but is guarded by `0 != a.length` and `disabled: !bCompatEnabled \|\| 0 === a.length`. With the probe registered the list is no longer empty — **this is now retestable and was not retested.** |
| `app_change_compat_tool` console command | `grep -rl` across the **entire** `~/Library/Application Support/Steam` tree → **0 files**. Not in `steamclient.dylib`, not in the UI bundle. |
| `config_refresh` console command | Same sweep → **0 files**. |

So two of the four "console levers" in the ticket **do not exist in this build**. `@sSteamCmdForcePlatformType`
and `@sSteamCmdForcePlatformBitness` **do** exist, in `steamclient.dylib`.

Valve's `steam-compat-tool-interface.md` documents neither `app_mappings` (as a local
`compatibilitytool.vdf` section) nor `STEAM_EXTRA_COMPAT_TOOLS_PATHS`. Both are real — the first is
proven by the §1 log line, the second by the `getenv` at `0x733990` — the spec is simply
incomplete. The spec also documents only two `%verb%` values (`run`, `waitforexitandrun`);
`getcompatpath` survives only as a legacy **v1** `commandline_getcompatpath` key, and `getnativepath`
appears nowhere in Valve's repo. The spec never mentions macOS anywhere in its 744 lines, nor in
five years of commit history — silence, not prohibition.

---

## 7. The mapping-priority ladder (recovered from disassembly)

| Priority | Source | Evidence |
|---|---|---|
| **250** | User mapping read from `Software\Valve\Steam\CompatToolMapping` | `0x734244` sets `250`; `0x734254` loads the literal `Software\Valve\Steam\CompatToolMapping` at `__TEXT,__const 0x1559e90` |
| **100** | `app_mappings` entry in a local `compatibilitytool.vdf` | `0x72ee78` passes `100` as the default to the `KeyValues::GetInt` at `0x11c5e40`; confirmed live by the §1 log line |
| **≤75** | Default / global mapping for `AppID 0` | `0x733dfc`: `if (priority >= 76) priority = 75`, then recorded with appid `0` at `0x733e18` |

Also present: `Skip mapping AppID %u to tool "%s" with priority %d: mapping to tool "%s" with
priority %d already exists.` — higher priority wins, first writer of a given priority holds.

**Actionable:** a `CompatToolMapping` entry written into `config.vdf`'s `InstallConfigStore` binds
at **250** and outranks everything else, including any future Valve default. This is the strongest
mapping lever available and it needs no console command.

---

## 8. Levers, ranked

| # | Lever | Confidence | Notes |
|---|---|---|---|
| 1 | `@sSteamCmdForcePlatformType windows` in `<install>/steam_dev.cfg` | **High** | The only demonstrated way to make the macOS client install a Windows depot. Confirmed present in `steamclient.dylib`. Costs: the file's presence implies `-skipinitialbootstrap` (blocks client self-update), and it is **global** — every app with any macOS depot gets re-platformed. kaon documents it does *not* work from `steam.cfg` or from the command line, only `steam_dev.cfg`. [mechanism verified in binary; behaviour is kaon's claim, not retested here] |
| 2 | Local tool in `<install>/compatibilitytools.d/` with `to_oslist "macos"` | **Proven** | Already working (§1). Keep `to_oslist` exactly `macos`. |
| 3 | `CompatToolMapping` written into `config.vdf` `InstallConfigStore` | **High** | Priority 250, beats everything (§7). Verified from disassembly; not yet exercised live. |
| 4 | `app_mappings` block in the local `compatibilitytool.vdf` | **Proven** | Priority 100, no console needed (§1). Undocumented in Valve's spec but real. |
| 5 | `BootStrapperInhibitAll=enable` in `<install>/steam.cfg` | **Medium** | `BootStrapperInhibitAll`, `BootStrapperInhibitUpdateOnLaunch`, `BootStrapperInhibitClientChecksum`, `BootStrapperInhibitBootstrapperChecksum` and the paths `Steam.AppBundle/Steam/Contents/MacOS/steam.cfg` and `../Resources/steam.cfg` are all confirmed in `steam_osx`. Protects in-bundle files from update churn — at the cost of never updating. |
| 6 | `STEAM_EXTRA_COMPAT_TOOLS_PATHS` | **Medium** | Confirmed in the binary (`getenv` at `0x733990`, `:`-separated). Would keep our tool **outside** the app bundle, sidestepping lever 5 entirely. **Untested — worth testing early, it is cheap and high-value.** |
| 7 | Steam client **beta** branch | **Low** | No changelog in Aug 2025 – Feb 2026 mentions macOS compat tools at all; nothing suggests beta differs. |
| 8 | Forcing an appinfo re-download / installing `891390` | **Very low** | Pointless: its contents are 100% `to_oslist "linux"` (§3). |
| 9 | `-compat-force-slr`, `-compat-disable-filtering` | **Very low** | Present as strings; tried in #5 with no effect. `slr` is the Steam Linux Runtime. |
| 10 | `app_change_compat_tool` / `config_refresh` console commands | **Nil** | Do not exist anywhere in this installation (§6). |

---

## 9. What `natbro/kaon` actually is (and is not)

[verified from source, repo cloned at commit `2c5f864`]

kaon is **not** a compat-tool integration. Its README explicitly says the compat-tool route is
registrable but unusable, and it routes around the whole thing:

- **Install** ← `@sSteamCmdForcePlatformType windows` in `steam_dev.cfg` (lever 1).
- **Play** ← hand-editing an extra Windows *launch option* into
  `Steam/appcache/appinfo.vdf` (via a patched Steam-Metadata-Editor) that points at a 19-line shell
  script which sets `CX_BOTTLE` and `exec`s `wine "$@"`. No `toolmanifest.vdf`, no `%verb%`, no
  compat tool involved.
- **Library path alignment** ← creating a sparse APFS image, adding it as a Steam drive, then
  hand-repointing that entry in `libraryfolders.vdf` at the CrossOver bottle's Steam tree.
- **Steamworks** ← a **real Windows Steam client running inside the same bottle**. There is no
  bridge to the native `steamclient.dylib`. Achievements are mentioned once, as something the
  in-bottle Windows Steam provides.
- **`lsteamclient/`** in the repo is an **unmodified Proton 9.0 subtree** — commit message: "no
  changes yet to lsteamclient, available just to facilitate study and contributions". Its
  `toolmanifest_runtime.vdf` still says `"to_oslist" "linux"`.

Repo activity: created 2025-02-15, **last commit 2025-02-15** — one day of work, dead since, 417
stars, 4 open issues, author has replied to none. Two PRs from Scvairy closed unmerged.

**What this means for us.** kaon overlaps our *Install* half and confirms lever 1 works, but it is a
**negative result** for the compat-tool route and contributes **nothing** to the bridge — its
`lsteamclient` is stock Proton. Our destination (zero Windows Steam processes, native dylib bridge)
is strictly beyond what kaon does. Its most reusable asset is the `steam_dev.cfg` recipe and the
`libraryfolders.vdf` validation caveat ("the Steam client does a little bit of validation or
checksumming and seems to wipe out additional library entries it didn't initially make itself"),
which the map already flags under "Surviving Steam's own maintenance".

Its issue #6 (2026-05) independently reinvented the CDP harness #5 built, and its lone comment adds
a timing caveat worth keeping: send `@sSteamCmdForcePlatformType windows` only *after* the library
has fully loaded, "otherwise it'll fall back to updating all installed mac games as their windows
counterparts."

---

## 10. Valve's changelog record, Aug 2025 – Feb 2026

Every stable client update in the window (2025-09-09, 2025-10-02, 2025-11-17, 2025-12-19,
2026-01-21) and all 63 beta announcements were checked. **Not one mentions compatibility tools,
`compatibilitytools.d`, Steam Play on macOS, Proton on macOS, or Apple Silicon compat.** The only
macOS items are cosmetic (Tahoe app icon, Big Picture resolution, Remote Play resolution) or the
macOS 11 "Big Sur" end-of-life on 2025-10-15.

The nearest relevant Valve statement predates the window by a year — stable **2024-11-05**, under
the **Linux** heading:

> "Removed the UI toggle to disable Steam Play globally, correctly reflecting that Steam Play is
> always enabled on Linux. Steam Play was always partially active even when set to off in the UI as
> it is a requirement for Steam client operation."

A Steam Play rework explicitly scoped to Linux, landing 17 days before the known community report
([Steam Client Beta discussions, 2024-11-22](https://steamcommunity.com/groups/SteamClientBeta/discussions/0/4630358592048891288/),
zero replies, no Valve response). Suggestive, not probative.

**Client version timeline on this machine** (from `logs/bootstrap_log.txt`; build numbers are Unix
timestamps): `1751405894` (2025-07-01) … *gap, log rotated* … `1763531587` (2025-11-19),
`1763795278`, `1766177208`, `1766451605`, `1769025840`, `1773099986`, `1773426488`, `1777411435`,
`1778281814`, `1779486452`, `1779918128`, `1780352834`, `1781041600`, `1782257239`, `1782344391`, …
`1785187029` (2026-08-02). The 2025-09 → 2025-11 transition is not covered by the surviving log.

---

## 11. Proposed experiments (not run — Steam must not be disturbed)

Ordered by value-per-risk. All are reversible; **1** and **2** are read-mostly.

**E1 — Does the registered tool now render in the per-game Compatibility tab?**
No file changes at all: the probe tool is already registered. Via the CDP harness, evaluate the
Properties → Compatibility component's props for appid `3215050` and read
`settingsStore.settings.bCompatEnabled` again. #5 concluded the tab renders nothing *on an empty
list*; the list is no longer empty, so the guard `0 !== a.length` now passes and only
`disabled: !bCompatEnabled` remains. This distinguishes "no UI at all" from "UI present but
disabled", which decides whether `bCompatEnabled` is worth attacking.
*Risk: none.*

**E2 — Does `STEAM_EXTRA_COMPAT_TOOLS_PATHS` work?**
Move the probe tool out of the app bundle to e.g. `~/steam-compat-tools/`, relaunch Steam with that
directory in `STEAM_EXTRA_COMPAT_TOOLS_PATHS`, and check `compat_log.txt` for
`Processing local tool list at …`. If it works, lever 5 (`BootStrapperInhibitAll`, which freezes
client updates forever) becomes unnecessary. Cheap, high value.
*Risk: low — one env var, one restart.*

**E3 — Does a `CompatToolMapping` entry in `config.vdf` bind at priority 250?**
With Steam **stopped**, add
`InstallConfigStore/Software/Valve/Steam/CompatToolMapping/3215050 = { name "crossover-probe";
config ""; priority "250" }`, back up `config.vdf` first, restart, and read `compat_log.txt`.
Expected: `Mapping AppID 3215050 to tool "crossover-probe" with priority 250`. Confirms §7 and gives
a mapping source independent of the local vdf.
*Risk: low; `config.vdf` is backed up and Steam rewrites it routinely anyway.*

**E4 — The decisive one: does Install offer a Windows depot?**
Write `@sSteamCmdForcePlatformType windows` into
`Steam.AppBundle/Steam/Contents/MacOS/steam_dev.cfg`, restart, wait for the library to fully load,
then inspect `SteamClient.Apps.GetAppData(3215050)` / the Install dialog for a Windows depot.
**This is the experiment that answers the ticket's "done when".**
*Risk: material. It disables client self-update (`-skipinitialbootstrap`) and is global — any
installed game with a macOS depot may be re-platformed by the background updater. Do it with the
two installed titles' `appmanifest`s backed up, and delete the file immediately afterwards. Do not
leave it in place.*

**E5 — Does Steam actually exec the tool?**
Only after E4. With the probe's `probe.sh` logging argv and the full environment, press Play and
check whether `probe.sh` is invoked and with which `%verb%` and which `STEAM_COMPAT_*` variables.
This is the last unproven link and would tell us exactly what the tool contract looks like on macOS.

---

## 12. Open questions

1. **Why did this machine log `Mapping AppID 0 to tool "proton_experimental" with priority 75` in
   July–September 2025?** Not explicable from `891390` (§3), and no persisted mapping survives on
   disk. Unresolved. Low importance.
2. **What populates `m_vecManifestAppIDs`, and is it platform-gated or install-gated?** The strong
   candidate is that Linux force-installs `891390` and macOS does not, making the empty list a
   consequence of the app not being installed rather than an explicit macOS gate. Not settled.
3. **Does `bCompatEnabled == false` block anything downstream** (Play-button enablement, depot
   selection), now that it demonstrably does not block registration? E1 addresses this.
4. **Whether in-bundle `compatibilitytools.d` survives a client update.** The bootstrapper replaces
   files it manages; an unknown extra directory is *probably* left alone, but this is untested.
   E2 makes it moot if it succeeds.
5. **Linux/macOS binary diff.** Nothing here compares `steamclient.dylib` against Linux's
   `steamclient.so`. Every "compiled in" claim in §5 is a positive finding about the macOS binary;
   no claim is made about what Linux additionally has.
