# src/ — what reaches a user's machine

**Admission rule:** a module belongs here if removing it breaks a user's install.

That is the whole rule, and it is deliberately narrow. Everything under this root is
release surface: it is what a beta cut tars up, what CI should build and gate, and what
`installer/build.sh` turns into a payload and `installer/deploy.sh` lays down. Adding a directory here widens the release surface;
removing one is a release-surface change and wants a note in the ADRs.

| | |
|---|---|
| `layout/` | the deploy contract (#32) — one manifest of every shipped path, emitted as a C header and an sh fragment |
| `installer/` | the build/deploy seam (ADR 0010) — `build.sh` repo→payload, `deploy.sh` payload→receipt, `install.sh` the from-source adapter over both |
| `compat-enabler/` | the `m_bCompatEnabled` one-byte patch injected into the macOS client (Level A) |
| `compat-tool/` | the two vdf templates + the launch script (the Level A ↔ Level B seam) |
| `shim/` | the bridge — PE `steamclient(64).dll` plus the native `.so` (Level B) |
| `overlay-inject/` | gets Valve's overlay renderer into the game process before the Mac driver loads |

Nothing here may depend on `instruments/` or `attic/` at build or run time. The one place
the roots touch is `shim/run.sh`, which drives `instruments/harness` as its acceptance
test — a test-time dependency, not a shipped one.
