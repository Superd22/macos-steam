# Changelog

This project follows [semantic versioning](https://semver.org).

## [0.4.0](https://github.com/Superd22/macos-steam/compare/v0.3.1...v0.4.0) (2026-09-05)

### Features

* **launcher:** state the wildcard mapping so Windows titles are installable here ([17e24b4](https://github.com/Superd22/macos-steam/commit/17e24b494a0411be78edd09be0c4b319480d53a6)), closes [#102](https://github.com/Superd22/macos-steam/issues/102)

## [0.3.1](https://github.com/Superd22/macos-steam/compare/v0.3.0...v0.3.1) (2026-09-05)

### Fixes

* **launcher:** "install Steam first" told a user with Steam installed to go get Steam ([f8f7b8b](https://github.com/Superd22/macos-steam/commit/f8f7b8b3393d19145cc802af491b053d540f0d03))

  If Steam is installed but has never been opened, the installer and
  the launcher now say so and tell you to open Steam once — instead of reporting
  that Steam is not installed and offering to download it again.
* **launcher:** CrossOver in /Applications read as CrossOver not installed ([3e4a4ce](https://github.com/Superd22/macos-steam/commit/3e4a4ce69c627435ede196decef46023249b9abd))

  CrossOver installed in /Applications — where its own installer puts
  it — is now found. It was only ever looked for in ~/Applications, so the app
  could tell you to install CrossOver no matter how many times you already had.
* **packaging:** brew never deployed anything — drop the post_install that could not work ([ff53088](https://github.com/Superd22/macos-steam/commit/ff530882365ba59a0ab600fc048259f22900ffff)), closes [Formula#run_post_install](https://github.com/Superd22/Formula/issues/run_post_install)

  `brew install macos-steam-shim` now tells you to run
  `macos-steam-shim` once to finish the install, and means it. The automatic
  deploy it used to attempt could never succeed — brew runs install steps in a
  sandbox with a throwaway home directory, and the payload has to go in yours.

## [0.3.0](https://github.com/Superd22/macos-steam/compare/v0.2.0...v0.3.0) (2026-09-05)

### Title compatibility

* **drm:** run wrapped titles through Valve's own signed client DLL ([27b183d](https://github.com/Superd22/macos-steam/commit/27b183d36f15da98120368dd7400d8c95d7676bc)), closes [#98](https://github.com/Superd22/macos-steam/issues/98)

  DRM-wrapped titles now run — the ones that failed with
  "Application load error 3:0000065432", such as Warhammer 40,000: Space Marine
  II. Run fetch.sh once from the compat tool's directory: it downloads Valve's
  own signed client DLL from Valve (~60 MB), and needs redoing after a Steam
  client update. The Steam overlay stays off for these titles; the rest of your
  library is unaffected.

## [0.2.0](https://github.com/Superd22/macos-steam/compare/v0.1.0...v0.2.0) (2026-08-26)

### Fixes

* **shim:** answer 1 from Set_SteamAPI_CCheckCallbackRegisteredInProcess ([ae4bcae](https://github.com/Superd22/macos-steam/commit/ae4bcaee46941bf594891d2c89b2e639479d65fd)), closes [#90](https://github.com/Superd22/macos-steam/issues/90)

### Title compatibility

* **compat-tool:** respect Steam's own per-title overlay setting ([23b7061](https://github.com/Superd22/macos-steam/commit/23b70616917064b9f471aaf6c0134fc8966b2889)), closes [#92](https://github.com/Superd22/macos-steam/issues/92)

  A title whose anti-cheat refuses to run alongside the Steam
  overlay can now be excluded on its own: untick "Enable the Steam Overlay
  while in-game" in its Steam properties and the shim skips overlay injection
  for that title, while the rest of your library keeps the overlay. Verified on
  Age of Empires IV. The cost is that the title has no overlay — no shift-tab,
  friends list or browser.
* **shim:** AoE IV reaches the menu — carry ISteamUser's native slot ([fb47c00](https://github.com/Superd22/macos-steam/commit/fb47c00381e109aadb0cea66b10dc2c8c14a1af1))

  Age of Empires IV now signs in to Relic Online and reaches the menu.
  It previously sat on its loading screen forever, because the shim called the
  wrong method for its Steam version and no sign-in ticket was ever requested.

### Build and packaging

* **packaging:** unbreak the release, and let commits carry a changelog note ([c0a04aa](https://github.com/Superd22/macos-steam/commit/c0a04aa27eced93df869998b45818a9b12e4e318))

  Release notes can now carry a short description under an entry,
  and title-compatibility changes get their own section.

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
