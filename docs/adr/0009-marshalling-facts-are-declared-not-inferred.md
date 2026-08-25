# 9. Marshalling facts are declared, not inferred

Date: 2026-08-26

## Status

Accepted

## Context

ADR 0008 left 44 refused methods. Two classes were recorded as needing work the
generator could not do:

> `void*` return (4) — returns a native pointer with no length to copy down by
>
> struct layout differs on x86_64 (6) — needs a converter, and the direction and
> extent are not in the signature

The first was over-stated. Of the four, one is an ordinary interface getter
Valve retired in place — `DEPRECATED_GetISteamUnifiedMessages` — which
`wire_getters` already handles for every sibling and missed only because its
prefix test is `GetISteam` and the name begins `DEPRECATED_GetISteam`. Another,
`GetServerDetails`, returns a pointer to a struct Proton states both sides agree
on, which on x86_64 is the same move a `const char *` return already makes: one
address space, so the PE dereferences the native object in place. Only two
genuinely resist — both return interface pointers we cannot hand a title without
presenting a PE vtable for them.

The second was accurate about the obstacle and wrong about its size. The
conversion itself is entirely mechanical: Proton states both layouts field for
field, the field NAMES and their order are identical between them, and the only
difference is where the explicit `__pad_N[]` members fall. What is not derivable
is two facts per parameter, and the evidence is worth stating because the
tempting heuristics both fail:

- **Direction is not const-ness.** `ISteamParties::CreateBeacon` takes a
  non-const `w_SteamPartyBeaconLocation_t *` that is an **input** — the location
  to open a beacon at. The other five non-const cases are outputs. A rule keyed
  on `const` would have converted `CreateBeacon`'s input in the wrong direction
  and returned the caller its own uninitialised buffer.
- **Extent is not in the type.** `GetAvailableBeaconLocations` fills an *array*
  whose element count is the next parameter.

## Decision

**Two facts per parameter are declared in `overrides.json`; everything else is
generated.**

- A new `marshal` section names, per (interface, method, argument): the
  direction, and either a literal count or the parameter the count comes from.
  Without it a divergent-layout parameter is still refused — the generator never
  guesses either fact.

- `arg` is the argument index and `param` is the name it must have. The name is
  **checked at generation time against every version**, so a version that renames
  or reorders parameters fails the build rather than marshalling the wrong one.

- Converters are generated from Proton's two field lists, matched by name, with
  `__pad_N[]` dropped — those exist to pin the layout, and copying them would be
  copying the difference the converter absorbs. The generator refuses outright if
  the two field name sets are not equal, because then they are not the same
  struct and a field-by-field copy is not a conversion.

- `check_convert.py` proves the copy is complete, and does so **without reading
  the converter**: it takes Proton's field list from `structs.json`, fills the
  Windows struct with one byte pattern and the destination with another, runs
  `w2u` then `u2w`, and compares every declared field. A field the converter
  never copies keeps the second pattern and is caught. The check and the
  generator derive from the same source by different routes, so they would have
  to be wrong in the same way.

- `wire_getters` matches `DEPRECATED_GetISteam*` as well as `GetISteam*`, and a
  return that is a pointer to an agreed-on struct is generated for x86_64 only,
  the same bitness-scoping ADR 0008 introduced.

## Alternatives rejected

**Infer direction from `const`.** `CreateBeacon` is the counter-example, and it
is not an edge case — it is one of the six.

**Convert in both directions always.** Safe for `in` and `inout`, and it would
have avoided declaring direction. Rejected because for a pure `out` it reads
uninitialised PE memory into the temporary before the call: harmless in practice,
but it makes every such call depend on the caller having zeroed a buffer it has
no contract to zero. The generated handler zeroes the temporary instead, which
is the same cost and no assumption.

**Hand-write the six.** Six methods, three structs, and a converter each. The
converters would then be transcriptions with no gate behind them — the exact
shape ADR 0007 removed, reintroduced at the point where a mistake is a silently
wrong field rather than a crash.

## Consequences

- Residue **44 → 37**. The divergent-layout class is closed entirely (6 → 0) and
  native-pointer returns go 4 → 2. Slots wired **5,191 → 5,218**; shapes
  **1,143 → 1,152**, of which 31 are x86_64-only.
- `#47` ISteamUGC, `#60` ISteamParties, `#73` ISteamUserStats and `#55`
  ISteamNetworkingUtils lose all or most of their remaining residue.
- The 37 left are 25 with no readable signature, 10 function-pointer parameters,
  and 2 interface-pointer returns. All three need a mechanism the seam does not
  have or logic Proton hand-wrote; none is a generator change.
- The build gains one gate, and it was checked the way the others were: dropping
  a single field from a generated converter makes `check_convert.py` name it and
  fail. A gate that cannot go red is decoration.
- **Not verified at runtime.** No title in hand calls a converted method — they
  are party beacons, leaderboard entries and workshop details. These rest on the
  build gates, which is stated here rather than glossed.
