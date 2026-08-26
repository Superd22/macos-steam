# Changelog

This project follows [semantic versioning](https://semver.org).

## 0.1.0 — 2026-08-26

First public beta. Play Windows-only Steam titles from the **native** macOS Steam client on
Apple Silicon, through CrossOver — one library, one client, achievements and overlay intact.

### Install

```sh
brew tap Superd22/macos-steam
brew install macos-steam-shim
```

### What works

- **The shim** — a reimplementation of `steamclient.dll` that bridges the Windows title to
  the native macOS client in-process. Both bitnesses: 64-bit titles load `steamclient64.dll`,
  32-bit ones `steamclient.dll` under a different registry value, and shipping only the
  64-bit half was what made 32-bit titles fail to sign in.
- **Every interface version Proton defines** is generated, not curated. The thunks, the
  vtables, the struct layouts and the overload sets all come from Proton's own PE-side
  sources; the marshalling facts a signature cannot carry are declared explicitly.
- **The compat gate** is flipped in memory at each Steam launch by an injected dylib, so
  Valve's Steam.app is never modified.
- **The Steam overlay**, on by default, injected at process creation. A single predicate
  owns the switch, and it is a stored preference the launcher reads at exec — changing it
  never needs a reinstall.
- **The launcher** — a compiled SwiftUI app that shows no UI when it works. It preflights in
  milliseconds and execs `steam_osx`; hold ⌥ for settings, diagnostics and uninstall. First
  run is a plain-language checklist that creates the CrossOver bottle for you and confirms
  Steam Play switched on by reading the log itself.
- **Deploy is a copy and a symlink swap that leaves a receipt** — every file and hash
  recorded, so `--verify`, `--rollback` and `--uninstall` all work with no checkout present.

### Known limits

- Apple Silicon only, macOS 14+. CrossOver is required.
- The overlay switch is per-session, not per-title: Steam captures the environment once, at
  client start.
- Unsigned, ad-hoc only. There is no Apple Developer ID yet, which is why the formula builds
  on your machine rather than shipping a binary — a locally built binary carries no
  quarantine bit, so Gatekeeper never interrogates it.

[Full history](https://github.com/Superd22/macos-steam/commits/v0.1.0)
