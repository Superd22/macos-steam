# `layout/` — the deploy contract

Where every shipped artifact lands, stated once.

Before this module the contract existed only as matching string literals: the
in-bottle payload path `C:\shim\steamclient64.dll` was written out in eight
places across `sh`, `C` and `C++`, the payload root in two, the injector
basenames in six. Nothing failed when one of them drifted — the failure surfaced
much later as a title that would not sign in, or an overlay that never armed.

| | |
|---|---|
| `layout.json` | the manifest — every path and basename, plus what each one is for |
| `gen.py` | emits `gen/shim_paths.h` (C/C++) and `gen/shim_paths.sh` (sh) |
| `check.py` | fails the build when a guarded value reappears as a literal in `src/` |
| `build.sh` | run both; every other `build.sh` calls this first |

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

The deployed compat tool carries its own copy of `shim_paths.sh` beside the
launch script, because at that point the repo is not on the machine.

## Adding or changing a name

Edit `layout.json` and rebuild. `atoms` hold literals; `joins` compose earlier
names, so a composite path like `C:\shim\steamclient64.dll` is not itself typed
anywhere. `guard: true` puts the value under `check.py`, which is what turns
drift into a build failure rather than a silent mismatch — set it on anything a
second module could plausibly restate.

Nothing here holds an absolute unix path. The macOS names are relative to
`$HOME` and the bottle names relative to the bottle dir, because whose `$HOME`
it is belongs to whoever is running, not to the contract.

`gen/` is generated and gitignored, like every `dist/`.

## Reading it from outside

`layout.json` is the machine-readable form of the payload layout: an installer
with a GUI, a packaging script or a CI check can read it directly rather than
scraping `install.sh`. Its shape is stable — `atoms` and `joins`, each entry
with a `name`, a value or an `of` list, and a `what`.
