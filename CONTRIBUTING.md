# Contributing

Contributions are welcome — issues, reproductions, docs fixes and patches alike. This
project sits in Steam's launch path on hardware not everyone has, so a careful bug report
against a real title is worth as much here as code.

## Before you start

- Read [CONTEXT.md](CONTEXT.md) first: it defines the vocabulary (Level A, Level B, the
  seam, the payload) that the code and the docs both assume.
- The decisions, and why the alternatives were rejected, are in [docs/adr/](docs/adr/).
  If your change contradicts one, say so in the PR — an ADR can be superseded, but not
  silently.
- Build and install from a clone with the steps in the [README](README.md#from-a-clone).

## Reporting a bug

Open a [GitHub issue](https://github.com/Superd22/macos-steam/issues). Hold ⌥ while
opening the launcher, run **Diagnose**, and paste the report: it runs the whole troubleshooting table and captures the versions, which is most of what a
maintainer would ask for anyway. Include the title's app ID, and say whether the overlay
was on.

Two things account for most false reports, so check them first: Steam must be signed in
and **online**, and `ps aux | grep -i steam.exe` must be empty.

## Pull requests

- Branch off `main`, keep the change focused, and open the PR against `main`.
- If the change touches something the docs claim was measured, rerun the relevant harness
  under [instruments/](instruments/) and update the `FINDINGS.md` next to it. A
  `FINDINGS.md` records what was measured live, with exact versions and negative controls —
  it outranks prose when the two disagree.
- Moving a module out of `src/` is a release-surface change; call it out explicitly.

## Commit conventions

Commits must be [Conventional Commits](https://www.conventionalcommits.org) — the release
is computed from them, so a subject that cannot be parsed is a change that never ships.

```
feat(launcher): merge into DYLD_INSERT_LIBRARIES instead of clobbering it   -> minor
fix(shim): 32-bit titles could not sign in without both bitnesses           -> patch
feat(shim)!: drop the curated interface list                                -> major
compat(compat-tool): AoE IV runs with Steam's overlay box unticked          -> minor
docs: ...  test: ...  ci: ...  chore: ...                                   -> no release
```

### A `Changelog:` footer, when the subject is not enough

A subject line is one line, and some changes need a sentence a user can act on. Add a
`Changelog:` footer and its text is rendered as an indented description under that
commit's changelog bullet:

```
compat(compat-tool): anti-cheat titles can run, at the cost of no overlay

<the usual body: what was measured, what was rejected, why. NOT rendered.>

Changelog: Untick "Enable the Steam Overlay while in-game" in a title's Steam
properties and the shim skips overlay injection for that title, while the rest
of your library keeps the overlay.

Refs #92.
```

It is **opt-in on purpose**. Bodies here are long and internal — generators, drift
guards, parity tables — and none of that belongs in a release note, so a commit without
the footer contributes only its subject, as before. The note ends at the next blank
line, which is what keeps trailing footers like `Refs #92.` out of it. Write it for
someone reading the release page to find out whether their game works now.

This is why `.releaserc.mjs` is JavaScript and not JSON: the footer needs a writer
`transform`, and a function cannot live in a `.json` file.

`compat` is ours, not part of the Conventional Commits spec. It exists because this
project's headline claim is which titles run, and a compatibility change is not a `feat`
(nothing new was built) or a `fix` (nothing was broken) — a title simply started
working. Those land in their own **Title compatibility** changelog section, where a
user scanning a release for "does my game work now" will actually find them. Say what
changed for which title, and name the cost if there is one.

A `!` before the colon, or a `BREAKING CHANGE:` footer, is what makes a major. Scopes
follow the module names under `src/` (`launcher`, `shim`, `compat-tool`, `installer`,
`packaging`, `overlay-inject`).

Nothing enforces this at commit time on purpose; `npm run release:dry` is where you see
what your commits would produce.

## Repo layout

Three roots, one admission rule each. Whether a module ships is a **path**, not a judgement
call: `src/` is the beta cut.

```
CONTEXT.md                  the glossary — read this first
docs/adr/                   the decisions, and why the alternatives were rejected
docs/research/              the measured evidence behind each decision
  INDEX.md                  which of it is still true — read before any doc in here

src/                        reaches a user's machine
  layout/                   the deploy contract — one manifest of every shipped path
  installer/                build.sh (repo -> payload), deploy.sh (payload -> receipt)
    packaging/              the brew formula, and the release scripts CI drives
  launcher/                 the app you click: preflight, exec, settings/diagnose/uninstall
  compat-enabler/           the m_bCompatEnabled injector (Level A)
  compat-tool/              the compat tool + launch script (the Level A <-> Level B seam)
  shim/                     the bridge: PE steamclient(64).dll + native .so (Level B)
  overlay-inject/           gets Valve's overlay renderer into the game process

instruments/                rerun to re-verify a claim after a CrossOver or Steam bump
  harness/                  Spacewar (480) achievements — the acceptance test src/shim/run.sh drives
  overlay-probe/            d3dprobe (#26), inputprobe + input-parity-run.sh (#28)
  native-probe/             connprobe — the native-side connection oracle

attic/                      question closed; kept as evidence, never rerun
  seam-spike/               superseded wholesale by src/shim (ADR 0001)
  shimprobe/                the clean-bottle decoy dll
  overlay-probe/            metalprobe{,3,5}, u32probe, vendored fishhook
  native-probe/             probe, machprobe, interpose
```

**The admission rules.** A module belongs in `src/` if removing it breaks a user's install;
in `instruments/` if you would run it again to re-establish a claim the docs make; in
`attic/` if its question is closed and the answer is written down elsewhere. Nothing is in
two roots. Moving a module out of `src/` is a release-surface change.

Every `FINDINGS.md` under these roots is a record of what was measured live, on real
hardware, with the exact versions, including the negative controls. When something
disagrees with the README, the FINDINGS file is the one that was measured.

## Releasing

Versions are [semver](https://semver.org) and come from
[semantic-release](https://github.com/semantic-release/semantic-release).

**Actions → release → Run workflow.** You choose _when_; the commits since the last tag
choose _what_. There is no bump input on purpose — a number a human types is a number a
human can get wrong, and the commits already record which kind of change each one was.
Tick **dry run** to see the version and notes without publishing anything.

A subject semantic-release cannot parse is one it silently ignores — a batch of those is a
release that publishes nothing, so `--dry-run` before a real release is worth the thirty
seconds.

The whole thing is one job on a Mac, and the order is the safety property:

| step      | what runs                                                                                                                                                           |
| --------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `prepare` | [release-prepare.sh](src/installer/packaging/release-prepare.sh) — stamp `VERSION`, run every gate, build the tarball, **rebuild that tarball**, render the formula |
| `publish` | the GitHub release, with the tarball, its checksum and the formula as assets                                                                                        |
| `success` | [push-to-tap.sh](src/installer/packaging/push-to-tap.sh) — the formula into the tap                                                                                 |

`prepare` runs before `publish`, so if any gate fails no tag and no release ever come into
existence. Building the checkout would only prove the repo builds; rebuilding the _unpacked
tarball_ proves what a user downloads builds, and that it stamps a bare `0.2.0` rather than
the `0.2.0+gSHA` spelling `version.sh` emits inside a clone.

Both scripts run by hand too — that is deliberate, since a release path that only exists
inside CI is one nobody can test before it fires:

```sh
npm ci
npm run release:dry                                  # version + notes, no writes
./src/installer/packaging/release-prepare.sh 9.9.9   # the real build, into dist-release/
```

The tap push needs a `TAP_TOKEN` secret — a fine-grained PAT with Contents:write on
[homebrew-macos-steam](https://github.com/Superd22/homebrew-macos-steam) and nothing else. A
workflow's built-in `GITHUB_TOKEN` is scoped to the repo it runs in and cannot reach a
second one. Without it the release still publishes and carries the formula as an asset; the
step warns rather than failing.
