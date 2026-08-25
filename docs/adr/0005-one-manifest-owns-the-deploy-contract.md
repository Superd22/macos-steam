# 5. One manifest owns the deploy contract, and drift is a build failure

Date: 2026-08-25

## Status

Accepted

## Context

Where every shipped artifact lands was knowledge held in eight places, in three languages,
kept in sync by hand. `C:\shim\steamclient64.dll` — the in-bottle payload path the whole
Level B mechanism rests on — appeared as a string literal in the launch script (four
times, including a `grep` guard and a registry write), twice in `overlayinject.c` (two
independent copies in the same file), and once more derived in `shim_pe.c`. The payload
root, the log dir, the injector basenames, the compat-tool dir name and the `steamclient*`
basenames each had the same shape: several literals, no interface, nothing that fails when
one of them moves.

`install.sh` said so out loud. A comment above its `STEAM_OSX` constant read "kept in sync
by hand with the same path inside the generated launcher below" — the launcher body is a
deliberately *quoted* heredoc, so the path could not be substituted in, and the two copies
were left to agree by discipline.

The failure mode is not a broken build; it is a launch that goes wrong three layers away.
A stale payload path means `steam_api.dll` finds no `steamclient.dll`, `SteamAPI_Init`
returns 0, and the title reports "Could not sign in to your Steam account" (#20). Nothing
in any log names the cause.

## Decision

**`src/layout/layout.json` owns every path and basename. The build emits the two dialects
that consume it, and a guard fails the build when one is restated.**

- `gen.py` emits `gen/shim_paths.h` (narrow and `_W` wide literals, for the four C/C++
  halves) and `gen/shim_paths.sh` (single-quoted assignments, dot-sourced by the five
  shell scripts). One input, two outputs, no interpretation.
- `check.py` walks `src/` and fails when a guarded value reappears as a literal in code.
  Comments are exempt on purpose: prose explaining *why* the registry value points at
  `C:\shim\steamclient64.dll` is documentation, not a second copy of the contract.
- `build.sh` runs both, and every other `build.sh` calls it first, so a manifest edit
  reaches whatever is being built without anyone remembering to regenerate.
- Composite paths are `joins` of earlier names, so even `C:\shim\steamclient64.dll` is not
  typed anywhere — it is `SHIM_DIR_WIN` joined to `PE64`.
- The two vdfs become templates. Steam matches the tool key inside `compatibilitytool.vdf`
  against the directory it was deployed into, and `toolmanifest.vdf` names the launch
  script: three facts that must agree, now all three from the manifest.
- The deployed compat tool carries `shim_paths.sh` beside the launch script, because at
  run time the repo is not on the machine.

`src/layout/` therefore joins the ship-set under ADR 0004's admission rule: remove it and
the deployed launch script has nothing to source.

## Alternatives rejected

**A C header only, with the shell scripts left alone.** Half the sites are shell — the
launch script is where the registry write, the `DllPath` plant and the payload copies live.
Covering only the C side would leave the two halves of the same path in different regimes.

**A shell-only contract, `#include`d nowhere.** The payload path has to be a compile-time
constant in the injector: it is what `patch_imports` writes into the target's import
directory before the title's first instruction runs. There is no environment to read.

**Generate the launch script and the vdfs wholesale from the manifest.** Rejected as the
wrong seam. Those files carry a great deal of load-bearing commentary (why the verb is
sniffed rather than assumed, why the front door and not a raw wineloader), and a generator
that owns the whole file makes that prose an artifact. The manifest owns the *paths*; the
scripts stay hand-written and read the names.

**Convention plus a README.** This is what was already in place, and the `install.sh`
comment is the evidence: an acknowledged hand-sync is still a hand-sync. Locality of
knowledge is worth having, but only enforcement makes drift loud.

## Consequences

- No behaviour changed. Every generated literal was verified byte-identical: the four
  binaries carry the same payload paths and basenames (checked in the built artifacts, narrow
  and wide), the planted `[Wine] DllPath` string is unchanged, and the generated launcher
  differs from its predecessor only by an unused intermediate variable.
- One edit now moves the whole stack. Renaming the in-bottle dir, relocating the payload or
  adding a third bitness is a manifest change plus a rebuild.
- `layout.json` is also the machine-readable contract a GUI installer needs, which is the
  half of the `install.sh` split (#33) that could not be expressed before.
- The guard is a real gate, not advice: reintroducing a literal fails the build with the
  file, line and the name to use instead.
- `gen/` is generated and gitignored, like every `dist/`. A checkout does not contain the
  header; the first `build.sh` writes it.
