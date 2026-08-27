# `src/drm/` — the DRM route

A DRM-wrapped title is launched with **Valve's own signed `steamclient64.dll`**, and our shim is
reached through trampolines written into it. Why it has to be that way, what it costs and where it
stops: **[ADR 0014](../../docs/adr/0014-drm-titles-run-through-valves-own-signed-client-dll.md)**.
The measurement behind it: `docs/research/steam-drm-shared-memory.md`.

| | |
|---|---|
| `fetch.sh` | gets Valve's DLL from Valve's public client manifest and verifies it |
| `shadow_tier0.c` | the hook — installs the trampolines |
| `shadow_vstdlib.c` | inert; needed only because its partner is shadowed |
| `gen_shadow.py` · `check_shadow.py` | generate the shadows' export lists, and fail the fetch if a client build outgrows them |
| `gen/` | those lists, committed, so the build stays offline |

```sh
./fetch.sh              # once, and again after a Steam client update
./build.sh              # offline
./build.sh --regen      # re-read Valve's libraries first
```

`SHIM_DRM=0` turns the route off. Without the fetch it is unavailable and the launch says so.
Only titles that are actually wrapped take it.
