---
status: historical
superseded-by: the whole of `docs/research/`, `docs/adr/` and `CONTEXT.md`
---

> [!NOTE]
> **Historical — the opening brief, 2026-08-03. Nothing shipping depends on it.**
> This is where the project started: a survey of the landscape and a list of open
> questions, written before any of them had been measured. It says so itself below —
> "not a finished design".
>
> Every question it raises has since been answered, and several of its guesses were
> wrong — notably §3's "cross-process overlay rendering relies on Linux/X11-specific
> injection and compositing tricks that don't map to macOS", which
> `overlay-injection.md` disproves outright, and §7 step 5's scoping-out of the overlay
> on that basis. Read it for the shape of the original problem, not for its answers;
> the answers live in `INDEX.md`'s `current` rows and in `docs/adr/`.

# macOS "Steam Play" — Research Brief

## Goal

Today, running a Windows-only Steam game on macOS means keeping two separate
Steam clients: the native macOS Steam app (for macOS-native games) and a
Windows copy of Steam running inside CrossOver/Wine (for Windows-only games).
This is annoying to use: two libraries, two windows, manual switching.

The target end state: open the native macOS Steam app, see **one** library
containing both macOS and Windows games. Clicking Install or Play on a
Windows-only game "just works": it transparently launches through
CrossOver/Wine (or another user-chosen backend) without the user having to
open, see, or think about a second Steam client. This is the same experience
Valve already ships on Linux via **Steam Play / Proton**, just ported
conceptually to macOS.

This document is a summary of research into whether/how this is achievable,
intended as a brief for a coding agent or engineer picking up the project.
It is **not** a finished design. Several things below are open questions
that need empirical verification against the current Steam client, since
Valve does not document any of this publicly and behavior has changed
(and regressed) over time.

---

## 1. The landscape of existing tools (context, not the project itself)

| Tool | What it is | Status |
|---|---|---|
| **CrossOver** (CodeWeavers) | Commercial Wine wrapper for macOS. Runs Windows apps/games, incl. a full Windows Steam client, in an isolated "bottle." Actively developed, paid. | Works today; is one of the compatibility layers we'd build on top of. |
| **Apple Game Porting Toolkit (GPTK)** | Apple's own dev tool: Wine + D3DMetal, translating DirectX 11/12 → Metal on Apple Silicon. Basis for both CrossOver's and Whisky's Windows-game performance. GPTK 4 beta (mid-2026) showed large perf gains (e.g. GTA V +66% fps on M4 Pro). | Actively updated by Apple. **License explicitly forbids combining GPTK with Wine/Proton/CrossOver-style community tooling** — important constraint, see §6. |
| **Whisky** | Free, open-source GUI wrapper around Wine + GPTK. Was the free alternative to CrossOver. | Original developer stopped active development (~2024/2025), publicly recommended CrossOver instead. Existing installs still run; no updates/support. |
| **Kegworks** (aka "Sikarugir") | Community fork continuing where Whisky left off. | Free, actively tinkered with, less polished than CrossOver. |
| **Parallels** | Full Windows-on-ARM VM (not a translation layer — actual Windows). Runs anything, but with overhead and subscription cost. | Fallback for games CrossOver/GPTK can't handle; not the target for this project. |

**Bottom line:** any of CrossOver, vanilla Wine+GPTK, or Kegworks can serve
as an interchangeable "backend": the design goal (see §3) is a small
front-end that lets the user pick which one to use per game/bottle. This
project is about the *Steam integration layer*, not about replacing
CrossOver/Wine itself, and doesn't require modifying any of these backends'
internals; they're driven from the outside, via whatever CLI/automation
interface each already exposes.

---

## 2. Prior art: `kaon`

Repo: https://github.com/natbro/kaon

> "Tools, and instructions for more easily installing and launching Windows
> games via Wine or CrossOver directly in the macOS Steam client."

This is the closest existing attempt at the unified-library goal, and is a
good starting reference implementation / cautionary tale about the rough
edges.

### How kaon works

1. **Library merge**: Forces the native macOS Steam client to treat Windows
   as a valid/playable platform (via a `@sSteamCmdForcePlatformType windows`
   directive in `steam_dev.cfg`), and connects a CrossOver Windows Steam
   install as a second library folder.
