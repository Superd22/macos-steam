# 8. Struct layouts come from Proton too, and a thunk may be one bitness only

Date: 2026-08-26

## Status

Accepted

## Context

ADR 0007 stopped retyping the *signatures* Proton already states. The residue it
left was 71 methods, and the two largest classes were both recorded as needing
work the seam deliberately avoids:

> `w_` struct param (20) — needs a Windows↔native layout **converter**, the
> machinery ADR 0001's unixlib transport exists to avoid
>
> aggregate return (7) / aggregate param (4) — needs the struct declared to know
> its size, transcription, the thing #78 removed

Both readings were wrong, in the same way #78's own residue table was wrong.
Proton publishes `steamclient_structs_generated.h` — 1,473 struct definitions
with explicit `#pragma pack` and explicit `__pad_N[]` members — and
`steamclient_structs.h` for the opaque byte-blob typedefs. `build.sh` was
fetching neither: it greps the tree for `^winISteam.*\.c$`.

More usefully, that header already answers the only question our seam asks. A
struct appears either as one plain definition, meaning Proton determined the two
layouts agree, or as a `w64_`/`u64_`/`w32_`/`u32_` family, meaning they differ
and here is exactly how. Within a family it distinguishes further:

```c
typedef struct w64_SteamParamStringArray_t u64_SteamParamStringArray_t;
```

That is Proton stating that **on x86_64 the unix layout IS the Windows layout**,
and only the 32-bit forms diverge. Measured across the 121 families: 35 are
identical on x86_64, 86 genuinely differ. Our seam is x86_64 on both sides, so
for the first group there is nothing to convert — the address crosses and the
native side reads it in place, like every other pointer.

The same correction applies to the pointer-to-pointer class. An array of
pointers crosses verbatim when both sides hold 8-byte pointers; it is only a
32-bit PE, whose pointees are 4 bytes wide, that cannot read it in place. That
was an i386 problem being charged to both bitnesses.

## Decision

**`extract_structs.py` reads Proton's layouts, and a shape may be generated for
one bitness only.**

- **The struct model is Proton's verdict, not ours.** Each base name is
  classified `plain`, `opaque`, `x64-identical` or `x64-differs` straight from
  the shape of Proton's own declarations. Nothing here re-derives a layout.

- **Bodies are captured verbatim.** Re-modelling a struct into a field list and
  re-emitting it would put our arithmetic between Proton's declaration and the
  compiler — one more place to be wrong about padding. The C++ sections are
  stripped and `W64_PTR` is resolved to its x86_64 expansion (which is the
  declaration unchanged); everything else is Proton's text.

- **By-value aggregates cross as the address of the caller's copy**, and the
  emitted type is what lets the compiler make the native call. That is the point
  of emitting the real struct rather than a sized blob:
  `InputDigitalActionData_t` is 2 bytes and comes back in RAX,
  `InputMotionData_t` is 40 and comes back through a hidden pointer. SysV return
  classification is not a thing to hand-roll.

- **A shape whose correctness depends on 8-byte pointers is generated for the
  64-bit build alone.** On i386 the slot keeps its logging stub, so a 32-bit
  title gets a named line in `shim-unix.log` rather than a wrong answer. This is
  the ADR's one genuinely new idea and the one with a behavioural consequence:
  **coverage is now bitness-dependent**. It is worth it because the alternative
  was refusing 27 methods on both bitnesses to protect one, and because a
  refusal that announces itself is exactly what #45 asked for.

## Alternatives rejected

**Write the converter.** Still the right answer for the 6 that genuinely differ,
and still not derivable from the signature. `CreateBeacon` takes a
`w_SteamPartyBeaconLocation_t *` that is an **in** parameter; the other five are
**out**; none is `const`, so const-ness does not decide it. Two of them —
`GetAvailableBeaconLocations`, `GetDownloadedLeaderboardEntry` — take an *array*
whose element count lives in another parameter. Direction and extent are both
per-method knowledge, which makes a converter a design decision rather than a
mechanical one. Refused by name instead, which is what `overrides.json` exists
for when someone takes that decision.

**Transcribe the handful of aggregates by hand.** Eight structs is a small,
tempting transcription. It is also unguarded: nothing in the build would catch a
struct declared one field short, the way `verify_abi.py` catches a wrong arity.
Parsing Proton's text has no such gap.

**Generate for both bitnesses and marshal on i386.** Real work, for the 32-bit
half of a surface no 32-bit title in hand touches (the server browser, workshop
publishing). The bitness-scoped refusal leaves that door open and says so.

## Consequences

- Residue **71 → 44 methods**. Gone entirely: pointer-to-pointer (8), by-value
  aggregate returns (7) and params (4), and 20 `w_` struct params. Slots wired
  **5,029 → 5,191**; shapes **1,102 → 1,143**, of which 22 are x86_64-only,
  covering 110 slots.
- `#47` ISteamUGC, `#57` ISteamNetworking, `#58` ISteamMatchmakingServers, `#60`
  ISteamParties, `#49` ISteamGameServer and `#51` ISteamHTMLSurface lose most or
  all of their residue.
- The remaining 44 are 24 with no readable signature (Proton hand-writes those
  wrappers because they carry real logic), 10 function-pointer parameters
  (needing a native→PE upcall the seam does not have), 6 genuinely-divergent
  struct layouts, and 4 native-pointer returns.
- **A refusal can now be partial.** The unit is (interface, method, signature),
  so `ISteamMatchmakingServers::RequestInternetServerList` is refused for the 2
  older versions Proton hand-writes and generated for the rest. `REPORT.md`'s
  versions column is that count, and says so.
- The build gates cover the new surface without changes:
  `check_abi_layout.py` went from 2,352 to 2,462 offsets compared under i686 and
  x86_64, and `verify_abi.py` re-derives 1,121 of the 1,143 generated thunks
  from the i386 disassembly — the 22 absent ones being exactly the x86_64-only
  set, which is the mechanism proving itself.
- Verified live: the #7 harness through the shim reads all five achievements
  unchanged and both `GetStat` overloads still round-trip. No title in hand calls
  a newly-generated method, so those are covered by the build gates rather than
  by a run — stated rather than glossed.
