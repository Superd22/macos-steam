# 14. DRM-wrapped titles run through Valve's own signed client DLL

Date: 2026-08-27

## Status

Accepted

Closes #97. Extends [ADR 0013](0013-a-presence-stub-may-represent-the-native-client-in-the-bottle.md),
whose refusal it does not touch. Deliberately does **not** use the injector of
[ADR 0003](0003-overlay-injection.md).

## Context

Steam's DRM wrapper does not use the Steamworks API. Measured in
`docs/research/steam-drm-shared-memory.md`: it resolves `CreateInterface`, traces it back to the
file that provided it, reads that file, and **verifies an RSA-1024 signature over it** against three
public keys built into the title. It wants a `"VLV\0"` block at DOS-header offset `0x40` — the
format Valve's own `steam_api64.dll` carries. Failing that check yields
`Application load error 3:0000065432`.

Our shim cannot carry that signature, and not merely because we lack Valve's key: `winebuild` writes
`"Wine builtin DLL"` at **offset `0x40`**, the same bytes. **A PE is either a Wine builtin or
Valve-signed.** Being a builtin is how the shim reaches its unix half at all (ADR 0001), so the
choice is not open to us. Proton's `lsteamclient` is an ordinary builtin too and would fail the same
check.

Proton passes it by patching its own loader: `build_module` special-cases a module basenamed
`steamclient64`, loads `lsteamclient.dll` beside it, rewrites the real DLL's exports into jumps into
the shim, and sets `LDR_DONT_CALL_DLLMAIN`. The module the wrapper inspects is Valve's genuine,
signed DLL, whose file on disk is untouched. We do not ship a Wine, so that exact site is not
available to us.

Three things were built and measured before this, and none of them is the answer:
`ActiveProcess\pid`; Proton's complete `steam_helper` presence surface with the stub as the title's
parent; and the full 57-ordinal export table. The `Local\SteamStart_SharedMem*` handshake turned out
to be the wrapper's **error-reporting channel** — answering it removes the dialog and changes
nothing else.

## Decision

**A DRM-wrapped title is launched with Valve's own signed `steamclient64.dll` at the path
`SteamClientDll64` names, and our shim is reached through trampolines written into that module's
exports in memory.**

Three parts:

1. **Valve's DLL is fetched from Valve.** `src/drm/fetch.sh` reads Valve's public, unauthenticated
   Windows client manifest — the same endpoint `steamcmd` bootstraps from — downloads the package,
   checks it against the SHA-256 Valve publishes, and caches the DLL per machine. We redistribute
   nothing; the bytes go from Valve to the user.
2. **Our code gets in front of it through the search path, not through injection.** The wrapper
   loads it with `LOAD_WITH_ALTERED_SEARCH_PATH`, so its own directory is searched first for its two
   non-system imports. We lay that directory down, so our `tier0_s64.dll` shadow initialises while
   the signed DLL is mapped but before its entry point runs. It rewrites the exports and overwrites
   the entry point with `mov eax,1 ; ret`.
3. **Only wrapped titles take it.** Steam's wrapper lives in a `.bind` section that is present in
   the file on disk, so the section table answers "is this title wrapped" exactly, without running
   anything.

### Why this is not forging

The rule ADR 0013 set is that the stub **forwards or reflects, it never decides**. The wrapper's
question is *"is the client DLL on disk the genuine Valve one"*, and the answer we let it read is
**yes, truthfully** — that file is Valve's, byte for byte, unmodified. We forge no signature, and we
do not patch the check out of the title's own binary. What we redirect is where the *calls* go, in
our own process, which is what this project has always done. It is the same shape as ADR 0003, where
we load Valve's own overlay renderer rather than imitating one.

### Why not the injector

ADR 0003's injector creates the title suspended and rewrites its imports from outside. That is
exactly what anti-tamper rejects — AoE IV's Aegis refuses to start with it, and the title that
motivated this ships EasyAntiCheat. The route chosen here is **in-process, at loader time**: no
suspended process, no `CreateRemoteThread`, and the title's own imports are never touched. Proton's
version lives in ntdll for the same reason: it is a loader-time job, not an injection job.

## Consequences

**A new module, and a new kind of payload.** `src/drm/` joins the ship-set (ADR 0004) and the deploy
contract (ADR 0005). It is the first component whose operation depends on a file we do **not** ship
and cannot: the launch falls back to the normal route when the fetch has not run, and says so.

