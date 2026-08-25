# shimprobe (archive) — the clean-bottle decoy

A deliberately dumb `steamclient64.dll` that does nothing but log its `DllMain` attach and
every `CreateInterface` it is asked for. It answered one question: in a bottle with no
Windows Steam installed, does the game's `steam_api64.dll` really load *our* file at the
path the registry names?

It does. **This module has no `FINDINGS.md` — its conclusions live in
`docs/research/clean-bottle-provenance.md`.**

Superseded by `src/shim`, which is the real thing at the same load point. Not rerun; kept
because the negative control it established (delete the dll, the run must flip back to
`SteamAPI_Init()=0`) is still the provenance check `src/shim/run.sh` documents.
