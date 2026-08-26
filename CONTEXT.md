# Context — macOS Steam Play (glossary)

The ubiquitous language for this effort. Terms only — no implementation detail, no
decisions (those live in `docs/adr/`). Update the moment a term is coined or sharpened.

## Terms

- **Bridge** — the whole mechanism that lets a Windows game running inside the CrossOver
  bottle reach the **native macOS Steam client**. Has two sides: the PE side inside the
  bottle and the native side outside it, joined by a **transport**.

- **Shim** — the PE half of the bridge: the `steamclient64.dll` the game's `steam_api64.dll`
  loads. It contains no Steam logic; it marshals calls across the **seam**. (Proton's
  equivalent is `lsteamclient`.) Not to be confused with Valve's real `steamclient64.dll`,
  which the shim replaces.

- **Seam** — the single boundary a call crosses to get from PE code to native code:
  `__wine_unix_call(handle, code, args)`. One function pointer and one integer index; no
  serialisation, no thread hop.

- **Transport** — how the seam is physically realised. The chosen one is the **unixlib**;
  the rejected alternative was a **native helper + IPC** (see ADR 0001).

- **Unixlib** — a Wine builtin DLL paired with a native `.so` on the unix side, the two
  halves talking over the seam in-process. The bridge's transport.

- **Native helper** — a separate native macOS process that would have held the
  `steamclient.dylib` connection and talked to the bottle over a loopback socket. Rejected
  transport; retained only as a documented fallback (ADR 0001).

- **Native side / native Steam client** — the real macOS Steam.app and its
  `steamclient.dylib`, running outside the bottle as an arm64 process. The bridge connects to
  it over Valve's own IPC; **no Windows Steam runs**. This "no Windows Steam" property is the
  whole point of the effort.

- **Ship-set** — the exact set of modules that reach a user's machine. It is a path, `src/`,
  not a judgement call: a beta cut, a CI target and the release payload all read the same
  directory. Widening or narrowing it is a release-surface change (ADR 0004).

- **Instrument** — a tool kept because it would be *rerun* to re-establish a claim the docs
  make, after a CrossOver, Steam client or macOS bump. Lives in `instruments/`. Distinct from
  an **attic** entry, whose question is closed and whose answer is written down elsewhere; the
  difference is tense, not code quality.

- **Deploy contract** — where every shipped artifact lands: the payload root, the in-bottle
  payload path, the log dir, and the basename of each file. One manifest owns it
  (`src/layout/layout.json`), and every module reads it as generated *names* rather than
  restating the path. A module that types one of those literals again is drift, and the
  build says so.

- **Payload** — the directory a deploy lays down: already built, self-describing, and the
  unit two halves of the install agree on. `build.sh` produces one from the repo and
  `deploy.sh` consumes one, which is what lets a brew formula or the launcher app deploy
  without a compiler or a checkout (ADR 0010). It carries its own `deploy.sh`, so an
  installed machine can verify, roll back and uninstall itself with no repo present.

- **Receipt** — what a deploy recorded: version, every file with its hash, the overlay
  setting, and the compatibility statement. The answer to "is my install intact and
  current", which used to be the user reading a log for `patched 1 site(s)` — a line that
  says the injector ran, not that the files on disk are the files that shipped.

- **Compatibility statement** — the combination a release was actually exercised against
  (macOS, CrossOver, Steam client, titles). Carried in the payload and copied into the
  receipt beside what the machine *observes*, so "works here" and "was measured here" stay
  distinguishable. The FINDINGS discipline, surfaced to a user instead of buried in docs.

- **Switch** — a runtime policy with exactly one owner: an env var, a default, and a
  generated predicate (`shim_overlay_enabled`) that every half of the stack calls instead
  of testing the variable itself. Declared beside the deploy contract in
  `src/layout/layout.json`. Asking a switch is reading policy; re-deriving it is drift, and
  the build says so (ADR 0006).

