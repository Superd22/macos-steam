#!/usr/bin/env python3
"""Test the correspondence the generated thunks rest on (#78).

Every generated call dispatches on the NATIVE side by indexing the dylib's
vtable with an index the PE side resolved from Proton's MSVC table. Those two
orders are the same everywhere except inside a set of same-name overloads, which
MSVC lays out in reverse; msvc_order.py resolves that, and gen_vtables.py writes
the resolved index into each method's `native` field. Whether it resolves it
CORRECTLY is a claim, and this checks it.

The evidence is independent, which is the point. steam_ifaces.h holds classes a
human transcribed from Valve's SDK headers over #11/#20/#43, in DECLARATION
order — the order the macOS dylib was compiled in, arrived at by reading the SDK.
vtables.json holds Proton's MSVC order, arrived at by clang compiling the same
SDK for Windows. Two readings of one source, by two routes.

So walk the transcription by declaration index i, and ask which Proton method
msvc_order says lands at native slot i:

  - it must be the same method, by name; or
  - where the transcription had to rename an overload (C++ cannot declare
    `GetStat` twice in one class), its SIGNATURE must be the one the rename
    describes. GetStatI at declaration slot 1 must resolve to a Proton entry
    taking an `int32_t *`, not a `float *`.

That second case is the whole point of this file. It is the only class where the
two orders disagree, so it is the only class where the resolution can be wrong,
and a name comparison cannot see it: the names differ by construction. Only the
types settle it — and they do, because getting the reversal backwards would hand
GetStat(const char*, float*) an int32_t* and write a float through it.

A disagreement anywhere else is not a bad transcription to patch. It means the
two orders diverge somewhere msvc_order.py does not model, and every generated
thunk on that interface is dispatching by an index that disagreement invalidates.

Usage: check_slot_transfer.py <vtables.json> <steam_ifaces.h>
"""
import json, re, sys
import msvc_order

# The hand-transcribed class -> the Proton version it was transcribed against.
# Both halves are named in steam_ifaces.h's own comments.
CLASSES = {
    'ISteamUser':             'SteamUser021',
    'ISteamUserStats012':     'STEAMUSERSTATS_INTERFACE_VERSION012',
    'ISteamUtils010':         'SteamUtils010',
    'ISteamClient':           'SteamClient020',
    'ISteamApps008':          'STEAMAPPS_INTERFACE_VERSION008',
    'ISteamInput006':         'SteamInput006',
    'ISteamRemoteStorage016': 'STEAMREMOTESTORAGE_INTERFACE_VERSION016',
    'ISteamFriends017':       'SteamFriends017',
}

# Where the transcription could not use Valve's own name, because the name is one
# of an overload set C++ will not let you declare twice. Each maps to the Proton
# base name and the parameter type that tells the siblings apart — which is what
# turns this from "the names differ, shrug" into a real test of the reversal.
RENAMED = {
    'GetStatI':     ('GetStat',     'int32_t'),
    'GetStatF':     ('GetStat',     'float'),
    'SetStatI':     ('SetStat',     'int32_t'),
    'SetStatF':     ('SetStat',     'float'),
    'GetUserStatI': ('GetUserStat', 'int32_t'),
    'GetUserStatF': ('GetUserStat', 'float'),
}


def main():
    tables = json.load(open(sys.argv[1]))['tables']
    src = open(sys.argv[2]).read()

    decls = {}
    for m in re.finditer(r'class\s+(\w+)\s*\{(.*?)\n\};', src, re.S):
        decls[m.group(1)] = re.findall(r'virtual\s+[^;]*?\b(\w+)\s*\(', m.group(2))

    bad, checked, reversed_checked = [], 0, 0
    for cls, ver in sorted(CLASSES.items()):
        if cls not in decls:
            bad.append('%s: no such class in steam_ifaces.h' % cls); continue
        if ver not in tables:
            bad.append('%s: %s is not a generated version' % (cls, ver)); continue
        slots = tables[ver]['slots']
        try:
            native = msvc_order.native_slots(slots)
        except msvc_order.OrderProblem as e:
            bad.append('%s (%s): %s' % (cls, ver, e)); continue
        # native slot -> the Proton entry that msvc_order says sits there
        at_native = {native[s['slot']]: s for s in slots}

        for i, name in enumerate(decls[cls]):
            if i not in at_native:
                bad.append('%s slot %d (%s): past the end of %s\'s %d-slot table'
                           % (cls, i, name, ver, len(slots)))
                continue
            checked += 1
            got = at_native[i]
            if name == got['name']:
                continue
            if name not in RENAMED:
                bad.append('%s declaration slot %d: the SDK declares `%s` there, but '
                           'msvc_order resolves Proton\'s `%s` (MSVC slot %d) onto it. '
                           'Every generated thunk on %s dispatches by that index.'
                           % (cls, i, name, got['name'], got['slot'], ver))
                continue
            # A renamed overload. Its base name must match, and its SIGNATURE must
            # be the one the rename describes — that is the reversal being tested.
            base, want = RENAMED[name]
            reversed_checked += 1
            if msvc_order.base_name(got['name']) != base:
                bad.append('%s declaration slot %d (`%s`): resolves onto `%s`, which is '
                           'not an overload of `%s` at all'
                           % (cls, i, name, got['name'], base))
                continue
            types = [a[0] for a in (got['sig'] or {}).get('args', [])]
            if not any(want in t for t in types):
                bad.append('%s declaration slot %d: the SDK declares `%s` there — a `%s` '
                           'overload — but msvc_order resolves Proton\'s `%s`(%s) onto it. '
                           'The overload reversal is backwards, and the call would write '
                           'through a reinterpreted pointer.'
                           % (cls, i, name, want, got['name'], ', '.join(types)))

    if bad:
        print('SLOT TRANSFER BROKEN (%d):' % len(bad), file=sys.stderr)
        for b in bad:
            print('  ' + b, file=sys.stderr)
        sys.exit(1)
    print('slot transfer verified: %d hand-transcribed declaration slots across %d '
          'interfaces resolve onto the right Proton method, %d of them by signature '
          'through an MSVC overload reversal' % (checked, len(CLASSES), reversed_checked))


main()
