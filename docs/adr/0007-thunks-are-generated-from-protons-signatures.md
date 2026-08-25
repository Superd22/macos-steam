# 7. Thunks are generated from Proton's signatures

Date: 2026-08-25

## Status

Accepted

## Context

The shim presents Valve's interfaces to a Windows title and forwards each call across the
seam. Doing that for one method means five artifacts: a params struct, an opcode, a PE
thunk, a unix handler, a dispatch-table entry. Roughly ninety methods had them, all
hand-typed, and #47–#77 planned the remaining ~1,270 the same way.

The plan was to retype information we already fetch and parse. `extract_vtables.py` has
read Proton's `lsteamclient` since #20 for two facts per method — MSVC slot order, and the
i386 `DEFINE_THISCALL_WRAPPER` byte count that makes a callee-cleanup stub safe. The third
fact sits on the very next line of the same file:

```c
int32_t __thiscall winISteamRemoteStorage_..._VERSION016_FileRead(
        struct w_iface *_this, const char *pchFile, void *pvData, int32_t cubDataToRead)
```

Every one of the five artifacts is a pure function of that signature. The extractor was
parsing past it and throwing it away.

Two properties make it more generatable than expected, and both were measured rather than
assumed. Proton has **already normalised the sret problem**: `CSteamID` returned by value
appears as `CSteamID *` with a hidden `_ret` first parameter, which is exactly the MSVC
convention that cost the most time in #11 — the fix arrives encoded in the signature. And
by-value `CSteamID`/`CGameID` parameters are one `uint64` under `pack(1)`, which
`steam_ifaces.h` already asserts and relies on: a type mapping, not a hazard.

## Decision

**`gen_thunks.py` emits all five artifacts per shape, and refuses by name what it cannot.**

- **The unit is (interface, method, signature)** — 1,269 of them across 6,555 vtable
  entries — because that is the unit one C function can serve. Wiring is still per
  (version, slot), resolved by name at generation time against each version's own table
  and emitted as a direct assignment. That is *stricter* than `wire_all`'s runtime byte
  match, which would hand one thunk to two versions whose arity coincides but whose
  signatures differ.

