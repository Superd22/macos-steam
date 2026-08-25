#!/usr/bin/env python3
"""The one place that knows how MSVC's vtable order differs from the dylib's (#78).

Three generators and one checker need this answer, so it is stated once here
rather than three times.

The shim holds Proton's MSVC vtable order and calls the macOS dylib, which was
compiled in DECLARATION (Itanium) order. Those are the same order everywhere
except inside a set of same-name overloads, which MSVC lays out in REVERSE
declaration order. So for a run of overloads occupying MSVC slots [s..e]:

    native = s + (e - msvc)

Detecting a run is the only part that needs care, because the sets do NOT appear
as duplicate names — Proton has already disambiguated them by DECLARATION index:
the SDK's two `GetStat` overloads become `GetStat` (declared first) and
`GetStat_2` (declared second). Counting duplicate names finds zero across all 212
versions. So runs are found by base name with a trailing `_<n>` stripped.

The formula itself uses only POSITION, never the suffix — the suffix decides
what belongs to a run, not where it lands. That matters because it means the
rule does not depend on Proton's numbering scheme staying what it is today.

Verified against ground truth in STEAMUSERSTATS_INTERFACE_VERSION012, where the
signatures settle it and not just the names:

    MSVC slot 1  GetStat_2(const char *, float *)     -> native 2
    MSVC slot 2  GetStat(const char *, int32_t *)     -> native 1

and steam_ifaces.h, transcribed by hand from Valve's SDK headers, independently
puts GetStatI(const char*, int32_t*) at slot 1 and GetStatF(const char*, float*)
at slot 2. check_slot_transfer.py asserts that pairing on every build.
"""
import re


class OrderProblem(Exception):
    """The premise failed. Callers must stop, not skip: if a set is not a
    contiguous run then MSVC has reordered something this file does not model,
    and every OTHER method's slot on that interface is in doubt too."""


def base_name(name):
    """`GetStat_2` -> `GetStat`. The suffix is Proton's declaration index."""
    return re.sub(r'_\d+$', '', name)


def native_slots(slots):
    """MSVC slot -> native (declaration-order) slot, for one version's table.

    `slots` is a version's slot list from vtables.json, in MSVC order.
    Non-overloaded methods map to themselves, which is the overwhelming majority
    and the reason the shim can send a slot across the seam at all.
    """
    n = len(slots)
    out, i = {}, 0
    runs = {}
    while i < n:
        b = base_name(slots[i]['name'])
        j = i
        while j + 1 < n and base_name(slots[j + 1]['name']) == b:
            j += 1
        runs.setdefault(b, []).append((i, j))
        for k in range(i, j + 1):
            out[slots[k]['slot']] = slots[i]['slot'] + (slots[j]['slot'] - slots[k]['slot'])
        i = j + 1

    # A base name appearing in two separate runs means its overloads are NOT
    # contiguous. The scan above would then quietly treat each as a run of one
    # and reverse nothing — the exact silent-wrong-answer this whole file exists
    # to prevent — so it is an error, not a skip.
    for b, rs in runs.items():
        if len(rs) > 1:
            raise OrderProblem(
                '`%s` occupies %d separate runs of slots (%s), not one. MSVC only '
                'ever reverses a CONTIGUOUS run, which is what leaves every other '
                'method\'s slot untouched; that assumption has just failed and no '
                'slot on this interface can be trusted.'
                % (b, len(rs), ', '.join('%d-%d' % (slots[a]['slot'], slots[z]['slot'])
                                         for a, z in rs)))
    return out


def overload_runs(slots):
    """The same runs, as {base name: [slot, ...]}, for reporting. Length-1 runs
    (i.e. everything that is not an overload) are omitted."""
    n, i, out = len(slots), 0, {}
    while i < n:
        b = base_name(slots[i]['name'])
        j = i
        while j + 1 < n and base_name(slots[j + 1]['name']) == b:
            j += 1
        if j > i:
            out[b] = [slots[k]['slot'] for k in range(i, j + 1)]
        i = j + 1
    return out
