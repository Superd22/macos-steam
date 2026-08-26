# `launcher/` — the app you click

Vehicle A of ADR 0002, as a Mach-O. It replaces the generated shell script that
`deploy.sh` used to write into the bundle, and it is what makes the beta
installable by someone who will never read a log (#42).

**Prime rule: the happy path shows no UI.** Clicking the icon behaves exactly
like clicking Valve's own Steam.app — a preflight measured in stat calls, then
`execve` into `steam_osx`. "Already running" needs no handling: Valve's
single-instance logic forwards a second launch to the running client and focuses
it, so re-clicking the Dock icon means *focus Steam*, the same muscle memory as
before.

| Gesture | What happens |
|---|---|
| Click | preflight → exec. Steam opens, or focuses. Zero UI. |
| ⌥-click | settings / diagnose / uninstall |
| “Steam Play Settings” (Spotlight) | the same pane — the nested helper app, for people who were never told about ⌥ |
| First run, or preflight broken | the checklist, the only time UI appears uninvited |

| | |
|---|---|
| `main.swift` | mode dispatch, and the exec that ends the process |
| `Launch.swift` | the child environment: the DYLD merge (#85), the tool path, the stated overlay |
| `Preflight.swift` | the six checks, as verdicts a user can act on |
| `LogWatch.swift` | reads `patched 1 site(s)` so the user never has to |
| `Diagnose.swift` | the README's troubleshooting table, executed |
| `Receipt.swift` / `Prefs.swift` | what is deployed (ADR 0010), and what the user chose |
| `AppModel.swift` / `UI.swift` | the two panes |
| `check_launch_env.sh` | #85's acceptance test, run by `build.sh` on every build |

## Why it is compiled, and why locally

Unsigned, un-notarized apps are blocked behind System Settings on macOS 14+ —
disqualifying for something that injects into Steam. Locally built binaries
carry no quarantine bit, so Gatekeeper never interrogates them. That is why
`build.sh` exists on the user's machine at all, and why the brew formula builds
from source rather than shipping a binary (#42).

Ad-hoc signed, like the injector dylib. When a Developer ID arrives, one line in
`build.sh` changes and nothing else does.

## The contract

The launcher is in the ship-set, so it reads the deploy contract rather than
restating it: `gen/ShimPaths.swift` and `gen/ShimPolicy.swift` are the third
dialect `layout/gen.py` emits, `check.py` holds `.swift` to the same drift
guard, and `check_policy.py` runs the Swift predicate against the C and sh ones
on the same table of values. A path typed into this module fails the build.

## Testing it

Two flags answer a question and exit, without UI and without launching Steam:

```sh
./dist/launcher --diagnose     # the support report, as text
./dist/launcher --print-env    # exactly what the exec would carry
```

`--print-env` is what makes #85's acceptance test a shell script rather than a
manual Steam launch. Exercise the app itself through **LaunchServices**, not
from a shell: ADR 0002's correction is that the two paths differ in both
launchability and process architecture, and the shell path is more permissive on
each.
