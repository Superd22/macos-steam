# 6. One predicate owns the overlay switch

Date: 2026-08-25

## Status

Accepted

## Context

"Unset means ON" is the overlay's default since #21's route was proven across titles
(ADR 0003's addendum). It was a policy with no module: five sites derived it, each with
its own env test, in three languages.

| site | test |
|---|---|
| `src/installer/install.sh` | `[ "${SHIM_OVERLAY:-1}" = 0 ]` |
| `src/compat-tool/steamclient-shim-launch.sh` | `[ "${SHIM_OVERLAY:-1}" = 1 ]`, three times in one file |
| `src/shim/shim_unix.cpp` | `if (on && (!*on \|\| *on == '0')) return;` |
| **`src/shim/shim_pe.c`** | **`GetEnvironmentVariableA("SHIM_OVERLAY", NULL, 0) > 0`** — presence, not value |
| `src/shim/run.sh`, `instruments/harness/harness.c` | two more copies, for the dev path |

The PE half's test is the one that disagreed. `GetEnvironmentVariableA` with a NULL buffer
returns the *length* the value needs, so it returns 2 for the string `"0"` — the variable
exists, therefore the overlay is on. The launch script's no-injector branch exports exactly
that (`SHIM_OVERLAY=0`, deliberately, because leaving it unset would mean ON), and the
installer bakes a literal `0` or `1` into the launcher. So in the one configuration whose
own comment at `shim_pe.c:686` says it must not, `DllMain` ran `ensure_seam()` and
`hook_child_creation()` anyway.

That divergence has not been observed as a user-visible failure, and would not be expected
to produce one: the unix half declines to `dlopen` the renderer under the same
configuration, so the seam binds and nothing arrives. This is filed and fixed as five
implementations of one rule where one disagreed, not as a reproduced defect.

The interesting property is not the bug but its shape. Nothing was wrong with any single
site read on its own; the rule was simply not anywhere, so each caller wrote it again, and
the one caller in the least testable position — a DLL in a title's `DllMain`, under loader
lock, inside a bottle — wrote it differently.

## Decision

**`layout.json` grows a `switches` section, and one generated predicate answers the
question everywhere.**

- A switch is one env var, one default, one predicate name, one list of off values:
  `SHIM_OVERLAY` / on / `shim_overlay_enabled` / `0`, empty.
- `gen.py` emits `gen/shim_policy.h` (a `static inline` predicate; `getenv` on unix,
  `GetEnvironmentVariableA` on PE, since a DLL injected into a title cannot count on that
  process's CRT) and `gen/shim_policy.sh` (a shell function, plus `shim_overlay_export`,
  because stating the answer to child processes is part of the same policy).
- `check.py` guards the env var name the way it guards a path, matched as a whole word so
  `SHIM_OVERLAY_PAYLOAD` is not a hit. A second `getenv("SHIM_OVERLAY")` anywhere under
  `src/` fails the build. Messages that need to name the variable use `SHIM_ENV_OVERLAY`.
- `check_policy.py` runs both dialects over the same table of values — unset, empty, `0`,
  `1`, and values that are none of those — and fails when they disagree. Two emitters of
  one rule is how this happened at a larger scale; the parity check is what stops the
  smaller version of it.
- The deployed compat tool carries `shim_policy.sh` beside `shim_paths.sh`, and the launch
  script refuses to start without both.

The default does not change. Only the number of places that know it.

## Alternatives rejected

**Fix `shim_pe.c` and leave the other four.** The one-line version of this ticket. It
closes the divergence that exists and leaves the mechanism that produced it: five authors
of one rule, with the next flip of the default having five edits to make and one of them
easy to miss — which is exactly what the flip in #21 already did once.

**A shared C header only, shell left alone.** Three of the five sites are shell, including
both places that *write* the variable. Covering only C would leave the two halves of one
policy in different regimes, which is the same failure at half the size.

**Make the value a tri-state (`on`/`off`/`auto`) while we are here.** Out of scope: the
ticket is a divergence between implementations of the current rule, and the launch script's
injector interlock already provides the "on but undeliverable" case. Widening the value
space is a separate decision, and easier to take once there is one predicate to widen.

## Consequences

- The polarity lives in one place. A future UI toggle (ADR 0002's vehicle B) has a single
  target, and a future flip of the default is a manifest edit plus a rebuild.
- The PE half now declines to bind the seam under `SHIM_OVERLAY=0`, which is what its own
  comment always said it did. On the shipped path the observable behaviour is unchanged —
  the renderer was never loaded in that configuration either way.
- Measured rather than argued, both PE tests compiled with mingw and run under CrossOver's
  `wineloader` (bottle `shim-clean`), since the Win32 branch is the half no host-side test
  reaches:

  | `SHIM_OVERLAY` | old `GetEnvironmentVariableA(..., NULL, 0) > 0` | `shim_overlay_enabled()` |
  |---|---|---|
  | unset | 0 | 1 |
  | `0` | **1** | 0 |
  | `1` | 1 | 1 |
  | empty | — | 0 |
  | `2` | — | 1 |

  The old test was wrong in both directions: it read the stated off (`0`) as on, and the
  manifest's default (unset) as off. Only the second is masked on the shipped path, where
  the launcher always states a literal `1` or `0`. The new column matches the shell
  predicate value for value, which is what `check_policy.py` asserts on every build.
- `install.sh` and `run.sh` now read an explicitly empty `SHIM_OVERLAY=` as off rather than
  as on. That is the manifest's rule applied uniformly; the old shell `:-` default treated
  empty as unset while the unix half already treated it as off.
- The build has one more gate, and it is cheap: `check_policy.py` compiles a small probe
  with the host `cc` and runs `sh`, both offline.
- `instruments/harness/` compiles against the manifest now (`-I ../../src/layout/gen`). An
  instrument that re-derives the rule it is measuring can pass while the shipped stack is
  wrong.
