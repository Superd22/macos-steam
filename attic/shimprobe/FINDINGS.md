# Findings — see `docs/research/clean-bottle-provenance.md`

This module has no findings of its own. What `shimprobe` established — that in a bottle with no
Windows Steam, the game's `steam_api64.dll` really does load *our* `steamclient64.dll` at the path
the registry names, and that deleting it flips the run back to `SteamAPI_Init()=0` — is written up
in [`docs/research/clean-bottle-provenance.md`](../../docs/research/clean-bottle-provenance.md),
together with the bottle recipe and the baseline registry state it was measured against.

This stub exists so the pointer sits where a reader looks for it. Every other module in
`instruments/` and `attic/` keeps its conclusions in a `FINDINGS.md` beside the code;
`shimprobe`'s belong to a research doc because they are a claim about Wine's loader, not about
this code.