**Two shadows, generated.** Valve's `steamclient64.dll` imports 165 names from `tier0_s64.dll` and
130 from `vstdlib_s64.dll`; every one must exist as an export or the loader will not bind it. The
lists are generated from the reference libraries' **exports** (542 and 276) rather than those
imports, so they are a superset and survive a client update that imports one more. `build.sh --regen`
regenerates.

The guard against losing that coverage runs at **fetch** time, not build time: that is the only
moment the client build the user will actually run is on disk, and a green build on someone else's
machine says nothing about theirs. The failure it prevents is silent — the loader simply refuses to
bind and the title dies with the same DRM dialog as if none of this existed — so `check_shadow.py`
ships beside `fetch.sh` and reads the built shadows' own export tables. Both shadows are inert —
once the entry point is neutered, no Valve code runs in the process at all.

**The shim ships under a second name.** Two modules cannot share a basename, and Valve's DLL must
keep `steamclient64.dll`. The shim is therefore **built** as `lsteamclient.dll`, not renamed: ntdll
derives a builtin's unix half from the PE's internal name, so a copy binds no seam and answers every
call from a PE with no unix half.

**Valve's DLL is deployed under a name of ours.** Wine resolves a builtin by basename off the DLL
path, which includes the shim dir — so under its own basename, ours loads in its place, silently,
and the wrapper rejects the unsigned file it finds. That is the one way this route fails while
looking like it worked. Renaming Valve's copy costs nothing, because the wrapper resolves whatever
provided `CreateInterface` and reads *that file's bytes*; the name never enters into it. The
alternative — removing our own PE from the bottle for the duration — was rejected: it is a
bottle-global mutation with no owner, and a concurrent launch replanting the file mid-load would
reintroduce exactly the failure it was meant to prevent.

**This route and the overlay injector are mutually exclusive, and the launch script enforces it.**
The injector makes our PE the title's *first static import* (ADR 0003); on this route the shim is
reached through Valve's DLL instead, and the two cannot both own that slot. So a wrapped title
launches with the overlay off, and the log says which input said no. This is not merely a
concession: the injector is what anti-tamper titles reject, and wrapped titles are exactly where
anti-tamper lives.

**`SteamClientDll64` is bottle-global and the choice is now per-title.** Steam's own `run`-verb
helpers are excluded — they no longer rewrite it, because a helper does not own that choice and
stomping it mid-startup would fail a wrapped title. What remains is two *titles* running in one
bottle at once, one wrapped and one not: the second launch flips the value under the first. That is
a property of sharing a prefix rather than of this route, and the mitigation is the one that already
exists — `SHIM_BOTTLE` names the bottle, so give them different ones.

**34 exports currently answer zero.** Valve's DLL exports 41 names; our shim exports 7. The rest get
a named thunk that logs which one was called and returns zero. That is a wrong answer, and #98 is
where it gets fixed — but it is a *loud* wrong answer, which is the standard #45 set.

**64-bit only, checked rather than assumed.** The shadows, the second shim name and the trampolines
are all 64-bit, so the detection reads the PE's `Machine` field as well as its sections: a 32-bit
wrapped title fails as it does today rather than losing the working 32-bit route on top.

**It is pinned to two things that move.** A Steam client update changes the DLL (re-fetch; the
manifest SHA-256 gates it) and could in principle change the import set (`check_shadow.py`). A title
update re-applies the wrapper, and the `.bind` layout could change. Both are named in the research
doc's `re-verify-on`.

## Amendment, 2026-09-05: the question is asked of the *title*, not of the file Steam names (#104)

The decision stands. What did not hold is the assumption inside part 3 — that the file Steam hands
the compat tool is the file to read the section table of.

Warhammer 40,000: Space Marine II is launched through a 32-bit outer launcher with no `.bind`
section at all; two of its eight EXEs are wrapped, and the one the game actually runs is four
directories down. So the route never armed through Steam, and neither did the warning, because both
asked the same question about the same wrong file. #97's proof launched the wrapped binary directly,
where `$EXE` *was* the wrapped file — true, and not measured through this path.

Two changes, both in `steamclient-shim-launch.sh`:

