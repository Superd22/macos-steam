# 11. The launcher is a compiled app that shows no UI when it works, and the overlay moves to launch time

Date: 2026-08-26

## Status

Accepted

## Context

ADR 0002 chose vehicle A: our own unhardened `.app` that execs Valve's
`steam_osx`. It shipped as a shell script `deploy.sh` generated into the bundle.
That was enough to prove the mechanism and not enough to ship (#42), for three
reasons that are not about polish:

- **Nothing checks anything before launching.** Every precondition — CrossOver,
  a clean bottle, a deployed payload — is discovered at *game* launch, three
  layers away, as a title that will not sign in. The user's evidence is a log
  they have to be told to read.
- **The overlay was baked in at install time.** Turning it off meant a
  reinstall, which is disqualifying for the target user who "will never re-run
  an installer to change a setting".
- **A `#!` script cannot carry the rest.** No settings, no uninstall, no version
  in the About box — and it hid a bug: `/bin/sh` drops `DYLD_*` on the way in,
  so the script never saw an inherited `DYLD_INSERT_LIBRARIES` and its clobber
  of that variable (#85) was invisible.

## Decision

**A Swift/AppKit Mach-O, built locally, whose happy path shows no UI.**

- **Prime rule: clicking the icon behaves exactly like clicking Valve's
  Steam.app.** A preflight of stat calls, then `execve`. No window, no splash,
  no delay to measure. "Already running" is not detected and needs no handling:
  Valve's single-instance logic forwards a second launch to the running client
  and focuses it, so re-clicking the Dock icon means *focus Steam* — the muscle
  memory users already have.
- **UI appears in exactly three cases**: the settings pane when asked for
  (⌥ at launch, or the nested "Steam Play Settings" helper app so Spotlight can
  find it without knowing the gesture), and the checklist when preflight is
  broken or nothing has been proven yet. Uninvited UI is a failure report.
- **The app verifies itself.** On the first run it watches `compat-enabler.log`
  for `patched 1 site(s)` and says "ready". That line was always the right
  signal, addressed to the wrong reader. Afterwards the question is asked
  *retrospectively* — "did the last launch open the gate?" — and never again
  interactively. Scoping proof to a payload version was tried first and is
  wrong: it makes every update an unproven claim, so every update greets the
  user with a checklist, which is the uninvited UI the prime rule forbids. A
  payload that breaks the injector is instead caught on the launch after it,
  when there is something real to report.
- **The overlay becomes a stored preference read at launch time.** ADR 0006's
  single owner is untouched: the *rule* is still the generated predicate, and
  the Swift dialect of it is checked against the C and sh ones on the same table
  of values. What moved is only where the value comes from — a literal baked
  into a script becomes a preference read at exec, so changing it is a toggle
  and a relaunch, never a reinstall. Per session, not per title: the environment
  a compat tool sees is captured when Steam starts. Per title is plumbable
  through the compat tool later and is deliberately not v1.
- **Built locally, ad-hoc signed.** Not a preference — it is the shipping story.
  An unsigned, un-notarized app delivered as a DMG is blocked behind System
  Settings on macOS 14+, which is disqualifying for something that injects into
  Steam. A locally built binary carries no quarantine bit, so Gatekeeper never
  interrogates it. This is also why the brew formula builds from source.
- **The launcher is in the ship-set and reads the contract.** `gen.py` emits a
  third dialect (`ShimPaths.swift`, `ShimPolicy.swift`), `check.py` holds
  `.swift` to the drift guard, and `check_policy.py` runs three predicates
  instead of two.

## Alternatives rejected

**Keep the shell script and add a separate settings app.** Two artifacts that
must agree about the launch environment, where the one that launches Steam is
the one that cannot be tested — `/bin/sh` hides the inherited-environment case
entirely.

**Show a small progress window on every launch.** It is the difference between
"a launcher" and "a thing between me and Steam". The preflight is stat calls; it
has nothing to report while it runs.

**Detect an already-running Steam and focus it ourselves.** Valve already does
this correctly, including the window activation. Reimplementing it adds a state
machine whose only possible outcome is being wrong sometimes.

**Ship the overlay preference as a compat-tool per-title setting.** The
environment is captured at Steam start; per-title needs the compat tool to carry
it, which is real work and a different seam. Stated as not-v1 rather than
half-built.

## Consequences

- The two-line install can end at "click the app": the first click explains
  anything missing, offers to create the bottle, and proves the gate itself.
- `DYLD_INSERT_LIBRARIES` is now genuinely inherited, so #85's merge is
  load-bearing rather than theoretical. It has an acceptance test that runs on
  every build (`check_launch_env.sh`), asked through a `--print-env` flag rather
  than by launching Steam.
- `--diagnose` prints the same findings as text, which is what a user pastes
  into an issue. The logs themselves are deliberately not included: they name
  the whole Steam library, which is why they live owner-only outside `/tmp`.
- A Swift toolchain is now needed to build the *launcher*. `build.sh` treats it
  as optional in one direction only: without `swiftc` the payload still deploys
  and `deploy.sh` writes the shell launcher it always wrote.
- Uninstall, verify and rollback are the deploy module's (ADR 0010); the app
  shells out to the `deploy.sh` inside the payload rather than reimplementing
  checks that must not disagree.

## Links

- Issue [#42](https://github.com/Superd22/macos-steam/issues/42) — the release
  packaging this is the third item of.
- Issue [#85](https://github.com/Superd22/macos-steam/issues/85) — the
  `DYLD_INSERT_LIBRARIES` merge, whose acceptance test only a Mach-O can meet.
- ADR 0002 — vehicle A, and the two corrections (`LSEnvironment`, arm64) that
  still apply to the bundle this app ships in.
- ADR 0006 — the switch whose *value source*, not whose rule, this moves.
- ADR 0010 — the deploy seam this app is the third caller of.