2. **Launch interception**: When you click Play on a Windows-only game in
   native macOS Steam, a wrapper script intercepts the launch, silently
   starts a hidden/background Windows Steam client inside the CrossOver
   bottle if needed (for ownership/achievement validation), and launches the
   actual game `.exe` through it.
3. Ships an embedded Proton `lsteamclient` subtree, apparently for potential
   future work toward a more native integration (see §4).

### Known limitations / unfinished parts (as of research)

**Library / platform-forcing issues:**
- `@sSteamCmdForcePlatformType windows` is blunt: it forces *all* games to
  Windows mode, with no per-game "prefer native if available" logic (unlike
  real Steam Play on Linux, which does this intelligently).
- The presence of `steam_dev.cfg` tends to block Steam's own client
  self-update process on macOS. It requires periodically deleting the file,
  letting Steam update, then restoring it.
- Games with both macOS and Windows depots will end up permanently
  Windows-only once forced; no clean way to keep them switchable.
- Merging in a second library folder (to point at the CrossOver/Windows
  Steam library) is fragile: Steam performs some internal
  validation/checksumming on `libraryfolders.vdf` and wipes out entries it
  doesn't recognize as self-authored. Current workaround involves creating a
  dummy disk image, which the author themselves calls "annoying."

**Install/Play automation issues:**
- No automatic detection/setup for new games: for every single title, a
  user must manually open a GUI tool (`steammetadataeditor`) and hand-enter
  custom launch options pointing at the wrapper script.
- That GUI tool requires Python 3.11 specifically; broken against the
  Python 3.13 that current Homebrew installs by default.
- Launch options have to be smuggled in via the launch *arguments* field
  rather than the executable field, because of how Steam quotes/mangles
  paths in the executable field (a workaround for a quirk, not a clean
  hook).
- Author's own stated "next step": build an easier/CLI version of the
  metadata editing to remove the manual GUI step. Not done yet.

**Important note on downloading/installing:** Steam's content-delivery
system (SteamPipe) doesn't care what OS a depot's files target; it just
downloads whatever depot is assigned for the platform the client thinks it
should fetch, and drops the files in the normal install folder, exactly like
any native game. That means the *download* itself is not hard and doesn't
need new code: the native client already does this correctly once it's
convinced the Windows depot is valid to install (the `steam_dev.cfg` force
flag, or ideally a real per-app mechanism; see §5's `compatibilitytool.vdf`
angle). kaon's "install flow isn't solved" rough edge is really about the
fragile platform-forcing/library-merge step blocking that decision, not
about needing to build a custom downloader.

### An open, promising lead: `kaon` issue #6, controlling Steam via its CEF debugger

Issue: https://github.com/natbro/kaon/issues/6 ("Found a way to control Steam")

A user reports that launching Steam with `-cef-enable-debugging` opens a
Chromium DevTools-compatible debugging interface (`http://localhost:8080`,
also reachable via websockets) *on macOS*, and that this makes "Steam
completely controllable and accessible from a discrete app" — including,
per their report, being able to trigger downloading of Windows games without
disturbing already-installed macOS games. This deserves its own section
because it turns out to connect to a much bigger, well-established body of
prior art (see §4).

### Related, more speculative project

Repo: https://github.com/Toast-dev-wq/Proton-mac-client-

> "a mac Compatibility tool for Steam Play based on Wine and additional
> components"

Essentially a copy of Proton's build scaffolding with "mac" in the name.
1 star, no releases, no real macOS-specific implementation found. Signals
that this is a known idea in the community, but nobody has gotten past
scaffolding.

---

## 3. Two architectural levels of "integration"

It's useful to separate the problem into levels of ambition, because they
require very different amounts of work. A third avenue (§4, the CEF/DevTools
control surface) may end up strengthening or replacing parts of Level A
rather than being a separate tier of its own (see the phased approach in
§7).

### Level A: Polished kaon (external orchestration)

The design shape for this level, concretely: a small config UI where the
user picks a backend ("emulator": CrossOver, vanilla Wine+GPTK, or Kegworks)
and, optionally, which bottle/prefix to use. A router component then
intercepts Play/Install for non-macOS games in the native Steam client and
shells out to whichever backend was chosen, passing it the game executable
path. **This does not require touching Wine/CrossOver's internals at all**:
each backend is treated as a black box, driven via its existing CLI/
automation conventions (CrossOver's own bottle-launch commands, or a plain
`WINEPREFIX=... wine game.exe` for vanilla Wine-based tools).