- **The arming signal is now "does this title ship a wrapped binary"** — a capped, scoped sweep of
  the title's install directory (`STEAM_COMPAT_INSTALL_PATH`) for any EXE carrying `.bind`, with the
  launcher's `ApplicationPath` ini followed first as a *refinement* that nothing depends on. This
  widens "only wrapped titles take it" from a file to a title: a title shipping a wrapped EXE it
  does not launch will arm the route and lose its overlay. Over-arming is visible in the log and
  recoverable; under-arming is a title that cannot start at all.
- **The bitness gate moved out of the wrapped test to its call site.** "Is this file wrapped" is a
  fact about bytes; "can our route help it" is a fact about the route. Merged, they made a 32-bit
  launcher fronting a wrapped 64-bit title indistinguishable from an ordinary unwrapped 32-bit one,
  and "wrapped, but we cannot help it" unsayable. The 64-bit-only consequence above is unchanged;
  it is now stated to the user instead of returning silently.

Both are interim. #107 removes the prediction entirely by arming the route once per bottle the way
Proton does, at which point the sweep is deleted and the split questions survive as diagnostics.

## Amendment, 2026-09-05: a wrapped title keeps its overlay after all (#112)

The decision stands. The consequence that does not is this one:

> So a wrapped title launches with the overlay off, and the log says which input said no.

**That exclusivity is the *injector's*, not the overlay's.** It was stated as a property of this
route because every arrangement measured on #107 confirmed it, and the reason offered — the route
reaches the shim after `user32`/`winemac` are up, so Valve's renderer is always late — is a real
observation attached to a wrong mechanism. `docs/research/drm-overlay-late-arming.md` (#111) read
the renderer's own code: it installs its five Metal hooks **only from a swizzled
`-[NSApplication init]`**, behind a once-guard. Arriving after `NSApplication` has been init'd does
not lose a race — it misses a method call, and a method call can be made by hand.

**So it is made by hand.** The DRM route already has our unixlib in the process, and it already
`dlopen`s the renderer; `overlay_arm_late()` in `shim_unix.cpp` now looks
`-[NSApplication init]` / `-[NSApplication steammetalhook_init]` up through the ObjC runtime, takes
whichever IMP `dladdr` places inside `gameoverlayrenderer.dylib`, and calls it with `self = nil`.
By name rather than by offset, because the address moves with every Steam client and the selectors
have not. It runs **only** when `NSApp` is already non-nil — i.e. exactly when Valve's own trigger
can no longer fire — so the injector path is bit-for-bit unchanged.

Measured 2026-09-05 on Space Marine II (2183900), launched as the maintainer launches it here (the
wrapped `… - Retail.exe` directly through `wineloader`, bypassing `start_protected_game.exe`),
CrossOver 26.3.0.39832, macOS 26.4, on a rotated `shim-unix.log`:

| run | `SHIM_OVERLAY` | `Hooking` | past the wrapper |
|---|---|---|---|
| pid 2904 | 1 | **5** (`winemac` present) | yes — `SteamClient020`, `GetAppID() -> 2183900`, `GetCurrentGameLanguage()`, SteamInput, the callback pump |
| pid 5220 | 0 | **no renderer log at all** | yes — same title-level traffic |

and in the bottle on `d3dprobe` driven into the same late-load state, the A/B that isolates the
call itself: this unixlib **5**, the `main` unixlib **0**, `winemac` present in both.

**What is unchanged.** The route and the *injector* still do not work together — #107's spikes D and
F both end in `Application load error 3:0000065432` — so the `USE_DRM = 0` interlock on the injector
call site stays exactly where it is. What moved is the *overlay env* interlock, which used to ask
"is there an injector" and now asks "is there a way to deliver a renderer", of which there are two.
Steam's per-title veto (ADR 0012) and the one-predicate rule (ADR 0006) are untouched: a wrapped
title whose user unticked the box still gets `SHIM_OVERLAY=0` and no `dlopen`, which is the
`Hooking`-less run above.

**What is still not measured.** Anti-tamper tolerance: the run above bypasses EasyAntiCheat, as
every SM2 measurement in this repo does. Input parity on the late path — `input-parity-run.sh`
needs a human at the keyboard, and so does the hotkey itself, so "the hooks are installed and the
renderer is tracking the drawable every frame" is what was shown, not "the overlay drew".
