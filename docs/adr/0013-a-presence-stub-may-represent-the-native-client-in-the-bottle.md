# 13. A presence stub may represent the native client inside the bottle

Date: 2026-08-27

## Status

Accepted

Sharpens the "no Windows Steam" property stated in `CONTEXT.md` and assumed by
[ADR 0001](0001-unixlib-transport-for-the-bridge.md). Unblocks #97.

## Context

Steam DRM does not use the Steamworks API. It looks for a Steam **process**.

Measured in `docs/research/steam-drm-shared-memory.md`: a DRM-wrapped title (Space Marine II,
2183900) calls `CreateInterface` only to trace it back to the file that provides it, then reads
`HKCU\Software\Valve\Steam\ActiveProcess\pid` — the key our own launch script writes — gets
`ERROR_FILE_NOT_FOUND`, later probes for the Windows client's `Local\SteamStart_SharedMem*`
objects, finds nothing, and shows `Application load error 3:0000065432`. It never dispatches a
single vtable method, so no amount of #45 coverage can reach it.

### What Proton does

Proton is two components, not one:

| | what it is |
|---|---|
| `lsteamclient` | the shim. A PE `steamclient64.dll` forwarding Steamworks across the seam to the **real Linux client**. Our `src/shim/` is this, ported. |
| `steam_helper` (`steam.exe`) | ~1000 lines of C. Writes `ActiveProcess\pid`, creates a window titled "Steam", owns the DRM events and semaphores, and **launches the game as its child**. |

Neither is a Windows Steam client. Proton does not ship one, and neither will we. The second
component is a *stub whose job is to be present* on behalf of a real client running outside.

We have the first and not the second. That is the entire difference, and it is the smaller half.

## Decision

**A Windows process may exist in the bottle for the sole purpose of representing the native macOS
Steam client, provided it holds no Steam logic of its own.**

The property this project keeps is unchanged in substance and sharper in wording:

- **No Windows Steam client runs.** Still absolutely true. No Valve Windows binary, no Windows
  login, no Windows Steam install, no second account session.
- **A Windows process may represent one.** New, and what this ADR grants.

The distinction that makes this safe is the same one the shim already lives by: the stub
**forwards or reflects, it never decides**. It may say "a Steam client is present and it is
process N" because one genuinely is present and authenticated — outside the bottle, as the real
macOS Steam.app, which this stack already requires to be running and already refuses to work
without. It may not mint an answer the native client did not give.

### The project this makes explicit

**Proton for macOS, minus the emulator — that part is deferred to CrossOver.** Every structural
question of the form "should we do X" now has a first answer available: *what does Proton do, and
can we do that without shipping a Wine of our own?* Where the answer diverges, the divergence is
worth an ADR of its own.

## Consequences

**A new module.** A presence stub joins the ship-set (ADR 0004), the deploy contract (ADR 0005)
and the receipt. It is the first component we ship whose purpose is to be *observed* rather than
called, which makes "is it running" a health question the launcher's diagnose pane should answer.

**Launch topology changes.** Under Proton the stub is the game's parent. Ours currently launches
the title directly from the compat-tool script, so adopting the parent role touches the one place
every title's launch already flows through — and the overlay injector already occupies that slot
(ADR 0003). Those two have to be sequenced, not stacked blindly.

**The boundary needs a guard, not just a paragraph.** "Forwards or reflects, never decides" is a
rule about every future line of the stub, and rules that live only in prose drift. Whatever the
stub answers should be traceable to something the native client said, the way the switches are
asked rather than re-derived (ADR 0006).

**It is not yet known to work.** The `pid` hypothesis is supported, not proven — the wrapper's
branch logic is in unpacked memory and has not been read. This ADR authorises the experiment that
settles it; it does not claim the outcome. If `pid` turns out not to be the gate, the decision
recorded here still stands and the surface required may be larger.

**What is still refused.** Defeating an integrity or anti-tamper check on our own binaries, forging
a signature, or answering an ownership question the native client did not answer. The measured
wrapper reads our DLL from disk (`GetFileSize` → `ReadFile` → `HeapFree`) and we do not know what
it does with the bytes. If it turns out to require our DLL to *impersonate* Valve's, that is where
this stops.

**It did.** Measured 2026-08-27 and recorded in
[`docs/research/steam-drm-shared-memory.md`](../research/steam-drm-shared-memory.md): the DRM stub
verifies an **RSA-1024 signature over `steamclient64.dll`** against three public keys hard-coded in
the title, looking for a `"VLV\0"` block at DOS-header offset `0x40` — the format Valve's own
`steam_api64.dll` carries.

**But that does not close it, and a first draft of this paragraph wrongly said it did.** Proton does
not sign `lsteamclient` — it cannot: `winebuild` writes `"Wine builtin DLL"` at offset `0x40` and
Valve's `VLV\0` block lives at `0x40..0xCB`, so a PE is one or the other. Proton instead patches its
**loader**: `build_module` special-cases a module named `steamclient64` (and the overlay renderer),
loads `lsteamclient.dll` beside it, rewrites the real DLL's exports into trampolines into the shim,
and sets `LDR_DONT_CALL_DLLMAIN`. The wrapper therefore inspects **Valve's genuine signed DLL, whose
file on disk is untouched**, and gets a truthful yes.

So the refusal above stands and is unaffected — we still forge nothing — while the outcome is
narrower than "out of reach". **Forging a signature is refused; using Valve's real one, as Proton
does, is the same shape as ADR 0003's use of Valve's own overlay renderer and is not refused.** What
blocks it is provisioning (macOS Steam ships no `legacycompat/steamclient64.dll`) and finding a
trampoline site outside CrossOver's loader — engineering and distribution, not cryptography.

The presence-stub decision above is unchanged: the stub was built, the surface works, and the
handshake it could not answer turned out not to be a presence question at all.
