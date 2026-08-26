# 12. Steam's per-title overlay setting vetoes ours

Date: 2026-08-26

## Status

Accepted

Widens the switch introduced in [ADR 0006](0006-one-predicate-owns-the-overlay-switch.md),
which deferred exactly this ("widening the value space is a separate decision, and easier to
take once there is one predicate to widen").

## Context

`SHIM_OVERLAY` is global. Steam's own "Enable the Steam Overlay while in-game" is not: it
exists globally (Settings → In Game) **and per game** (Properties → General). We ignored it,
so a user who wanted the overlay everywhere except one title had no way to say so — the only
lever was turning it off for the whole library.

The motivating case is a title that cannot start at all with the overlay on. Age of Empires
IV's Aegis anti-tamper rejects the injector's import rewrite and debugger attach, so the
overlay is not a degraded experience there, it is a launch failure. Anti-tamper titles are a
category, not one game, and the category will grow.

### What we measured

#92 sketched three routes, cheapest first. Route 1 — "check whether Steam already tells us" —
turned out to be the whole answer, and two of the issue's own assumptions were wrong.

The compat tool's inherited environment was dumped for a default-on title (Among Us, 945360)
and a title with the box unticked (AoE IV, 1466860). One variable differs:

| signal | overlay on | overlay off | |
|---|---|---|---|
| **`SteamNoOverlayUI`** | absent | **`1`** | the answer |
| `ENABLE_VK_LAYER_VALVE_steam_overlay_1` | `1` | `1` | — |
| `SteamOverlayGameId` | `945360` | `1466860` | — |
| `STEAM_DYLD_INSERT_LIBRARIES` | renderer | renderer | — |

`ENABLE_VK_LAYER_VALVE_steam_overlay_1` is a decoy worth naming: it *is* `0` for the
`iscriptevaluator` helper invocation and `1` for the real launch, so it looks like a
per-launch overlay answer until you compare two titles. It tracks which invocation this is,
not what the user asked for.

**The signal is live.** Unticking the box for Among Us and relaunching it — same running
Steam, no restart — produced `SteamNoOverlayUI=1` on the very next launch. This is the
finding that decided the route: #92 assumed we would have to parse `localconfig.vdf` and
accept a stale-until-Steam-restarts contract, because toggling the setting writes nothing to
disk while Steam is running. Steam computes this variable from memory at launch time, so the
staleness problem never arises.

For the record, the vdf key #92 went looking for does exist, at
`UserLocalConfigStore/apps/<appid>/OverlayAppEnable` — not under `Software/Valve/Steam/apps/`
as the issue guessed, and materialising only once the setting is non-default. We do not read
it. It is the same answer, one flush behind.

## Decision

**The manifest's `OVERLAY` switch grows a `veto` input, and the generated predicate consults
both.** The overlay is delivered when `SHIM_OVERLAY` is not a stated off value **and** Steam
has not said no for this launch.

```
SHIM_OVERLAY=0                    -> off, always. The launcher's kill switch.
SteamNoOverlayUI=1                -> off for this title. Steam's answer.
neither                           -> on, subject to the injector interlock.
```

- The veto is declared in `layout.json` beside the switch it modifies — env var, on-values,
  and the prose explaining what it is. `gen.py` emits the combined rule into all three
  dialects; no site tests `SteamNoOverlayUI` by hand.
- `check.py` guards the veto's name the way it guards our own. Whole-name matching keeps it
  from flagging `SteamNoOverlayUIDrawing`, which is a **different flag** — the renderer's,
  not the client's — that our launch path already sets in five places. The two names differ
  by a suffix and answer different questions; this is the sharpest edge in the change.
- `check_policy.py` now checks the **cross-product** of both variables, 55 combinations
  rather than 11. The corner that motivates it: the C emitter returned early on the unset
  case, before any second input could be consulted, so a one-axis table would have passed an
  emitter that ignored the veto entirely.
- A diagnostics-only `shim_overlay_vetoed` is emitted for shell, so the launch script can say
  *which* input turned the overlay off without re-deriving the rule.

### `SHIM_OVERLAY=1` means "do not force off", not "force on"

This is the part worth stating plainly, because it reads as a semantic change and is not one
we could avoid. The launcher **always** exports a literal `1` or `0` (ADR 0006 made stating
the answer part of the policy). So on the shipped path `SHIM_OVERLAY` is never unset, and if
`1` meant "force on", Steam's answer could never win for anyone using the launcher — which is
everyone. `1` therefore has to mean "no objection from our side".

The consequence is that there is no way to force the overlay ON for a title Steam says no to.
That is the correct trade for now: the direction users need is off-for-one-title, and the
inverse ("Steam says no, do it anyway") is a request to inject into a title whose owner
disabled the overlay — which for the motivating case is a title that then fails to launch.

## Alternatives rejected

**Parse `localconfig.vdf` at launch** (#92's route 2). More code, an sh VDF parser, a
per-user `userdata/<id>/` lookup we would have to resolve, and it answers with the last
flush rather than the live setting. It is strictly worse than a variable Steam already hands
us, and it was only the plan because the env route had not been checked.

**A native probe that asks `steamclient.dylib` per app** (#92's route 3). Heaviest route,
and unnecessary once route 1 landed.

**`ISteamUtils::IsOverlayEnabled`.** Already documented as the wrong oracle in
`overrides.json` and CONTEXT.md's loaded-vs-armed entry: it answers whether the renderer is
armed in *this* process, not what the user chose, and it can only be asked after we have
already injected — which is the decision being made.

**Tri-state `SHIM_OVERLAY` (`on`/`off`/`auto`).** The widening ADR 0006 actually sketched, and
it would preserve a force-on escape hatch. Rejected as more machinery than the ask needs:
`gen.py` rejects any default that is not `on`, the launcher and its stored preference both
write literal `0`/`1`, and nobody has wanted force-on. The veto is one field in the manifest
and no change to the value space. Tri-state remains available if a force-on case appears.

**Read the veto only in the launch script.** Smallest diff — the script resolves Steam's
answer and folds it into `shim_overlay_export` before any child sees it, leaving the
predicate pure. Rejected because it puts an opinion about the overlay rule back into a
caller, which is the precise failure ADR 0006 exists to prevent.

## Consequences

- Turning the overlay off for one title is now something the user does in the UI they already
  know, and it takes effect on the next launch of that game — not the next launch of Steam,
  which is the contract `SHIM_OVERLAY` still has.
- AoE IV launches. The library keeps its overlay.
- The stack now reads a variable it does not own. If Valve renames or retires
  `SteamNoOverlayUI`, the veto silently stops firing and the overlay comes back on for titles
  the user disabled it for — a failure that is quiet at exactly the wrong moment. It is one
  manifest field and one `instruments/` re-run to re-establish; the launch log naming the
  variable is what makes it diagnosable from a user's report.
- The launch log distinguishes the three reasons the overlay can be off (Steam's setting,
  `SHIM_OVERLAY=0`, no injector). Before this they were one message.
- `shim_overlay_enabled` is no longer a pure function of our own variable, so a caller
  holding its answer across an environment change is now wrong in one more way. Every current
  caller reads it once at launch.
