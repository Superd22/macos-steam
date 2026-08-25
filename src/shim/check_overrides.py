#!/usr/bin/env python3
"""Keep overrides.json honest about what is actually hand-written (#78).

overrides.json is what gen_thunks.py reads to decide what NOT to emit, and its
largest class is "a hand-written thunk in shim_pe.c already serves this". That
class is a claim about another file, and a claim about another file goes stale.

Both directions are failures, and neither is cosmetic:

  listed but no longer hand-wired  -> the generator skips a method NOTHING
                                      serves. That is the silent 0 from #43,
                                      reintroduced by a stale list.

  hand-wired but not listed        -> the generator emits a second thunk for a
                                      method that already has one, and which of
                                      the two wins the slot is a question about
                                      the order of two calls in build_vtables().

So: every method shim_pe.c hand-wires must appear in overrides.json under SOME
reason, and every method overrides.json calls hand-written must still be
hand-wired.

The two halves are asymmetric on purpose. A semantic override is a judgement
about behaviour, not a claim about code — the four overlay predicates are both
hand-written and semantic, and the day someone deletes their thunks the right
answer is still "do not generate them". So a semantic entry satisfies the first
rule and is exempt from the second.

Usage: check_overrides.py <vtables.json> <overrides.json> <shim_pe.c>
"""
import json, re, sys


def main():
    tables = json.load(open(sys.argv[1]))['tables']
    ovr = json.load(open(sys.argv[2]))
    src = open(sys.argv[3]).read()

    def iface(ver):
        t = tables[ver]['tag'].split('_', 1)[0]
        return t[3:] if t.startswith('win') else t

    # Every wiring form in shim_pe.c, reduced to (interface, method). The
    # reference version names the interface; which versions it reaches does not
    # matter here, only that the method is claimed by hand.
    wired, unknown = set(), []
    for ver, meth in re.findall(r'\bwire(?:_all)?\("([^"]+)",\s*"([^"]+)"', src):
        if ver not in tables:
            unknown.append('%s.%s' % (ver, meth)); continue
        wired.add((iface(ver), meth))
    for meth, ra, _rb in re.findall(
            r'wire_all_2\(\s*"([^"]+)",\s*"([^"]+)",[^,]+,\s*"([^"]+)"', src, re.S):
        if ra not in tables:
            unknown.append('%s.%s' % (ra, meth)); continue
        wired.add((iface(ra), meth))
    # wire_getters_all("ISteamClient") claims the whole GetISteam* family across
    # every version of that interface, by prefix rather than by name.
    for i in re.findall(r'wire_getters_all\("([^"]+)"\)', src):
        for ver, t in tables.items():
            if iface(ver) != i:
                continue
            for s in t['slots']:
                n = s['name']
                if n.startswith('DEPRECATED_'):
                    n = n[11:]
                if n.startswith('GetISteam'):
                    wired.add((i, s['name']))

    listed = {(o['interface'], o['method'])
              for o in ovr['skip'] if o['reason'] == 'hand-written'}
    listed_any = {(o['interface'], o['method']) for o in ovr['skip']}

    stale = sorted(listed - wired)
    missing = sorted(wired - listed_any)
    if unknown or stale or missing:
        for w in unknown:
            print('ERROR: shim_pe.c wires %s, which is not a generated version'
                  % w, file=sys.stderr)
        for i, m in stale:
            print('ERROR: overrides.json calls %s::%s hand-written, but nothing in '
                  'shim_pe.c wires it any more.\n'
                  '       Drop the entry and the generator will serve it.'
                  % (i, m), file=sys.stderr)
        for i, m in missing:
            print('ERROR: shim_pe.c hand-wires %s::%s, which overrides.json does '
                  'not list.\n'
                  '       Add it with reason "hand-written", or the generator '
                  'emits a second thunk for the same slot.' % (i, m), file=sys.stderr)
        sys.exit(1)

    sem = sum(1 for o in ovr['skip'] if o['reason'] == 'semantic override')
    print('overrides: %d hand-written entries match shim_pe.c exactly, '
          '%d semantic overrides' % (len(listed), sem))


main()
