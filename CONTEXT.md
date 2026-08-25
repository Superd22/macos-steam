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
