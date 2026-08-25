# 4. The tree is split three ways: what ships, what measures, what is answered

Date: 2026-08-25

## Status

Accepted

## Context

This repo reached a working beta by spiking, probing and measuring its way there, and the
artifacts of that journey ended up sitting next to the code that ships, as peers, in one
`tools/` drawer. Ten directories, no ordering. `tools/shim/` — 2,900 hand-written lines
that reach a user's machine — and `tools/shimprobe/` — a decoy DLL whose question closed
weeks earlier — were indistinguishable by path. The only thing separating them was a
paragraph of README prose, which nothing enforces and nothing can read.

At the cusp of beta that stops being a tidiness problem. A beta cut, a CI target and a
release payload all need the same thing: **the ship-set has to be a path, not a judgement
call.** Roughly 1,900 lines of probe and spike code were inside the apparent scope of
"build the project" for no reason other than where they sat.

## Decision

Three roots, one admission rule each.

- **`src/`** — reaches a user's machine. A module belongs here if removing it breaks a
  user's install. `installer`, `compat-enabler`, `compat-tool`, `shim`, `overlay-inject`.
- **`instruments/`** — rerun to re-verify a claim after a CrossOver, Steam client or macOS
  bump. `harness`, `overlay-probe` (`d3dprobe`, `inputprobe` + `input-parity-run.sh`),
  `native-probe` (`connprobe`).
- **`attic/`** — the question is closed, the answer is written down elsewhere, and nothing
  here is rerun. `seam-spike`, `shimprobe`, the Metal-side `overlay-probe` harnesses, the
  superseded `native-probe` binaries.

Consequences of the rule, not separate decisions:

- Nothing is in two roots. `overlay-probe` and `native-probe` each split, and the split is
  the point — `connprobe` still gets rerun, `probe` never will.
- Nothing in `src/` may depend on `instruments/` or `attic/` at build or run time. The one
  crossing is `src/shim/run.sh` driving `instruments/harness` as its acceptance test, which
  is a test-time dependency.
- Each root carries its rule in its own `README.md`. Moving a module out of `src/` is a
  release-surface change.
- The rule is about *future tense*, not about code quality. An instrument's question is
  still live because its answer can expire; an attic entry's cannot.

The distinction the roots do **not** encode is "good code" versus "throwaway code".
`attic/seam-spike/bridge_pe.c` is the only latency instrumentation in the repo (~40 ns bare
seam, ~16–40 µs `GetPersonaName`); it is in the attic because the transport fork it settled
is settled, not because it is poor.

## Alternatives considered

**Leave `tools/` and document harder.** This is what was already in place, and it is what
failed: the README paragraph was accurate and still nobody could point CI at a directory.
Prose does not survive contact with a build script.

**A per-module `SHIPS: yes/no` marker file.** Keeps the flat tree and makes the ship-set
machine-readable, but the ship-set stays a set that has to be assembled by scanning, and a
missing or stale marker fails silently. A path fails loudly.

**Two roots (`src/` and everything else).** Simpler, and it does get the beta cut right.
Rejected because it loses the more useful of the two distinctions: after a CrossOver bump,
the question "what do I have to rerun?" has an exact answer, and collapsing instruments
into the attic destroys it.

## Consequences

- No behaviour changed. This ADR is moves, path fixups in the build scripts and `.gitignore`,
  and one rule per root. `src/shim/build.sh` passes its four ABI gates, every module builds
  from its new path, `src/installer/install.sh` deploys, and `src/shim/run.sh` still drives
  the harness to `SteamAPI_Init()=1` with real achievement reads and no Windows Steam.
- The payload-layout work (#32) reads much more obviously once the shipping modules sit
  together, which is why it comes next.
- `attic/shimprobe/` has no `FINDINGS.md`; its conclusions are in
  `docs/research/clean-bottle-provenance.md`, and a README in the module now says so.
- `instruments/native-probe/FINDINGS.md` stays on the instruments side even though most of
  the binaries it describes are archived: the claim it records is one that gets re-verified.
