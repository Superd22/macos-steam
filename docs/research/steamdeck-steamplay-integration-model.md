---
status: current
---

# Steam Deck / Steam Play integration model: per-title controls and ProtonDB

Research for [#17](https://github.com/Superd22/macos-steam/issues/17). Decision this settles:
**if Steam Deck / desktop Steam has no per-title *opt-out* of the automatic compat
routing, our macOS analogue does not need to build one either.** Sources below are
Valve-owned (Proton repo, Steamworks docs, the Valve `steam-for-linux` tracker) except
where a claim is explicitly about a third party (ProtonDB).

## Bottom line (sub-question 5)

**Steam's own model has NO per-title opt-out of the automatic Proton routing.** The only
per-title control is an *opt-in to a specific compatibility tool* — the "Force the use of
a specific Steam Play compatibility tool" checkbox, whose dropdown lists only compatibility
tools (Proton versions / installed tools). For a Windows-only title there is no "run
natively" and no "None / don't use Proton" target, so there is nothing to opt out *to*.
Un-checking the box does not disable Proton; it just reverts the title to the **global**
default tool. Mirror this: a global default-tool setting plus an optional per-title
*force-a-specific-tool* override is the complete Steam model. **Do not build a per-title
"disable compatibility layer" switch — Steam doesn't have one.**

---

## 1. Steam Play settings model (global)

The Steam Play page under Settings is the global control surface. Two independent enables
plus a default-tool dropdown:

- **"Enable Steam Play for supported titles"** — routes titles Valve has whitelisted
  (native-Windows games Valve has validated) through the bundled Proton.
- **"Enable Steam Play for all other titles"** — the broad catch-all: run *every* other
  Windows title through the selected tool. This is the exact switch our macOS design
  mimics ("route all Windows-only titles through the CrossOver tool automatically").
- **"Run other titles with:" default-tool dropdown** — selects which compatibility tool is
  the global default for the catch-all bucket.

Primary source, Proton README (Valve): *"Steam ships with several versions of Proton,
which games will use by default or that you can select in Steam Settings' Steam Play
page."* and *"go to the Steam Play section of the Settings window. If the build was
correctly installed, you should see 'proton-localbuild' in the drop-down list of
compatibility tools."*
<https://github.com/ValveSoftware/Proton> (README, `master`).

Key structural fact for us: the dropdown is a list of **compatibility tools**. There is no
"native / none" entry in that list — it selects *which* tool, never *whether* to use one.

## 2. Per-title override — opt-in to a tool, not an opt-out

Per game: right-click → **Properties → Compatibility → "Force the use of a specific Steam
Play compatibility tool"** (checkbox), then pick a tool from the dropdown. This is
first-party Steam client UI, tracked in Valve's own `steam-for-linux` repo — e.g. issue
titles quoting the exact string *"Force the use of a specific Steam Play compatibility
tool"*:
<https://github.com/ValveSoftware/steam-for-linux/issues/9272>,
<https://github.com/ValveSoftware/steam-for-linux/issues/7606>.

What it does and does not do:
- **Checking it = opt-in to a *specific* tool** for that one title (e.g. force Proton
  Experimental instead of the global default). The dropdown, again, only contains
  compatibility tools.
- **Un-checking it does NOT opt the title out of Proton.** It reverts the title to the
  global default from §1. For a Windows-only title with "all other titles" enabled, the
  default *is* Proton — so the game still runs through Proton.
- There is **no per-title target that means "do not use a compatibility tool"** for a
  Windows-only game, because there is no native build to fall back to. "Forcing it off"
  is not an available operation for a Windows-only title; the only off-switch is the
  *global* "all other titles" enable.

(Per-title runtime *tuning* — distinct from tool selection — is done via
`Properties → Set Launch Options`, e.g. `PROTON_USE_WINED3D=1 %command%`. Still not an
opt-out; it configures Proton, it doesn't bypass it. Proton README, Runtime Config
Options: <https://github.com/ValveSoftware/Proton>.)

## 3. Steam Deck specifics

Same per-game **Properties → Compatibility** controls exist in the Deck client (the Deck
runs the same Steam client / SteamOS Steam Play stack). The Deck-specific concept is the
**Deck Verified badge** (Verified / Playable / Unsupported / Unknown), which is a
*separate, purely informational* layer — it is **not** a behaviour or routing control.

Valve, Steamworks "Steam Deck and Steam Machine Compatibility Review":
- Verified — *"Your game passes all compatibility checks. No configuration work is
  required…"*
- Playable — *"Your game functions on Deck/Machine, but may require manual work from the
  user."*
- Unsupported — *"Your game does not function on this device due to incompatibility with
  Proton or specific hardware components."*
- Unknown — has not completed review.

Crucially, the rating changes presentation only, never availability or behaviour:
*"The results of a compatibility review will not affect whether your game is available to
customers on Deck or Machine, but will affect how it is presented."* Unsupported does
**not** block install or launch.
<https://partner.steamgames.com/doc/steamhardware/compat>

Takeaway for us: a compatibility *rating/badge* and the compatibility *routing* are
orthogonal in Valve's model. The badge never gates behaviour, so we don't owe one either
(and certainly not as a control).

## 4. ProtonDB — third-party, not in-client

**ProtonDB is an independent community site, not a Valve/first-party integration.** It is
crowd-sourced compatibility reports (rating scale Borked → Bronze → Silver → Gold →
Platinum), run by the community (project of @bdefore), *no affiliation with Valve*. It is
not surfaced inside the Steam client. <https://www.protondb.com/>

The only compatibility rating the **Steam client itself** surfaces is Valve's own **Deck
Verified** status (§3) — Valve-native, and informational only. So: nothing ProtonDB-like
is built into Steam; the native in-client rating is Deck Verified, and even that doesn't
alter routing.

---

## Design implications for the macOS analogue

1. Build the **global** pair: "route supported titles" + "route all other titles through
   the CrossOver tool," plus a global default-tool selection. This is the Steam Play page.
2. Build a per-title **force-a-specific-tool** override (opt-in to a particular
   CrossOver/compat build). Optional/nice-to-have, mirrors Steam exactly.
3. **Do NOT build a per-title "disable the compatibility layer / opt out of routing"
   switch** — Steam has no such control for Windows-only titles. The only off-switch is the
   global enable. This is the decision issue #17 asked us to settle.
4. No ProtonDB-style integration is required for parity; the only Valve-native rating is
   Deck Verified, which is informational and does not affect routing.