Concretely, fixing kaon's rough edges without changing the fundamental
approach means:
- Smarter, per-game platform preference instead of the global force-Windows
  flag (may require intercepting/patching how Steam decides platform
  compatibility per-app, since there's no documented per-game override,
  though see §4, which may offer a cleaner lever for this).
- A more robust way to merge in the CrossOver library that survives Steam's
  validation/checksum behavior (need to find what Steam actually accepts;
  might mean writing entries the way Steam itself would, or finding an
  undocumented supported mechanism).
- Automatic detection of newly-installed/owned Windows games + automatic
  generation of the wrapper launch options (replacing the manual
  `steammetadataeditor` step), effectively a background watcher/daemon.
- Fix/replace the Python 3.11-dependent tooling.
- A real Install flow, not just Play (kaon's install path is one of the
  unsolved rough edges), though per §2's note, the download itself is
  already handled by Steam once platform-forcing is solved; the missing
  piece is that trigger, not a custom downloader.

This still keeps a hidden Windows Steam client running in the background for
Steamworks calls (achievements, cloud saves, friends). It is **not** a true
Proton-equivalent, but gets close to "invisible to the user" if built out
properly. This is likely the highest-value-per-effort starting point.

### Level B: True `lsteamclient` equivalent (no hidden Windows Steam at all)

On Linux, `lsteamclient` is a Wine-side shim: instead of loading the real
Windows `steamclient.dll`, Wine loads this shim, which marshals Steamworks
API calls (achievements, overlay, friends, cloud saves) across to the
*native Linux* `libsteam_api.so` / running native Steam client, via Wine's
`__wine_unix_call()` mechanism. No second (hidden) Steam client process is
ever needed; there's just one real Steam client, native to the host OS,
answering everything.

**Important clarification on what this does and doesn't require:** this
approach does *not* mean installing any part of Windows Steam inside the
Wine/CrossOver environment. Quite the opposite. The whole point is to
*replace* the files a Windows game would normally use to find Steam
(`steamclient.dll`, etc.) with a custom shim, so no real Windows Steam
software is ever present in the bottle. The game still needs to find
*something* at the location/registry entry it expects, so a thin stub
(an install-path marker and possibly an OS-level named object some games'
DRM checks for) is needed, but it contains only the custom shim, not any of
Valve's real Windows binaries.

**Why this doesn't trivially port to macOS:**
- There is no macOS-native `libsteam_api`-equivalent library that Valve
  ships in a form this shim could redirect calls to. The whole trick
  depends on that native counterpart already existing (it does on Linux; it
  doesn't in the form needed on macOS).
- The actual mechanism the Linux shim uses to reach the running native Steam
  client is an **undocumented local IPC channel** inside the real Steam
  client itself, reverse-engineered by the Proton team over years, not
  published by Valve. Nobody has done the equivalent reverse-engineering
  for the macOS Steam client's internals. (§4's CEF/DevTools finding may
  turn out to be a more tractable way into this than raw binary reverse
  engineering; worth investigating before assuming a from-scratch IPC
  reverse-engineering effort is required.)
- Steam's macOS client is old/neglected relative to Linux/Windows: still an
  Intel binary in large part (per community reports), heavy use of CEF for
  UI, roughly two-thirds of the catalog incompatible for other reasons. This
  is a smaller, less-tooled target to reverse-engineer than the actively
  maintained Linux client.
- Cross-process overlay rendering (Shift+Tab overlay, screenshot/video
  capture, controller input redirection) relies on Linux/X11-specific
  injection and compositing tricks that don't map to macOS's very different
  windowing/injection model (and macOS is considerably more locked-down
  about code injection into other processes).

**Building this would require:** figuring out whether the macOS Steam client
has any of the relevant IPC/dispatch code already present (possibly latent,
shared from the same Valve codebase, just unused; worth checking before
assuming it needs to be built from scratch), and if not, doing genuine
reverse-engineering work plus building a Wine-loadable macOS shim.

