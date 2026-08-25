#!/usr/bin/env python3
"""Test the one assumption the generated thunks rest on (#78).

Every generated call dispatches on the NATIVE side by indexing the dylib's
vtable with the slot the PE side resolved from Proton's MSVC table. That is only
correct if the MSVC order and the dylib's own Itanium order are the same order,
and they are not the same order in general: MSVC lays a set of same-name
overloads out in reverse. gen_thunks.py answers that by refusing every method in
such a set. Whether the refusal is sufficient is a claim, and this checks it.

The evidence is independent, which is the point. steam_ifaces.h holds classes a
human transcribed from Valve's SDK headers over #11/#20/#43, in DECLARATION
order — that is the Itanium order the macOS dylib was compiled with, arrived at
by reading the SDK. vtables.json holds Proton's MSVC order, arrived at by clang
compiling the same SDK for Windows. Two different readings of the same source,
by two different routes. Line them up index by index:

  - a name that matches exactly is a slot that transfers
  - a name that differs must be inside a same-name overload set, which
    gen_thunks.py already refuses, or the design is broken

A disagreement anywhere else is not a bad transcription to patch. It means the
MSVC and Itanium orders diverge somewhere this file did not predict, and every
generated thunk on that interface is dispatching to some other method.

Usage: check_slot_transfer.py <vtables.json> <steam_ifaces.h>
"""
import json, re, sys

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

# Where the transcription deliberately does not use Valve's name, because Valve's
# name is one of an overload pair C++ cannot declare twice in one class. Each of
# these is inside a set gen_thunks.py refuses, so none of them is dispatched by
# slot; they are listed so this check can tell "renamed on purpose" apart from
# "the two orders disagree".
RENAMED = {
    'GetStatI', 'GetStatF', 'SetStatI', 'SetStatF',
    'GetUserStatI', 'GetUserStatF',
    'InitiateGameConnection_DEPRECATED', 'TerminateGameConnection_DEPRECATED',
}


def main():
    tables = json.load(open(sys.argv[1]))['tables']
    src = open(sys.argv[2]).read()

    decls = {}
    for m in re.finditer(r'class\s+(\w+)\s*\{(.*?)\n\};', src, re.S):
        decls[m.group(1)] = re.findall(r'virtual\s+[^;]*?\b(\w+)\s*\(', m.group(2))

    bad, checked = [], 0
    for cls, ver in sorted(CLASSES.items()):
        if cls not in decls:
            bad.append('%s: no such class in steam_ifaces.h' % cls); continue
        if ver not in tables:
            bad.append('%s: %s is not a generated version' % (cls, ver)); continue
        msvc = [s['name'] for s in tables[ver]['slots']]
        for i, name in enumerate(decls[cls]):
            if i >= len(msvc):
                bad.append('%s slot %d (%s): past the end of %s\'s %d-slot table'
                           % (cls, i, name, ver, len(msvc)))
                continue
            checked += 1
            if name == msvc[i]:
                continue
            # Differs. The ONLY acceptable reason is an overload set: Proton
            # disambiguates those with a `_n` suffix, and gen_thunks.py refuses
            # every member.
            base = re.sub(r'_\d+$', '', msvc[i])
            overloaded = sum(1 for n in msvc if re.sub(r'_\d+$', '', n) == base) > 1
            if overloaded and name in RENAMED:
                continue
            bad.append('%s slot %d: the SDK declares `%s` there, Proton\'s MSVC '
                       'table has `%s`. Outside an overload set the two orders '
                       'must agree — every generated thunk on %s is dispatching '
                       'by an index this disagreement invalidates.'
                       % (cls, i, name, msvc[i], ver))

    if bad:
        print('SLOT TRANSFER BROKEN (%d):' % len(bad), file=sys.stderr)
        for b in bad:
            print('  ' + b, file=sys.stderr)
        sys.exit(1)
    print('slot transfer verified: %d hand-transcribed Itanium slots across %d '
          'interfaces agree with Proton\'s MSVC order' % (checked, len(CLASSES)))


main()
