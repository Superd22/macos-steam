# attic/ — questions that are closed

**Admission rule:** the question this code answered is settled, the answer is written down
somewhere else, and nothing here is rerun.

Kept, not deleted: these are the working instruments behind claims the ADRs and research
docs now assert flatly. When someone doubts one of those claims, this is the evidence.
Nothing in `src/` or `instruments/` may depend on anything here.

| | question, and where its answer now lives |
|---|---|
| `seam-spike/` | can a PE call native code across one `__wine_unix_call`? Superseded wholesale by `src/shim`; **ADR 0001** holds the conclusion. |
| `shimprobe/` | does a decoy `steamclient64.dll` actually get loaded in a clean bottle? Answer in `docs/research/clean-bottle-provenance.md` — this module has no `FINDINGS.md` of its own. |
| `overlay-probe/` | is Valve's macOS overlay reachable from a process we control, and what gates it? **ADR 0003**; detail in `docs/research/steam-overlay-feasibility.md` Addendum 2. |
| `native-probe/` | `probe`, `machprobe`, `interpose` — the #2 go/no-go probes. Verdict in `instruments/native-probe/FINDINGS.md`, which stays with `connprobe` because that claim still gets re-verified. |

One thing here is not purely historical: `seam-spike/bridge_pe.c` holds the **only latency
instrumentation in the repo** (~40 ns bare seam, ~16–40 µs `GetPersonaName`). `src/shim`
has none. If seam cost ever needs re-measuring, extract that QPC loop rather than writing a
new one.
