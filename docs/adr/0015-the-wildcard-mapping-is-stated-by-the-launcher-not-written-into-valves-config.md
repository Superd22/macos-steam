# 15. The wildcard mapping is stated by the launcher, not written into Valve's config

Date: 2026-09-05

## Status

Accepted

Completes the **compat gate** ([research](../research/compat-vdf-platform-override.md)), which
turns the subsystem on but never says which tool it should reach for. Resolves
[#102](https://github.com/Superd22/macos-steam/issues/102).

## Context

A Windows-only title **already installed on another machine on the account** offered only
_Stream from `<machine>`_ in the macOS library. No install option anywhere — not on the button,
not in the machine picker. Every other Windows title installed and launched normally.

The machine picker is built from `per_client_data` on the app overview, one row per machine.
`CSteamUIAppController::FillInAppOverview` in `steamui.dylib` builds it, and the local row is
pushed only when a predicate on the app answers true. That predicate is
`is_available_on_current_platform` — the same value the row would have carried. A machine that
cannot run the title is not listed as unable to run it; it is not listed at all.

The value is the client's. `CUserAppManager::GetAppStateInfo` sets it through
`GetSystemConfigurationForApp`, the **compat-aware** variant, so there is no third
non-compat-aware call site — the hypothesis #102 opened with, and it was wrong. What that
function needs is `BIsCompatibilityToolEnabled(appid)`, which asks whether a tool is
**selected** for the title, not whether one is available. Nothing selects one for a title that
has never been installed: Steam writes the selection as part of installing.

Which is why the bug hid. With no other machine owning the title the machine list comes back
empty, no picker is drawn, the ordinary install button appears — and pressing it is what
creates the selection. A second machine makes the list non-empty, so the picker replaces that
button, and the picker is built from a list we were never added to. The one affordance that
would have made the selection is the one its absence removes.

On Linux and on Deck the same code runs, but the selection exists up front: "Enable Steam Play
for all other titles" writes it before any install. That settings page is drawn only when the
platform is Linux — one string comparison in `steamui/library.js` — so on macOS nothing ever
writes it.

### What we measured

Read live out of the running client, read-only, by locating the app objects in memory and
reading the flags word. Titles reading "not available on this platform", library-wide:

| configuration | unavailable |
|---|---|
| gate patched, no mapping | 455 / 750 |
| gate patched + mapping | **57 / 750** |
| mapping, gate **not** patched | 455 / 750 |

The third row is the control, run twice — once with the mapping in `config.vdf` and once with
it in the environment, byte-identical results both times. The client parses the mapping and
logs it either way, then never applies it: `BIsCompatibilityToolEnabled` tests the gate byte
before it reads anything. **Neither half works alone.**

Confirmed at the UI level in the same session: with both halves live, a title installed only on
the remote machine offers streaming *and* installing here.

## Decision

The launcher states the wildcard mapping in `steam_osx`'s environment:

```
STEAM_COMPAT_TOOL_MAPPINGS="0 <priority> <tool name>"
```

Beside `STEAM_EXTRA_COMPAT_TOOLS_PATHS`, which already delivers the tool the mapping names, and
subject to the same rule: **Valve's spelling, our value.** The variable name stays in
`Launch.swift` with the other borrowed names rather than moving into `layout.json`, because the
manifest owns the contract this project defines and cannot own a spelling we do not control.
The tool name is `ShimPath.toolName`, so the drift guard holds.

Priority is stated as **250**, which is the client's threshold rather than a round number above
it (`GetWildcardMapping`: `cmp w8, #0xfa`). Below it the entry is stored and never consulted,
which is exactly the failure the shipped `compatibilitytool.vdf` already has: its own
`app_mappings { "0" { priority "100" } }` is a *tool* declaring itself willing, it lands in a
different map, and raising that number to 250 was measured to change nothing.

### Rejected: writing `CompatToolMapping` into `config.vdf`

Measured working, and rejected on delivery. It is Valve's file; the client rewrites it on exit,
so the installer would have to refuse or defer while Steam is running; it outlives an uninstall
unless we also learn to unwrite it; and it would be the first thing we ship that edits a file
we do not own. The environment costs none of that and dies with the process.

### Rejected: patching the predicate in `steamui.dylib`

The four-instruction predicate is uniquely pattern-matchable and one `cset` away from always
true. Rejected because it is downstream of the real decision and would also claim every
*remote* machine can run every title. A second patched byte to say something false, when one
environment variable says something true.

## Consequences

- The shipped stack still patches exactly **one** byte. This issue adds no new patch surface,
  and nothing new to re-establish after a client update beyond what the gate already needs.
- Our tool becomes the default for every title with no tool of its own — deliberately, and the
  same thing the Linux toggle does. It is a library-wide behaviour change and belongs in the
  receipt rather than happening quietly.
- A title that already has a tool chosen keeps it: a wildcard mapping is the fallback, not an
  override.
- The mapping is only as live as the launcher. Steam started any other way is unmapped *and*
  ungated, so the two failure modes stay together instead of drifting apart.
- Addresses cited here are one `steamclient.dylib` / `steamui.dylib` build and expire with it.
  The full trace, with instructions, lives in #102.
