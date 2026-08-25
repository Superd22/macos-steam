# instruments/ — what gets rerun to re-verify a claim

**Admission rule:** a tool belongs here if you would run it again to re-establish a claim
the docs already make — after a CrossOver bump, a Steam client update, or a macOS release.

The distinction from `attic/` is *future tense*. An instrument's question is still live
because its answer can expire; an attic entry's question is closed and cannot.

| | measures |
|---|---|
| `harness/` | Spacewar (480) achievements through the shim — the acceptance test `src/shim/run.sh` drives |
| `overlay-probe/` | `d3dprobe` (does the overlay see D3DMetal's frames? #26), `inputprobe` + `input-parity-run.sh` (input parity under the overlay, #28) |
| `native-probe/` | `connprobe` — the honest native-side connection oracle (`Steam_BConnected`) |

Each carries the claim it measures and the negative control that makes the measurement
mean something. A run with no control is not a measurement.
