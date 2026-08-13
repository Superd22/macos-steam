# 2. The compat gate flip ships as our own `.app` launcher (vehicle A), with Info.plist re-sign (vehicle B) as an opt-in toggle

Date: 2026-08-14

## Status

Accepted

## Context

Half A is proven: on a patched client the native macOS Steam client installs the Windows
depot per-app and hands the `.exe` to a compatibility tool with Valve's full
`STEAM_COMPAT_*` contract (issues #9, #15, #16). But every proven mechanism depends on two
things reaching the `steam_osx` process at launch:

1. `DYLD_INSERT_LIBRARIES` → our in-memory injector, which flips `m_bCompatEnabled`
   (runtime-only, dropped on restart, so a resident lever is mandatory).
2. `STEAM_EXTRA_COMPAT_TOOLS_PATHS` → our compat-tool dir (so the tool registers from
   outside the app bundle and survives client updates).

The blocker (#17): **`LSEnvironment` in Valve's outer `Steam.app` cannot carry `DYLD_*`.**
The outer bundle is hardened (`flags=0x10000(runtime)`, no `allow-dyld-environment-variables`
entitlement); dyld strips `DYLD_*` for hardened processes *and removes them from the
environment*, so the inner `steam_osx` never sees them. `LSEnvironment` can therefore deliver
`STEAM_EXTRA_COMPAT_TOOLS_PATHS` but **not** the injector.

Three candidate vehicles were on the table:

- **A — ship our own (unhardened) `.app`.** Its `LSEnvironment` carries both vars; its
  executable execs the inner `steam_osx`, which inherits the env across the exec. Touches
  none of Valve's files. Cost: a different Dock icon, and Steam only integrates when launched
  through us.
- **B — edit Valve's outer `Info.plist` + ad-hoc re-sign.** Re-signing drops the
  hardened-runtime flag, restoring `DYLD_*` flow, so launching Valve's own `Steam.app`
  directly carries the vars (native icon, Dock, login auto-start). Cost: strips Valve's
  signature on the outer bundle and breaks its resource seal; Gatekeeper may object on a
  fresh machine. Upside: client updates rewrite the *inner* bundle in Application Support,
  not `/Applications/Steam.app`, so the edit is unusually durable.
- **C — file patch, no launcher.** Patch `steamclient.dylib` on disk (size-padded so the
  bootstrapper's size-only check passes) and keep the tool inside the bundle's
  `compatibilitytools.d`, needing no env vars at all. Cost: both the patch and the tool dir
  are wiped by every client update — needs a "repair after Steam updates" step.

Ruled out earlier: `launchctl setenv` (injects our dylib into every GUI app),
`~/.MacOSX/environment.plist` (removed from modern macOS), and CDP/`SpecifyCompatTool`
(shipping an open remote-debugging port is a non-starter).

The key realisation that shaped the decision: **A and B deliver the *same two env vars* to
the same inner `steam_osx`; they differ only in *which bundle's `LSEnvironment` sets them*.**
The actual payload (injector dylib + compat-tool dir) is identical and lives outside any
bundle. The "vehicle" is only the delivery bundle — so A and B are not exclusive, and B is
naturally expressible as a capability *of* A.

Two facts settled the remaining questions (see the linked research):

- **The mapping is fully self-contained** — a static `app_mappings { "0" { … } }` block in
  the tool's `compatibilitytool.vdf` routes *every* Windows-only title through the tool at
  priority 100, with no runtime command and no per-appid enumeration driver
  (`app-mappings-self-contained.md`).
- **Steam has no per-title opt-out** of automatic compat routing — the only per-title control
  is opt-in to a *specific* tool; Deck Verified badges are informational, ProtonDB is
  third-party and not in-client (`steamdeck-steamplay-integration-model.md`). So we build no
  opt-out either.

## Decision

**Ship vehicle A as the default, with vehicle B as an opt-in toggle inside the same app.
Reject vehicle C.**

- **Default = A.** Our own unhardened `.app` sets both env vars in its `LSEnvironment` and
  execs the inner `steam_osx`. Touches zero Valve files; uninstall is deleting our app and
  the external payload dir. Fully reversible, no signature or Gatekeeper questions.

- **B is a capability of A, not a second artifact.** App A carries a single toggle
  ("Integrate with Steam's Dock icon"), off by default. Enabling it patches Valve's outer
  `Info.plist` (back up original → merge the two vars into `LSEnvironment`, alongside Valve's
  existing `LC_ALL` → `codesign --remove-signature` then `codesign -f -s -`) behind one admin
  prompt. From then on, launching Valve's own `Steam.app` directly also carries the vars.
  Because both A and B read the same external payload, they never conflict.

- **B's uninstall does not restore Valve's signature; a Steam reinstall does.** Disabling B
  restores the original `Info.plist` bytes but leaves the bundle ad-hoc signed. Full pristine
  Valve signature = reinstall Steam, which leaves `steamapps`/library untouched. This is the
  one irreversible cost of B, which is exactly why B is opt-in behind an explicit warning.

- **Reject C.** Its update-fragility (patch + tool dir wiped every client update, needing a
  repair step) is the worst shipping story of the three, and A already achieves the same "no
  env var needed" cleanliness without ever touching a Valve binary.

### Bundle layout (specified well enough to build)

**External payload — shared by A and B, outside every bundle, survives client updates**
(e.g. `~/Library/Application Support/<our-app>/`):

- `libcompat-enabler.dylib` — the injector. **Built universal (arm64 + x86_64)** so it loads
  cleanly whether inherited by the arm64 `steam_osx` or the x86_64-under-Rosetta CrossOver
  processes Steam launches; it no-ops outside `steam_osx`. Pointed at by
  `DYLD_INSERT_LIBRARIES`.
- `compatibilitytools.d/<our-tool>/` — the compat tool, pointed at by
  `STEAM_EXTRA_COMPAT_TOOLS_PATHS`. Contains:
  - `compatibilitytool.vdf` with `to_oslist "macos"` and a static
    `app_mappings { "0" { … priority 100 } }` catch-all → every Windows-only title routes
    through the tool, no driver.
  - the launch script Steam invokes as `waitforexitandrun <game>.exe` with the full
    `STEAM_COMPAT_*` environment. **This script is the boundary with Half B (#12)** — it is
    what actually invokes CrossOver against the `.exe`; its internals (bottle selection, the
    `steamclient64.dll` shim + `SteamClientDll64` registry hook) are #12's territory, not this
    ADR's.

**Vehicle A — our `.app`:** an unhardened bundle whose `Info.plist` `LSEnvironment` sets
`DYLD_INSERT_LIBRARIES` (→ the injector) and `STEAM_EXTRA_COMPAT_TOOLS_PATHS` (→ the tool
dir), and whose executable execs `…/Steam.AppBundle/Steam/Contents/MacOS/steam_osx`. Also
hosts the B toggle and the uninstaller.

**Mapping = one static line**, no resident component. Per-title opt-out is not built (Steam
has none). A per-title *opt-in to a specific tool* remains expressible later as a priority-250
`config.vdf` entry, but is out of scope for v1.

## Consequences

- The shipped product is a single `.app` download that touches no Valve files by default and
  uninstalls to verifiably-stock Steam. B is an opt-in power-user upgrade with a clearly
  disclosed, reinstall-reversible signature cost.
- No CDP / open debug port ever ships; CDP stays a dev instrument only.
- No resident library-watcher or per-appid enumeration is needed — the `appid 0` catch-all
  does the whole job statically.
- **Open verification carried into #12/build:** confirm the `appid 0` catch-all leaves
  macOS-native and dual-platform titles (and the `appid 0` non-Steam-shortcut edge case)
  untouched — only titles at `display_status 14` should ever route through us, honouring the
  hard requirement that a macOS-compatible title installs as macOS.

## Links

- Issue [#17](https://github.com/Superd22/macos-steam/issues/17) — the shipping-vehicle
  decision this ADR resolves.
- `docs/research/app-mappings-self-contained.md` — the self-contained mapping + `appid 0`
  catch-all evidence.
- `docs/research/steamdeck-steamplay-integration-model.md` — no per-title opt-out in Steam.
- `docs/research/compat-vdf-platform-override.md` — the gate patch, size-padding, injector.
- ADR 0001 — the bridge transport (Half B), whose launch-script seam this bundle hands off to.