- **One type map, stated once.** Pointers to `uint64_t` (a PE address the game owns,
  zero-extending into a field the native side reads through in place),
  `CSteamID`/`CGameID` to `uint64_t`, scalars to their fixed-width equivalent. Returns take
  three shapes the signature already distinguishes: `const char *` takes the `native_str`
  copy-down on i386 (#20), `CSteamID *` takes the sret pattern (#11), everything else comes
  back as a value.

- **The unix half dispatches by SLOT, not by class cast** — the ISteamFriends pattern from
  #23, generalised. A class cast needs every slot up to the deepest method transcribed by
  hand so the compiler emits the right index; 39 interfaces across 212 versions is not
  transcribable, and the transcription is where the risk lives. `steam_ifaces.h` therefore
  does not grow at all.

- **Refusals are loud and named.** Every method the emitter declines lands in
  `gen/REPORT.md` with its reason, and the declined slot keeps its #45 logging stub, so a
  title that calls one still names it in `shim-unix.log`. A silent skip would recreate #43
  at scale — a plausible `0` nobody can attribute.

- **Semantic cases live in `overrides.json`**, a declarative list the generator reads,
  deliberately not a patch applied to generated output. A patch would be overwritten on the
  next run and the reason would live nowhere. `IsOverlayEnabled` is the standing example:
  blind forwarding is correct code and wrong behaviour.

- **`src/shim/gen/` is not checked in** (`REPORT.md` excepted), and `build.sh` re-derives it
  from `vtables.json` on every build rather than behind `--regen-vtables`. Drift between
  the generated code and the data it came from is then not a state the tree can be in.

### The assumption, and how it is tested

Dispatching by slot is only correct where the MSVC order the PE half holds and the dylib's
own Itanium order agree. They do not agree in general: MSVC lays a set of same-name
overloads out in **reverse** declaration order. The reversal is confined to a contiguous
run, so every method outside one keeps its index — which is what makes the slot
transferable, and makes overload-set members the one class that must be refused.

That refusal nearly did not happen. Overload sets do not appear as duplicate names, because
Proton has already disambiguated them: the SDK's two `GetStat` overloads become `GetStat`
and `GetStat_2`, and the vtable lists `_2` first. Counting duplicate names finds **zero**
across all 212 versions and would have let all 140 of them through, each dispatching to its
own sibling — `GetStat(int32*)` landing on `GetStat(float*)`. Detection is by base name with
a trailing `_n` stripped, and the generator additionally asserts each set is contiguous,
exiting rather than generating 6,000 slots on a premise that just failed.

`check_slot_transfer.py` then tests the assumption against independent evidence.
`steam_ifaces.h` holds classes a human transcribed from Valve's SDK headers over #11/#20/#43
in declaration order — the Itanium order, arrived at by reading the SDK. `vtables.json`
holds the MSVC order, arrived at by clang compiling the same SDK for Windows. Two readings
of one source by two routes, lined up index by index: 151 slots across 8 interfaces agree
exactly, and every name that differs is inside a refused overload set.

## Alternatives rejected

**Hand-write #47–#77 as planned.** ~1,270 methods x five artifacts, each a chance to
mistype a width. The information was already in the tree; the only thing being added by
hand was the opportunity for error.

**Derive the bodies from Proton too.** Out of scope, and a different decision: this ticket
takes interface *facts* from Proton's sources, not implementation. #78 says so explicitly.

**Declare 39 C++ classes in `steam_ifaces.h` and keep the class-cast pattern.** This is the
transcription problem restated. It also gets *harder* with scale, because every slot before
the one you want must be right, and nothing checks it.

**Patch the generated output for the semantic cases.** The reason would live in a diff
against a file regenerated on every build.

## Consequences

- 4,873 of 6,555 vtable slots now reach the native client through a generated thunk, on
  top of the ~1,210 slot entries the 98 hand-written opcodes already claimed. 1,071 shapes
  emitted, 265 params structs, 216 methods refused with a stated reason. `C_COUNT` goes
  from 98 to 1,169; both dispatch tables grow to match, and the `static_assert` that keeps
  them in step with the enum already existed.
- The build gates became the generator's test suite, and neither trusts it.
  `check_abi_layout.py` recompiles every generated params struct under i686 and x86_64 and
  compares 2,256 offsets — a type mapped to the wrong width fails the build.
  `verify_abi.py` disassembles the i386 object and re-derives each thunk's `ret N` from the
  binary, comparing it against Proton's own `DEFINE_THISCALL_WRAPPER` count: 7,725 entry
  points, 1,071 of them generated. Two numbers from different halves of Proton's source,
  meeting in a disassembly.
- `check_overrides.py` keeps `overrides.json`'s hand-written claims honest in both
  directions. A stale entry means the generator skips a method nothing serves — #43's
  silent `0`, reintroduced by a list rather than by code.
- Each generated handler logs its **first** call and never again. Per-call logging at this
  scale would bury the log it belongs in; silence is the failure mode this shim keeps
  relearning. A run is now a record of which of ~1,100 methods a title actually exercised.
- Verified live, not only at build time: the #7 harness through the shim in bottle
  `shim-clean` against the running macOS client reads all five achievements unchanged, and
  two generated thunks — `ISteamController::RunFrame` and
  `ISteamClient::BShutdownIfAllPipesClosed`, both logging stubs before this — fired and
  returned correctly.
- The residue is now measured instead of hypothesised, and #78's starting table was wrong
  in both directions. Function-pointer parameters and by-value aggregates are real, as
  predicted. It missed aggregate *returns* (`InputMotionData_t`, `SteamIPAddress_t`),
  pointer-to-pointer parameters (whose pointees are 4 bytes wide in a 32-bit PE and 8 on the
  native side), parameters pointing at Proton's own `w_`-prefixed structs (its marker for a
  layout needing conversion, which we have no converter for), and 25 methods Proton
  hand-writes with no one-line signature to read. It also over-predicted: several methods it
  listed generate cleanly.
- The PE binaries roughly double in size (1.6 MB / 1.4 MB). `struct w_iface` gained a cached
  version descriptor so `native_slot()` does not rescan 212 descriptors per call.