**On implementation language:** Rust is a solid candidate for the shim DLL
itself. It cross-compiles cleanly to a Windows `cdylib` that Wine loads like
any other DLL, no need to touch Wine's source tree. The one wrinkle is that
Steam's API is C++ with virtual interfaces (vtables), not a flat C ABI; this
is handled by hand-building `#[repr(C)]` structs of function pointers
matching the expected layout, which is a well-worn (if fiddly) FFI pattern,
not a blocker. Extending Valve's actual `lsteamclient` code generator
(Python + C, tightly wired into Wine's own build system) is a separate
matter: not worth porting to Rust; if that pipeline is reused/forked, stick
to what it's already written in.

This is a much larger, more open-ended undertaking than Level A, with real
uncertainty about feasibility until someone actually inspects the binary.
Skipping game overlay entirely for a v1 is a reasonable scope cut regardless
of which level is pursued.

---

## 4. Steam's CEF/DevTools remote-debugging control surface

This is a distinct, and possibly very high-leverage, avenue uncovered via
`kaon` issue #6 (§2). It deserves its own section because it connects to a
mature, well-documented (if not-for-macOS-documented) body of prior art from
the Steam Deck modding community, and could plausibly replace some of Level
A's most fragile hacks (config-file editing, checksum-fighting) with a live,
programmatic interface.

### What it is

Steam's UI, on every platform, is built on **Chromium Embedded Framework
(CEF)** rather than a native toolkit. Steam has a flag/marker
(`-cef-enable-debugging` launch argument, or a `.cef-enable-remote-debugging`
file) that turns on the standard **Chrome DevTools Protocol**, the same
remote-debugging interface used to debug ordinary websites, pointed at
Steam's own UI process, by default at `http://localhost:8080` (`8081` on
Steam Deck).

Once connected, the debugging interface exposes several "targets" (pages),
including one called **`SharedJSContext`**, a tab with no visible UI where
most of Steam's internal JavaScript runs. Inside that context there is a
global object, **`window.SteamClient`**, which is Steam's actual internal
client API.

### How mature this is, and the gap

This is **not** an obscure one-off trick. It's the literal foundation of an
active modding ecosystem:

- **Millennium** (https://github.com/SteamClientHomebrew/Millennium) — an
  actively maintained (4.1k★, 178 releases as of mid-2026) "modding
  framework for creating and managing Steam Client themes and plugins,"
  explicitly built on this mechanism, advertising "full control over Steam"
  for plugins.
- **Decky Loader** / **CSS Loader** — equivalent tooling for Steam Deck,
  with public docs describing the same `SharedJSContext` /
  `window.SteamClient` mechanism.
  → https://wiki.deckbrew.xyz/plugin-dev/cef-debugging
  → https://docs.deckthemes.com/CSSLoader/Cef_Debugger/

**The gap:** every one of these projects' documentation explicitly scopes
itself to **Windows and Linux/Steam Deck only**; macOS is not mentioned
anywhere in their docs as a supported target. The `kaon` issue #6 report
appears to be the first documented instance of this flag/port working on
the *macOS* Steam client specifically. That's a good sign (it fits with
everything else pointing at a shared Valve codebase across platforms), but
it means:
- There is no existing reference for what `SteamClient.*` methods are
  actually present/functional on the macOS build.
- Nobody in the mature Windows/Linux modding community has verified or
  documented macOS behavior: this is unexplored territory, not a
  known-working recipe waiting to be copied.

### Why this matters for the project

If `SteamClient`'s JS API exposes (or can be coaxed into exposing) hooks
around per-app platform selection, install state, or launch behavior, it
could offer a **live, programmatic alternative** to kaon's most fragile
mechanisms: editing `steam_dev.cfg` and fighting `libraryfolders.vdf`
checksums. Driving the client through its own internal API, the way
Millennium drives theme changes, is architecturally more robust than hoping
a text file survives Steam's next validation pass. This is arguably the
single most promising unexplored lead from this research, and is now a
first-class candidate for the phased approach in §7 (see step 0).

### Security note

