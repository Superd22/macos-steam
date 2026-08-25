# macos-steam

MacOS Gaming has come a long way in the last few years: I have personally been using [Crossover](https://www.codeweavers.com/crossover) on my MacBook Pro for quite a while with great success, only one thing bothered me with the experience: having to have a dedicated steam for windows install, meaning I have two steam installs depending on if the game is native MacOS (yeah, there are a couple) or windows. Two libraries to manage.

This app is an attempt to brings the steamdeck's [Steam Play](https://store.steampowered.com/steamplay/) experience to MacOS: Download, Launch, Play from MacOS' Steam through crossover.

> **Status: working beta. Sources only.**
> There is no signed download or release build yet. You clone this repo and run the
> installer, which builds and deploys from source. It has been driven end-to-end on real
> retail titles on Apple Silicon, but it is young and it sits in
> Steam's launch path, so expect breakage. See [Known limits](#known-limits).

## What it does

macOS Steam already knows how to download a Windows depot and hand the `.exe` to a
compatibility tool, the same Steam Play machinery as Proton on Linux. Two things stop it:
the feature is gated off on macOS, and nothing on macOS answers the Steamworks API for a Windows game.

This repo supplies both halves:

- **Level A, opening the gate:** a small injected dylib flips the client's latched
  `m_bCompatEnabled` at runtime, and a compat tool registers itself so every Windows-only title routes through it. Valve's own files are never modified.
- **Level B, the bridge:** a replacement `steamclient64.dll` (and 32-bit `steamclient.dll`)
  inside the CrossOver bottle. It contains no Steam logic: it marshals every call across the Wine unix seam to a native `.so` that hosts Valve's real macOS `steamclient.dylib`,
  in-process. So the game talks to your _native_ Steam client. Achievements unlock on the real server; the overlay is Valve's own `gameoverlayrenderer.dylib`, injected into the game
  at process creation.

The vocabulary above is defined in [CONTEXT.md](CONTEXT.md); the decisions behind it are in
[docs/adr/](docs/adr/).

## Requirements

- Apple Silicon Mac, macOS 14+ (developed on macOS 26, M3 Pro).
- **CrossOver** installed at `~/Applications/CrossOver.app` (25.1.1 and 26.2 both exercised).
  CrossOver is required. The launch goes through its front door, so its D3DMetal/GPTK wiring comes along.
- The **native macOS Steam client**, installed, and signed in **online**.
- Build toolchain: Xcode command line tools (clang) and mingw-w64:
  ```sh
  brew install mingw-w64
  ```

## Install

1. **Create the bottle the games will run in.** It must be a clean `win10_64` bottle with
   _no_ Windows Steam installed. The shim is the entire client, and a real
   `steamclient64.dll` in the bottle would win the lookup instead.

   ```sh
   "$HOME/Applications/CrossOver.app/Contents/SharedSupport/CrossOver/bin/cxbottle" \
       --create --bottle steam-shim --template win10_64
   ```

   (`steam-shim` is the default name; override it with `SHIM_BOTTLE` if you use another.)

2. **Build and deploy.**

   ```sh
   ./tools/installer/install.sh
   ```

   This builds anything missing and lays down two things:
   - the **payload** in `~/Library/Application Support/macos-steam-shim/` — the injector, the
     compat tool, and both bitnesses of the shim. It lives outside every bundle, so a Steam
     update cannot wipe it.
   - the **launcher** `~/Applications/Steam (macOS Play).app` — an unhardened `.app` whose
     `LSEnvironment` carries the injector and the tool path into Valve's own `steam_osx`,
     which it then execs unmodified.

3. **Quit Steam, then launch `Steam (macOS Play)`** instead of Steam.app. The gate is flipped
   in memory each launch, so this app is how you start Steam from now on.
   Check `/tmp/compat-enabler.log`: it should say `patched 1 site(s)`.

4. **Install and play a Windows title** from the normal Steam library UI. Steam downloads the
   Windows depot into your macOS `steamapps`, then hands the `.exe` to the compat tool, which
   launches it in the bottle against the shim.

The Steam overlay is **on by default**. To turn it off, reinstall with it disabled:

```sh
SHIM_OVERLAY=0 ./tools/installer/install.sh
```

### Uninstall

```sh
./tools/installer/install.sh --uninstall
```

Removes the launcher and the payload. Steam itself was never modified, so there is nothing
else to undo.

## Troubleshooting

Three logs cover almost everything:

| Log                       | What it tells you                                               |
| ------------------------- | --------------------------------------------------------------- |
| `/tmp/compat-enabler.log` | whether the compat gate was flipped                             |
| `/tmp/shim-launch.log`    | what the compat tool was invoked with, and how it launched      |
| `/tmp/shim_unix.log`      | every Steamworks call crossing the seam, per interface and slot |

Two failure modes account for most confusion:

- **Steam offline:** an offline client is a convincing false negative. Everything looks
  wired and nothing unlocks. Confirm you are signed in and online first.
- **A stray Windows Steam:** `ps aux | grep -i steam.exe` must be empty. If a Windows Steam
  is running in some bottle, it may be what answered, and the result proves nothing.

## Known limits

- **Sources only:** no release artifact, no signed app, no Gatekeeper story yet.
- Anti-cheat is out of scope.

## Related projects

The closest prior art, and the one this project is measured against, is
[**natbro/kaon**](https://github.com/natbro/kaon): kaon forces re-platforming _every_ app
globally and blocks the client's self-update; it merges the CrossOver bottle's Steam tree in
as a second library folder; And Steamworks is answered by **a real Windows Steam client running inside the same bottle**.

Those limitations were the reason this project came to be

Others that came up in the research:

- [Proton](https://github.com/ValveSoftware/Proton) and its `lsteamclient` — the Linux
  equivalent of Level B, and the source of the vtable layouts this shim generates from.
  ADR 0001 explains where the transports diverge.
- [Proton-mac-client](https://github.com/Toast-dev-wq/Proton-mac-client-) — an abandoned
  scaffold toward the same goal, kept as a record of a route not taken.
- [Millennium](https://github.com/SteamClientHomebrew/Millennium) and
  [Decky Loader](https://wiki.deckbrew.xyz/plugin-dev/cef-debugging) — Steam client modding
  through the CEF debug surface. That surface is a dev instrument here and never ships (ADR
  0002); kaon's issue #6 proposes it as a control path.
- [Steam-Play-None](https://github.com/Scrumplex/Steam-Play-None) and
  [steamtinkerlaunch's wiki](https://github.com/sonic2kk/steamtinkerlaunch/wiki/Steam-Compatibility-Tool)
  — the community reference for `compatibilitytool.vdf` and `toolmanifest.vdf` shape, since
  Valve documents neither.
- [CrossOver](https://www.codeweavers.com/crossover) — the Wine layer this builds on, and
  where Apple's Game Porting Toolkit reaches the game.
  [Sikarugir](https://github.com/Sikarugir-App/Sikarugir), formerly Kegworks, continues where
  Whisky left off, but is untested here.

## Repo layout

```
CONTEXT.md            the glossary — read this first
docs/adr/             the decisions, and why the alternatives were rejected
docs/research/        the measured evidence behind each decision
tools/installer/      install.sh — the one command that deploys everything
tools/compat-enabler/ the m_bCompatEnabled injector (Level A)
tools/compat-tool/    the compat tool + launch script (the Level A ↔ Level B seam)
tools/shim/           the bridge: PE steamclient(64).dll + native .so (Level B)
tools/overlay-inject/ gets Valve's overlay renderer into the game process
tools/harness/        Spacewar (480) achievement harness — the reference instrument
tools/*-probe/        the instruments used to measure the claims in docs/research
```

Every `tools/*/FINDINGS.md` is a record of what was measured live, on real hardware, with
the exact versions, including the negative controls. When something disagrees with this
README, the FINDINGS file is the one that was measured.