- **Veto** — a second input to a switch, owned by somebody else. The overlay switch has one:
  Steam's `SteamNoOverlayUI`, which the client sets per launch when the user unticks "Enable
  the Steam Overlay while in-game" globally or for that title. It can only turn the switch
  off, never on, so `SHIM_OVERLAY=0` stays a kill switch and Steam's answer stays a per-title
  opt-out. Because the launcher always states a literal `1` or `0`, `SHIM_OVERLAY=1` means
  "no objection from our side", not "force on" (ADR 0012).

- **Shape** — the unit the shim generates a thunk for: one (interface, method, *signature*),
  not one method and not one version. It is the unit a single C function can serve, because
  everything the seam needs — field widths, i386 arity, the return path — is a function of
  the signature alone. One shape typically covers many interface versions; the same method
  in two versions with different signatures is two shapes (ADR 0007).

- **Refusal** — the generator declining to emit a thunk, *with a named reason*, recorded in
  `src/shim/gen/REPORT.md`. A refusal is an outcome, never an omission: the slot keeps its
  logging stub, so a title that calls it still says so in `shim-unix.log`. The thing a
  refusal exists to prevent is a silent `0` — a plausible answer a title builds on, which is
  how a complete cloud save stayed invisible for a whole session.

- **x86_64-only shape** — a **shape** the generator emits for the 64-bit build alone,
  because its correctness depends on pointers being 8 bytes wide on both sides of the
  seam (an array of pointers, or a struct whose layouts agree on x86_64 and diverge on
  i386). On the 32-bit build the slot keeps its logging stub, so coverage is deliberately
  bitness-dependent and a 32-bit title meets a named line rather than a wrong answer
  (ADR 0008).

- **Override** — a method the generator is told not to emit even though it could, declared in
  `src/shim/overrides.json` with its reason. Two kinds: *hand-written* (a thunk already
  serves it, and the build checks that claim both ways) and *semantic* (forwarding it would
  be correct code and wrong behaviour — `IsOverlayEnabled` is the standing example). An
  override is a list the generator reads, never a patch to what it wrote.

- **Level A** — making the native macOS Steam client *install and launch* a Windows title
  through a compatibility tool (the download + launch half of the destination).

- **Level B** — making a launched Windows game's Steamworks calls reach the native client
  through the **bridge** (the achievement half of the destination).

- **Compat gate** — the single latched boolean (`m_bCompatEnabled`) in the macOS client that
  turns the whole Steam Play subsystem on. Off by default on macOS; crossable with a one-byte
  patch. The subject of Level A.

- **Bottle** — a CrossOver Wine prefix. A **clean bottle** is one with no Windows Steam
  installed, required so the shim is tested without the real `steamclient64.dll` winning the
  lookup.

- **Overlay renderer** — Valve's `gameoverlayrenderer.dylib`, shipped inside macOS Steam.app.
  The thing that actually draws the overlay, by swizzling `CAMetalDrawable` / `MTLCommandBuffer`.
  We do not write one; we load theirs into the Wine process (ADR 0003).

- **Loaded vs armed** — two different states of the overlay renderer, and conflating them is
  the failure this vocabulary exists to prevent. **Loaded** means the dylib is in the process
  and its hooks are installed. **Armed** means the native client has additionally completed
  its handshake with it, so a panel can actually appear. A renderer can be loaded and never
  arm. `ISteamUtils::IsOverlayEnabled()` answers **armed**, never loaded — a title told
  "yes" pauses and waits for a panel, so answering it from the wrong state is a hang.

- **`SteamNoOverlayUI` vs `SteamNoOverlayUIDrawing`** — two Valve variables one suffix apart
  that answer different questions, and the likeliest thing to get wrong in this area.
  `...UIDrawing` is read by the **renderer**: it means "do not draw", and our launch path
  sets or unsets it to arm the renderer we loaded. `SteamNoOverlayUI` is read by the
  **client**, and Steam hands it to the compat tool as `1` when the user has the overlay
  disabled for this launch — it is an *input* telling us what the user chose, not an output
  we set. Measured live: it appears on the next game launch after the box is unticked, with
  no Steam restart (ADR 0012). The drift guard matches whole names so the two never collide.
