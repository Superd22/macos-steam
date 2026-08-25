---
status: current
re-verify-on: Steam client update — live-tested against client build `1785799196`
---

# `app_mappings` is a self-contained, shippable compat-tool mapping, and `appid 0` is a global catch-all

Live-tested against the running macOS Steam client on 2026-08-13 (client build
`1785799196`), gate flipped in-memory, zero downloads. Resolves the gating fact of issue
[#17](https://github.com/Superd22/macos-steam/issues/17): can a shipped bundle map its
compat tool to titles without the CDP-only `SpecifyCompatTool` path (an open remote
debugging port is a non-starter to ship).

## Question

Does a static `app_mappings` block inside a compat tool's own `compatibilitytool.vdf`
(with no `SpecifyCompatTool` call, no `ExecCommand`, no force-platform convar, and an
empty `config.vdf` `CompatToolMapping`) make a Windows-only title installable once
`m_bCompatEnabled` is flipped? And is there a global-default mechanism, or must every
appid be enumerated?

## Answer

**Both yes.** The static vdf mapping alone works, and a single `appid "0"` entry maps the
tool to *every* Windows-only title at once.

### Per-appid static mapping works (`display_status 14 → 9`)

Test title **Absolver (473690)**, a real Windows-only game, uninstalled, never previously
mapped. Only mapping present was the static priority-100 `app_mappings` entry in the tool's
`compatibilitytool.vdf`.

| Title | Mapping | Gate | `display_status` |
|---|---|---|---|
| **473690 Absolver** | static `app_mappings`, priority 100 | OFF → **ON** | **14 → 9** |
| 318020 Act of Aggression | none (control) | ON | 14 (unchanged) |
| 620980 Beat Saber | none (control) | ON | 14 (unchanged) |

Unmapped controls staying at **14** with the gate ON prove this is a per-app effect
from the static mapping, not a global host-platform flip.

`display_status` enum: `6` installed, `9` ready to install, `14` unavailable on platform.

### `appid "0"` is the global catch-all (the Steam Play "all other titles" analogue)

With `"0" → our-tool` added to `app_mappings`, every previously-unmapped Windows-only
control flipped at once:

| Title | With `appid 0` catch-all, gate ON |
|---|---|
| 318020 Act of Aggression | 14 → **9** |
| 620980 Beat Saber | 14 → **9** |
| 457860 Apollo 11 VR | 14 → **9** |

Steam log: `Mapping AppID 0 to tool "crossover-probe" with priority 100`. This matches
Valve's own server-side default bucket (`Mapping AppID 0 to tool proton_experimental with
priority 75`), and `GetAvailableCompatTools(0)` returns the tool as the default-bucket
query. No per-appid enumeration driver / resident library-watcher is needed. One
static line maps everything.

## Evidence Steam actually parsed our vdf (log-line oracle)

`compat_log.txt`, gate-on session:

```
Processing local tool list at .../compatibilitytools.d/crossover-probe/compatibilitytool.vdf...
Recording non-user mapping for 473690 at priority 100 to tool crossover-probe
Mapping AppID 473690 to tool "crossover-probe" with priority 100
Recording non-user mapping for 480 at priority 100 to tool nonexistent-probe-INVALID-oracle
```

"non-user mapping … priority 100" is the static-vdf path; a `SpecifyCompatTool` (user)
mapping is priority **250** in `config.vdf`. Steam echoed the custom tool name
`nonexistent-probe-INVALID-oracle` verbatim: that string exists only in the planted file,
an unambiguous parse oracle.

## Priority model (critical to the per-title override question)

- Static `app_mappings` in the tool's vdf → **priority 100** ("non-user mapping").
- `SpecifyCompatTool` / user Properties override → **priority 250** in `config.vdf`
  `CompatToolMapping`, which overrides the priority-100 catch-all.

So a per-title user override is expressible as a 250 entry, but note the parallel finding in
[`steamdeck-steamplay-integration-model.md`](./steamdeck-steamplay-integration-model.md):
Steam offers no per-title *opt-out*, only opt-in to a specific tool.

## Traps respected during the test

- **Online asserted** before trusting results (`navigator.onLine`, logged in, 354-app
  library loaded). Offline is a convincing false negative.
- **Confound eliminated:** `config.vdf` held leftover priority-250 user mappings for 3215050
  and 945360 from prior CDP sessions; those override priority-100, so those titles were
  ambiguous. Backed up `config.vdf`, emptied `CompatToolMapping`, used a fresh never-mapped
  title (473690).
- **Zero downloads** — `display_status` reads only.
- Gate flipped with the in-memory DYLD injector
  (`src/compat-enabler/libcompat-enabler.dylib`, pattern-scan, 1 site patched per launch);
  no Valve file modified. Full restore afterward (tool removed, `config.vdf` restored,
  CDP port 8080 closed, `steamclient.dylib` byte-for-byte stock).

## Consequence for #17

The shipped bundle is **fully self-contained**: a single `app_mappings { "0" { … } }` block
in the tool's `compatibilitytool.vdf` routes every Windows-only title through the tool with
no runtime command, no CDP, and no enumeration driver. CDP is a dev instrument only and
never ships.
