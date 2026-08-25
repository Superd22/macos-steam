# `layout/` — the deploy contract, and the switches

Where every shipped artifact lands, and which way each runtime switch points,
stated once.

Before this module the contract existed only as matching string literals: the
in-bottle payload path `C:\shim\steamclient64.dll` was written out in eight
places across `sh`, `C` and `C++`, the payload root in two, the injector
basenames in six. Nothing failed when one of them drifted — the failure surfaced
much later as a title that would not sign in, or an overlay that never armed.

| | |
|---|---|
| `layout.json` | the manifest — every path and basename, every switch, plus what each one is for |
| `gen.py` | emits `gen/shim_paths.{h,sh}` (the paths) and `gen/shim_policy.{h,sh}` (the switches) |
| `check.py` | fails the build when a guarded value reappears as a literal in `src/` |
| `check_policy.py` | fails the build when the two dialects of a switch predicate disagree |
| `build.sh` | run all three; every other `build.sh` calls this first |

## Using it

C and C++ — add `-I../layout/gen` and include the header. Names come in a narrow
and a wide form, the wide one suffixed `_W`:

```c
#include "shim_paths.h"
lstrcpynW(buf, SHIM_PATH_PE64_WIN_W, MAX_PATH);   /* C:\shim\steamclient64.dll */
```

Shell — dot-source the fragment:

```sh
. ../layout/gen/shim_paths.sh
cp -f "$SHIM_DIST/$SHIM_PATH_PE64" "$SHIMDIR/$SHIM_PATH_PE64"
```

The deployed compat tool carries its own copy of `shim_paths.sh` and
`shim_policy.sh` beside the launch script, because at that point the repo is not
on the machine.

## Switches

A switch is one environment variable, one default, one predicate — the policy
half of the manifest (#33, ADR 0006). Ask it; never re-derive it:

```sh
. ../layout/gen/shim_policy.sh
shim_overlay_enabled && echo "the overlay is on"
shim_overlay_export 0          # state the answer to children, never imply it
```

```c
#include "shim_policy.h"
if (shim_overlay_enabled()) ensure_seam();   /* getenv, or GetEnvironmentVariableA on PE */
```

`SHIM_OVERLAY` defaults to **on**: unset means on, and `0` or empty means off.
That asymmetry is why the predicate exists — a caller that wants the overlay off
has to state a literal `0`, and five separately-written env tests did not all
agree on it. The env var name is guarded like a path, so a second `getenv` of it
fails the build; use `SHIM_ENV_OVERLAY` when a message needs to name it.

## Adding or changing a name or a switch

Edit `layout.json` and rebuild. `atoms` hold literals; `joins` compose earlier
names, so a composite path like `C:\shim\steamclient64.dll` is not itself typed
anywhere. `guard: true` puts the value under `check.py`, which is what turns
drift into a build failure rather than a silent mismatch — set it on anything a
second module could plausibly restate. A `switches` entry needs no `guard`: its
env var name is guarded unconditionally, since a switch read in a second place
is a policy re-derived, not merely a string repeated.

Nothing here holds an absolute unix path. The macOS names are relative to
`$HOME` and the bottle names relative to the bottle dir, because whose `$HOME`
it is belongs to whoever is running, not to the contract.

`gen/` is generated and gitignored, like every `dist/`.

## Reading it from outside

`layout.json` is the machine-readable form of the payload layout: an installer
with a GUI, a packaging script or a CI check can read it directly rather than
scraping `install.sh`. Its shape is stable — `atoms`, `joins` and `switches`,
each entry with a `name`, a value or an `of` list, and a `what`. A GUI toggle
for the overlay (ADR 0002's vehicle B) has one target: the switch entry, and the
predicate everything already calls.
