# src/ — what reaches a user's machine

**Admission rule:** a module belongs here if removing it breaks a user's install.

That is the whole rule, and it is deliberately narrow. Everything under this root is
release surface: it is what a beta cut tars up, what CI should build and gate, and what
`installer/install.sh` deploys. Adding a directory here widens the release surface;
removing one is a release-surface change and wants a note in the ADRs.

| | |
|---|---|
| `installer/` | `install.sh` — builds what is stale, deploys the payload, writes the launcher |
| `compat-enabler/` | the `m_bCompatEnabled` one-byte patch injected into the macOS client (Level A) |
| `compat-tool/` | `compatibilitytool.vdf` + `toolmanifest.vdf` + the launch script (the Level A ↔ Level B seam) |
| `shim/` | the bridge — PE `steamclient(64).dll` plus the native `.so` (Level B) |
| `overlay-inject/` | gets Valve's overlay renderer into the game process before the Mac driver loads |

Nothing here may depend on `instruments/` or `attic/` at build or run time. The one place
the roots touch is `shim/run.sh`, which drives `instruments/harness` as its acceptance
test — a test-time dependency, not a shipped one.
