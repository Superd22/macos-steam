# 10. Deploying is a copy plus a symlink swap, and it leaves a receipt

Date: 2026-08-26

## Status

Accepted

## Context

`install.sh` was one script with four responsibilities: preflight, build, payload staging,
and launcher emission (#34). Deploying was expressed *as* build orchestration, so a user
installing a beta needed Xcode CLT and mingw-w64 — there was no path that laid down an
already-built payload. ADR 0002 accepted a shipped `.app`, and #42 adds a brew formula;
both are callers that have no compiler and, in the formula's case, no checkout by the time
the payload is deployed.

Three further gaps were live at the same time, and they turn out to be one gap:

- **Nothing recorded what was laid down.** Verification was the user reading
  `compat-enabler.log` for `patched 1 site(s)` — which says the injector ran, not that the
  files on disk are the files that were shipped. Uninstall was two inline `rm -rf`s against
  paths typed into the same script.
- **Nothing carried a version.** The `.app` said `0.1` because someone typed `0.1`.
- **An update was an in-place overwrite**, so there was no state in which the previous
  payload still existed and a bad update could be undone without a network.

Millennium's macOS port (surveyed in #42) solves the last one with a shape worth taking:
install to `runtime/<version>/`, symlink `current` at it, and let the app bundle hold only
references *into* `current`. An update swaps one symlink.

## Decision

**`deploy(payload) -> receipt`, where a payload is a directory and a deploy is a copy, a
symlink swap, and a receipt.**

- `src/installer/build.sh` — repo → payload. Needs clang, mingw-w64, python3, a checkout.
- `src/installer/deploy.sh` — payload → receipt. Needs none of them.
- `src/installer/install.sh` — the first adapter: build, then deploy. The brew formula is
  the second and the launcher app the third.

The payload is **self-describing**: beside the artifacts it carries `VERSION`, the
contract fragments, `compatibility.env`, and `deploy.sh` itself. That last one is what
makes an installed machine able to verify, roll back and uninstall itself with no repo and
no network — at uninstall time there is certainly no checkout, and after a `brew uninstall`
there is no formula either.

- **Versioned, behind `current`.** The payload lands in
  `<root>/versions/<version>/` and `<root>/current` points at it. Every consumer path in
  the manifest is a join onto `LIVE_REL`, so the launcher, the compat-tool registration
  Steam has cached, and the DYLD entry are all version-blind. One previous version is kept:
  `deploy.sh --rollback` re-runs the deploy that the previous version dir carries.
- **The receipt is the record.** `receipt.json` at the payload root — outside any version,
  because it outlives one — holds the version, the deploy time, the overlay setting, the
  compatibility statement, and every file with its sha256. `--verify` re-hashes it and also
  checks that `current` points where the receipt says. `--uninstall` reads it.
- **One version string, computed once.** `src/installer/version.sh` reads the repo's
  `VERSION`, appending `+g<sha>[.dirty]` when a `.git` is present, because a build from a
  clone is not the release it is numbered after. It reaches the payload, the receipt and
  `CFBundleShortVersionString` from that one place.
- **Deploying a payload that is already the live version is a repair, not an error.** It
  refreshes the symlink, the launcher and the receipt, and deliberately skips the copy —
  `rm -rf`ing the destination first would delete the source, since the source *is* the
  destination whenever `current/deploy.sh` is run with no arguments.

## Alternatives rejected

**Keep one script, add a `--no-build` flag.** The flag is a seam that nothing can be
tested against: the second caller still has to know the repo's directory shape, which is
exactly what it does not have. A directory is a contract two callers can meet.

**Put the receipt inside the version dir.** It would be deleted by the prune that a
rollback depends on, and there would be two of them after an update, with nothing saying
which is live.

**Hash-verify by re-running the build and diffing.** Requires the toolchain on the user's
machine — the thing the split exists to remove — and answers a different question
(reproducibility) from the one a user has (is my install intact).

**A `.pkg` receipt / `pkgutil`.** Ties the record to an installer technology we rejected
(unsigned `.pkg` hits the same Gatekeeper wall as the DMG, #42) and cannot record the
compatibility statement or the overlay setting.

## Consequences

- The build toolchain leaves the user's requirements list. A payload built anywhere
  deploys anywhere, which is what the brew formula and the launcher app both need.
- Uninstall and verify are interfaces rather than inline `rm -rf`s, and both work from an
  installed machine with no repo.
- An update is one symlink; a bad one is one `--rollback`. Disk use is bounded at two
  versions.
- The consumer paths moved: `ENABLER_REL` and `COMPAT_TOOLS_REL` are now joins onto
  `LIVE_REL`, not onto `PAYLOAD_REL`. A pre-versioning install has stale copies at the old
  root, and `deploy.sh` removes them on the next deploy rather than leaving a second copy
  of the payload where a confused reader finds it first.
- `layout.json` gained the names this needs (`VERSIONS`, `CURRENT`, `RECEIPT`,
  `LAUNCHER_BIN`), so none of them is typed into a script — ADR 0005 applies to the deploy
  layout exactly as it applies to the in-bottle one.

## Links

- Issue [#34](https://github.com/Superd22/macos-steam/issues/34) — the seam this cuts.
- Issue [#42](https://github.com/Superd22/macos-steam/issues/42) — the release packaging
  this unblocks, and where the Millennium survey is written up.
- ADR 0002 — the shipping vehicle whose `.app` is the third caller.
- ADR 0005 — one manifest owns the deploy contract.
