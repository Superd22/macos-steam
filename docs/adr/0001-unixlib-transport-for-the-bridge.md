# 1. The bridge crosses the Wine boundary via a unixlib, not a native helper + IPC

Date: 2026-08-13

## Status

Accepted

## Context

The bridge that lets a Windows game inside the CrossOver bottle talk to the native
macOS Steam client has to get a call from PE code, across the Wine boundary, to native
x86_64 code that holds the `steamclient.dylib` connection. Two shapes were on the table
(issue #8):

- **Unixlib** — what Proton does: a Wine builtin DLL paired with an `.so` on the unix
  side, talking over Wine's `__wine_unix_call` seam. In-process, lowest latency, but was
  feared to require rebuilding Wine and to couple us to a specific CrossOver build.
- **Native helper + IPC** — a plain PE DLL talking over a loopback socket to a separate
  native macOS process. Backend-agnostic and update-proof, but pays per-call IPC cost and
  adds a process to manage.

Prior research had already removed the helper's supposed advantages:

- **No architecture boundary forces a separate process** (#4): CrossOver's whole Wine unix
  side is x86_64 under Rosetta, and an x86_64 process `dlopen`s the universal
  `steamclient.dylib` and connects (#2). arm64 buys nothing for connecting.
- **The callback direction is a poll, not a push** (#3 §6): `Steam_BGetCallback` is called
  by the game's own `steam_api64.dll` from `SteamAPI_RunCallbacks`, so both transports face
  only outbound calls plus a game-driven pump — no native→game push thread exists on the
  achievement path.
- **Latency does not discriminate**: the achievement path is a handful of calls plus a
  once-per-frame pump; even loopback TCP's measured ~18 µs RTT (#4) would be fine.

The one open question was a fact, not a preference: does CrossOver's builtin loader impose
any gate beyond the `"Wine builtin DLL"` marker that would stop a third-party unixlib from
loading? That was settled by building the seam spike (`tools/seam-spike/`, stage 1 of #10)
rather than by argument.

## Decision

**The bridge uses a Wine unixlib as its sole v1 transport.** A marker-stamped PE builtin
(`steamclient64.dll`) is paired with an unsigned x86_64 `.so` that hosts `steamclient.dylib`
in-process; calls cross via `__wine_unix_call`. No Wine rebuild, no separate helper process,
no IPC on the hot path.

The spike proved this works on the shipped CrossOver 25.1.1 with an unsigned `.so`: the PE
loads **as a builtin** on the `0x40` marker alone (no allowlist or signature gate), the
unix entry runs in the same process, and a magic value round-trips. Details and the
reproduce recipe are in `tools/seam-spike/FINDINGS.md`.

The native-helper + IPC option is **rejected**, and recorded as the documented fallback if —
and only if — a later stage hits one of two triggers:

1. A future CrossOver changes the loader to gate third-party builtins beyond the marker, or
2. `steamclient.dylib` proves unable to run inside the Wine process specifically (symbol or
   CoreFoundation-runtime collisions, thread/run-loop requirements a Wine process can't meet)
   — which is exactly what stage 2 of #10 exists to find out.

Loopback TCP (#4's control channel) is dropped from v1: its job was bootstrapping a helper
process, and there is no helper. It survives only as the fallback transport under the
triggers above.

## Consequences

- **Deployment layout is fixed by how CrossOver's loader resolves the pair** (see FINDINGS):
  the builtin PE must exist as a real marked file where the PE loader already searches (the
  game dir, or the `SteamClientDll64` registry path); the `.so` is discovered via
  `WINEDLLPATH`/`cxbottle.conf [Wine] "DllPath"` as `<entry>/<name>.so`, beside the PE on the
  DllPath — **not** in a sibling `x86_64-unix/` directory as upstream Wine docs suggest. A bare
  `LoadLibrary` of an unregistered name never probes `WINEDLLPATH`.
- **We accept coupling to Wine's unixlib ABI.** A CrossOver major bump could change it; the
  cost is recompiling one `.so`, gated by a runtime `wineloader --version` check that fails
  loudly on mismatch. This is cheaper than permanently carrying a second notarized binary and
  a process lifecycle, which the helper would have cost on every release regardless.
- **The target arch stays a build-time switch, not a hardcode.** Today the `.so` must be
  x86_64 (the Wine unix side is x86_64 under Rosetta). If CodeWeavers ships an `aarch64-unix`
  host — its launcher already probes for one — we rebuild the `.so` as arm64 and nothing else
  changes. This keeps the design off the far side of Rosetta 2's deprecation clock (#4 §6).
- **The seam spike de-risks #10.** With the transport proven, #10 no longer has to prove the
  boundary is crossable at all — only that the real `steamclient.dylib` behaves inside the
  Wine process.
