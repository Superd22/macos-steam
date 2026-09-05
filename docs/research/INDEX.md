# `docs/research/`: what is still true

Every document here carries a `status:` block at its top. This page is the same information in
one table, so nothing has to be opened to find out whether it can be trusted.

**Read the status before the document.** These are dated measurements against a moving target: a
Steam client that updates itself, a CrossOver that has already moved a major version during this
project's life, and a Proton tree that is somebody else's branch.

## The three statuses

| | meaning |
|---|---|
| `current` | The conclusions hold. Code in `src/` and decisions in `docs/adr/` rely on them. |
| `superseded` | Another document replaced its verdict. `superseded-by:` names it. A superseded doc may still be the only source for some of its material — `still-cited-for:` says which part. |
| `historical` | Kept for the record. Nothing depends on it, and it is not a place to look for an answer. |

`re-verify-on:` names the event that would invalidate a `current` document: a Steam client
update, a CrossOver upgrade, a Proton bump. It is a warning, not an expiry. The doc is accurate
until that event happens; stale claims are the ones that survive it unchecked.

## The docs

| doc | status | |
|---|---|---|
| [`app-mappings-self-contained.md`](app-mappings-self-contained.md) | `current` | A compat tool's own `compatibilitytool.vdf` can map itself to titles, and `appid 0` is a global catch-all. Re-verify on a **Steam client update**. |
| [`clean-bottle-provenance.md`](clean-bottle-provenance.md) | `current` | Proving it is our shim that answered, not a stray Valve DLL. Re-verify on a **CrossOver upgrade**. |
| [`compat-vdf-platform-override.md`](compat-vdf-platform-override.md) | `current` | The compat gate, the one instruction that flips it, and Valve's launch environment contract. Cited from `src/compat-enabler/enabler.c`. Re-verify on a **Steam client update** — the addresses are offsets in one build. |
| [`compatibilitytools-d-macos.md`](compatibilitytools-d-macos.md) | `current` | Local tool discovery works, from the Steam **install** dir. Carries its own wrong turn (the data dir) at the bottom. Re-verify on a **Steam client update**. |
| [`compatibilitytools-d-macos-CORRECTED.md`](compatibilitytools-d-macos-CORRECTED.md) | `superseded` | Merged into `compatibilitytools-d-macos.md`. A stub, kept because issues #5 and #9 link to it by name. |
| [`crossover-bridge-surface.md`](crossover-bridge-surface.md) | `current` | What this machine's CrossOver exposes, and the viable transports into a bottle. **Behind the tested matrix**: pinned to 25.1.1 / `wine-10.0-8474` while the project also exercises 26.2 / `wine-11.0-8723`. Re-verify on a **CrossOver upgrade**. |
| [`lsteamclient-mechanics.md`](lsteamclient-mechanics.md) | `current` | How Proton's `lsteamclient` works, and the minimum subset that ports. Upstream of the vtable generator. Re-verify on a **Proton bump** — pinned to `proton_11.0` @ `0745bfbc4cf4`. |
| [`macos-steamplay-chain.md`](macos-steamplay-chain.md) | `current` | Where the chain from *tool registered* to *Steam installs and launches a Windows depot* breaks. |
| [`macossteamplayresearch.md`](macossteamplayresearch.md) | `historical` | The opening brief, 2026-08-03. Superseded in full by this directory, `docs/adr/` and `CONTEXT.md`. Several of its guesses were wrong. |
| [`overlay-injection.md`](overlay-injection.md) | `current` | What loads, when, to get Valve's overlay renderer drawing over a Windows title. The evidence behind ADR 0003 and `src/overlay-inject/`. Re-verify on a **CrossOver upgrade** — the deadline is a property of the Wine loader. |
| [`drm-overlay-late-arming.md`](drm-overlay-late-arming.md) | `current` | A DRM-wrapped title's overlay **is** recoverable: the renderer's Metal hooks come from a swizzled `-[NSApplication init]`, so the "deadline" is a missed call, not a platform limit — calling the installer by hand arms it late, in-process, with no import rewrite. Ranks that against a display-driver shim, an ntdll preload, and the dead `AppInit_DLLs` route. Re-verify on a **Steam client update** (the swizzle) or a **CrossOver upgrade**. |
| [`steam-drm-shared-memory.md`](steam-drm-shared-memory.md) | `current` | Steam **DRM** requires `steamclient64.dll` to carry a **Valve RSA-1024 signature** (`"VLV\0"` at DOS offset `0x40`), verified against keys hard-coded in the title — so DRM-wrapped titles are out of reach by a private key, not by unfinished work. Includes the `Local\SteamStart_SharedMem*` protocol (an error-reporting channel, not a check), the `.bind` disassembly, and every candidate built and eliminated. Why such a title fails *exactly like* a `#45` coverage gap without being one. Re-verify on a **Steam client update** or a **title update**. |
| [`steam-overlay-feasibility.md`](steam-overlay-feasibility.md) | `superseded` | By `overlay-injection.md`, for its verdict. Still the only source for the **binary-level anatomy** of Valve's macOS overlay (§1–§4) and the Proton/Valve comparison (§6). |
| [`steamclient-js-api-macos.md`](steamclient-js-api-macos.md) | `current` | Full `window.SteamClient` inventory for the macOS client. Re-verify on a **Steam client update** — it is a snapshot of one build's JS surface. |
| [`steamdeck-steamplay-integration-model.md`](steamdeck-steamplay-integration-model.md) | `current` | Steam's own per-title model, and why there is no per-title opt-out to mirror. |
| [`steamworks-vtable-tables.md`](steamworks-vtable-tables.md) | `current` | The authoritative vtable slot order per interface. Re-verify on a **Proton bump**. |

## Corrections, and where they live

The repo's rule is that a document reads as though it were written today, with the route not
taken at the bottom under **Wrong turns**. A reader who stops at the top of a file should be
right, not merely unsurprised. Two files carry substantial ones:

- `compatibilitytools-d-macos.md`: the first answer tested the Steam **data** dir and concluded
  discovery was broken on macOS. The same install-vs-data mistake was then made independently for
  `.cef-enable-remote-debugging`.
- `steam-overlay-feasibility.md` and `overlay-injection.md`: the overlay was got wrong in both
  directions, first that `dlopen` was structurally dead, then that a remote thread in a suspended
  process runs before the title's imports. Both were overturned by measurement.

## The re-verify list, by trigger

| after a… | re-check |
|---|---|
| **Steam client update** | `steam-drm-shared-memory.md` (the shared-memory object names are Valve's) · `compat-vdf-platform-override.md` (byte offsets, and `src/compat-enabler` patches against them) · `compatibilitytools-d-macos.md` (the bootstrapper can wipe a planted tool) · `app-mappings-self-contained.md` · `steamclient-js-api-macos.md` · `overlay-injection.md` (the renderer dylib is replaced) |
| **CrossOver upgrade** | `crossover-bridge-surface.md` (already behind) · `clean-bottle-provenance.md` · `overlay-injection.md` |
| **Proton bump** | `lsteamclient-mechanics.md` · `steamworks-vtable-tables.md` |

`instruments/` exists to answer exactly these. A module lives there because it would be *rerun*
to re-establish a claim one of these documents makes.