Every source describing this mechanism (Decky's docs explicitly) warns that
the debug port is **unauthenticated on the local network by default**:
anyone on the same network segment can get full access to the debugger and
therefore full control of the Steam client while it's enabled. Any tool
built on this needs to bind to localhost only and not leave the debug flag
set outside of active development/use.

---

## 5. The `compatibilitytool.vdf` mechanism: what's known

This is the actual plumbing Valve uses on Linux/SteamOS to make Steam Play
work as a first-class client feature (not a hack), i.e. what "real Steam
Play" would look like if Valve built it for macOS themselves. Understanding
it matters because **there is evidence this mechanism partially exists,
unofficially, inside the macOS client already**, which changes the shape of
the project from "invent this" to "find, stabilize, and extend what's
already there but broken/regressed."

### Format (best available public knowledge, not officially documented by Valve)

A compatibility tool = a folder inside a `compatibilitytools.d` directory,
containing:

- **`toolmanifest.vdf`** — tells Steam *how to run* the tool: a command-line
  template (with placeholders like `%verb%` for the action Steam is
  performing) that Steam invokes instead of running the game directly.
- **`compatibilitytool.vdf`** — tells Steam *what the tool is*: an internal
  name, `display_name`, `install_path`, and crucially `from_oslist` /
  `to_oslist`, declaring "games built for platform X" → "runnable on host
  platform Y."

Example (Linux, no-op tool), for reference on shape/syntax:
```
"compatibilitytools"
{
  "compat_tools"
  {
    "Steam-Play-None"
    {
      "install_path" "."
      "display_name" "Steam-Play-None"
      "from_oslist"  "linux"
      "to_oslist"    "linux"
    }
  }
}
```
Proton itself declares roughly `from_oslist "windows"` /
`to_oslist "linux"`, i.e. "takes Windows-built games, runs them on a Linux
host." **The macOS-equivalent declaration for our use case would be
`from_oslist "windows"` / `to_oslist "macos"`**: take Windows games, run
them on a Mac host. This is the direction that matters for us.

No comprehensive official field reference exists publicly. The closest
community documentation (SteamTinkerLaunch's wiki) is itself incomplete.
Most real knowledge comes from reverse-engineering by the authors of
Boxtron, Luxtorpeda, and Proton-GE, all Linux-focused.

### Evidence that `to_oslist "macos"` is a real, recognized value in the client

Two independent, directly relevant findings:

1. A Steam Community bug report (Steam Client Beta forum, Nov 2024): a user
   had a **self-made compatibility tool with `to_oslist "macos"` actually
   working** on macOS (using Wine to launch DRM-free games), placed at
   `/usr/local/share/steam/compatibilitytools.d`. They report that after a
   client update, **Steam stopped discovering tools in that folder
   entirely**: a regression, unaddressed, zero Valve responses in the
   thread.
   → https://steamcommunity.com/groups/SteamClientBeta/discussions/0/4630358592048891288/

2. A Valve bug tracker feature request (`ValveSoftware/steam-for-linux`
   issue #12612) about Steam Cloud save-path resolution failing for
   compatibility tools using `from_oslist "macos"`. **Important nuance**:
   on closer reading, this specific report is about the *reverse* scenario
   from ours: wrapping a **macOS-native game build** to run it on a
   *Linux* host (their example: DELTARUNE's Mac build, via Steam-for-Linux),
   not a Windows game running on a Mac host via CrossOver. The failure is
   that Steam's Cloud sync code has no path-remapping rule for the
   `MacAppSupport` root type; it only knows how to remap `Win*` roots (into
   the compat-data prefix folder), which is exactly what Proton relies on.
   → https://github.com/ValveSoftware/steam-for-linux/issues/12612

   **Implication for us**: our actual target scenario (Windows game →
   CrossOver/Wine → macOS host) would declare `from_oslist "windows"`, which
   is the *already-proven-working* remap path (same code Proton uses on
   Linux for Windows→Linux). The broken/unhandled case in issue #12612 is a
   different, narrower one (Mac-native build → other host) that likely
   doesn't affect us. **This needs to be verified empirically, not assumed**:
   it's an inference from reading the client's apparent logic, not a
   confirmed fact.

### Net "state of the art"

- The hook Valve would need for real macOS Steam Play appears to already
  exist in some form in the shipped client (shared codebase with
  Linux/SteamOS), evidenced by real (if old, if regressed) working examples.
- It is unofficial, undocumented, not maintained, and has at least one
  known regression (compat tool discovery on macOS silently stopped working
  at some point before Nov 2024) with zero indication Valve is tracking or
  fixing it.
- Nobody has published a current (2025/2026), verified writeup of "here's
  how to register a working compatibility tool on today's macOS Steam
  client." One of the very first practical research steps for this project
  is empirical: build a minimal `compatibilitytool.vdf` + `toolmanifest.vdf`
  pair (`from_oslist "windows"`, `to_oslist "macos"`), place it in
  `compatibilitytools.d`, and see whether **today's** Steam client even
  discovers it. Much of the downstream design depends on that answer; see
  §7's phased approach for how this relates to the §4 CEF/DevTools lead.

---

## 6. Constraints and risks

- **Apple's GPTK license explicitly forbids combining it with
  Wine/Proton/CrossOver-style community tooling.** A from-scratch macOS
  Steam Play implementation cannot legally build on top of Apple's official
  GPTK if it also touches community Wine/Proton code; it would need to use
  independent community Wine builds (the kind CrossOver/Kegworks are built
  on), which tend to lag GPTK in performance/compatibility. This is a real
  constraint on Level B in particular.
- **This all rests on undocumented, closed, third-party (Valve) behavior.**
  Anything discovered by reverse-engineering or by exploiting a
  latent/shared code path (this applies to §4's CEF/DevTools surface just as
  much as §5's compatibility-tool mechanism) can be silently removed or
  changed in a future Steam client update, with no changelog entry and no
  one to file a bug against with any expectation of a fix (see the Nov 2024
  regression, still open, no Valve response).
- **The CEF/DevTools debug port is unauthenticated on the local network by
  default** (§4). Any tool built on it must bind to localhost only and avoid
  leaving it enabled outside active use. This is a real, immediate security
  consideration, not just a theoretical one, since the modding community's
  own docs flag it explicitly.
- **Binary patching / runtime injection risk (relevant mainly to Level B).**
  If the needed hook doesn't already exist and has to be added by patching
  the running Steam app, this needs a patch mechanism that survives Steam
  auto-updating itself (a static edit gets wiped on update; likely need a
  library-injection approach that re-applies at every launch instead).
  This is a meaningfully higher-risk category of hack than anything in
  Level A, and closer to "modifying someone else's closed application" than
  "scripting around it."
- **Steam Cloud save sync** may or may not actually be broken for our exact
  use case; see §5's nuance on `from_oslist`/`to_oslist` direction. Needs
  empirical testing before assuming it's a blocker. If Steam Cloud sync
  genuinely doesn't work for this configuration, the pragmatic fallback is
  to **not rely on Steam Cloud at all**: symlink the game's actual save
  location (inside the Wine/CrossOver bottle, typically under a fake
  `AppData` folder) to a normal folder on the Mac filesystem, and let a
  general-purpose sync tool (iCloud Drive, Dropbox, etc.) handle
  cross-device continuity instead. This does *not* work as a fix for a
  Cloud-sync code-path failure (that fails before touching any file, so
  there's nothing on disk to trick), but it fully solves the user-facing
  goal of "my save follows me between machines" via a different mechanism.

---

## 7. Suggested phased approach

0. **New, cheap, high-leverage first experiment (from §4): probe the
   CEF/DevTools control surface on macOS.** Launch macOS Steam with
   `-cef-enable-debugging` (or drop the `.cef-enable-remote-debugging`
   marker file), connect to `localhost:8080` with a Chrome DevTools client,
   find the `SharedJSContext` target, and poke at `window.SteamClient` to
   see what's actually there on the macOS build. This is fast to try, low
   risk, and its outcome should inform whether steps 1–2 below lean on this
   interface instead of (or alongside) config-file hacking.
1. **Verify the compatibility-tool hook still exists on the current Steam
   client.** Build the minimal `compatibilitytool.vdf`/`toolmanifest.vdf`
   pair described in §5, targeting `from_oslist "windows"` /
   `to_oslist "macos"`, and test discovery on today's macOS Steam client.
   This experiment determines whether the whole project is "extend an
   existing (if broken) mechanism" or "there's truly nothing there anymore,
   fall back to kaon-style external orchestration only."
2. **In parallel / regardless of (0)/(1)'s outcome**, harden kaon's Level-A
   approach: build the config-UI + backend-picker + router design described
   in §3, replace the global platform-force with something safer (ideally
   informed by whatever step 0 found), automate the metadata/launch-option
   setup (kill the manual GUI step), find a library-merge method that
   survives Steam's validation/checksum behavior, and build out the missing
   Install flow. This is valuable on its own even if Level B / native
   compat-tool support never pans out.
3. **If (1) shows the hook works:** pursue registering as a real
   compatibility tool as the primary integration path instead of the
   external-wrapper-script approach. This is architecturally closer to
   "real Steam Play" and would likely resolve the Install-flow problem too
   (Steam would drive install placement itself, the way it does for
   Proton).
4. **Only after (2)/(3) are solid**, consider Level B (`lsteamclient`
   equivalent) as a stretch goal, starting with an audit of whether the
   macOS Steam client binary already contains latent
   IPC/dispatch-to-native-client code (shared codebase with Linux) before
   assuming it needs to be built from nothing, and factoring in whatever
   step 0 revealed about the CEF/DevTools surface as a possible alternate
   route into the client's internals.
5. **Explicitly scope out Steam overlay / in-game injection features** for
   an initial version, across all levels. No known feasible path for this
   was found, and it adds a lot of platform-specific risk for a
   nice-to-have.

---

## 8. Source list

- kaon (primary prior art): https://github.com/natbro/kaon
- kaon issue #6, "Found a way to control Steam" (CEF/DevTools lead): https://github.com/natbro/kaon/issues/6
- Millennium (Steam client modding framework built on the CEF debug surface): https://github.com/SteamClientHomebrew/Millennium
- Millennium docs (Steam client environment / CEF architecture): https://docs.steambrew.app/developers/environment
- Millennium issue #591 (`.cef-enable-remote-debugging` file behavior): https://github.com/SteamClientHomebrew/Millennium/issues/591
- Decky Loader (frontend/CEF debugging docs: SharedJSContext, `window.SteamClient`): https://wiki.deckbrew.xyz/plugin-dev/cef-debugging
- CSS Loader (CEF Debugger docs): https://docs.deckthemes.com/CSSLoader/Cef_Debugger/
- Steam client parameters/console commands reference (gist, incl. `-cef-enable-debugging`): https://gist.github.com/davispuh/6600880
- Proton-mac-client (abandoned scaffold attempt): https://github.com/Toast-dev-wq/Proton-mac-client-
- Steam Play compatibility tool discovery regression on macOS (Nov 2024, unanswered): https://steamcommunity.com/groups/SteamClientBeta/discussions/0/4630358592048891288/
- Steam Cloud save-path remap feature request (`from_oslist macos`, Valve issue tracker): https://github.com/ValveSoftware/steam-for-linux/issues/12612
- `compatibilitytool.vdf` example / shape reference: https://github.com/Scrumplex/Steam-Play-None/blob/main/compatibilitytool.vdf
- Compatibility tool manifest community documentation (incomplete): https://github.com/sonic2kk/steamtinkerlaunch/wiki/Steam-Compatibility-Tool
- Whisky development ending, CrossOver recommended instead: https://appleinsider.com/articles/25/04/16/whisky-development-ends-on-macos-to-help-wine-flourish
- Apple Game Porting Toolkit 4 beta coverage: https://appleworld.today/2026/07/apples-game-porting-toolkit-4-beta-makes-the-mac-a-more-viable-gaming-platform/
- Steam macOS client neglect / Intel binary / CEF UI discussion (Hacker News): https://news.ycombinator.com/item?id=37313094
- Whisky vs CrossOver vs Parallels overview: https://blendlogic.com/posts/whisky-vs-crossover-vs-parallels.html

---

## 9. Open questions for whoever picks this up

- What does `window.SteamClient` actually expose on the *macOS* Steam
  client, concretely? (Untested as of this writeup; see §7 step 0. No
  existing documentation covers macOS at all for this API.)
- Can the CEF/DevTools surface (§4) be used to drive per-app platform
  selection or install/launch behavior directly, sidestepping the
  config-file fragility described in §2/§3 entirely?
- Does `compatibilitytools.d` discovery work at all on the *current*
  macOS Steam client version? (Untested as of this writeup; see §7 step 1.)
- If it works, does Steam Cloud sync actually succeed for a
  `from_oslist "windows"` / `to_oslist "macos"` tool, or does it hit the
  same/a related unresolved-path failure as issue #12612? (The reasoning in
  §5 suggests it might not be affected, but this is inference, not a tested
  fact.)
- Does the macOS Steam client binary contain any latent/shared code from the
  Linux client related to native Steamworks IPC dispatch (the thing
  `lsteamclient` talks to on Linux), or is that code path entirely absent on
  macOS? Nobody has published an answer to this. The CEF/DevTools surface
  may offer an easier way to probe this than static binary analysis.
- What exactly changed in the Steam client update that caused the Nov 2024
  compatibility-tool-discovery regression reported on macOS? No changelog
  or explanation found.
