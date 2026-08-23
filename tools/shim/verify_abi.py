#!/usr/bin/env python3
"""Verify the i386 build pops the right number of bytes on every vtable slot (#20).

i386 thiscall is CALLEE-cleanup, so every function reachable through a vtable —
generated stub or hand-written thunk — must `ret N` with exactly the bytes its
MSVC caller pushed. Get it wrong and the caller's stack is corrupted on the
first call: no error, no wrong answer, just a jump to garbage some frames later.
That is not a failure mode worth discovering at runtime, so it is checked at
build time against Proton's own DEFINE_THISCALL_WRAPPER counts (which include
`this`, hence the -4).

This is what caught SteamInput002::Init(this) vs SteamInput006::Init(this,bool):
same method name, different signature, one C thunk originally serving both.

Usage: verify_abi.py <vtables.json> <shim_pe.c> <i386-object-file>
"""
import json, re, subprocess, sys

OBJDUMP = 'i686-w64-mingw32-objdump'

def ret_bytes(obj):
    """Map function name -> bytes popped by its ret."""
    out = subprocess.run([OBJDUMP, '-d', obj], capture_output=True, text=True, check=True).stdout
    funcs, cur = {}, None
    for line in out.split('\n'):
        # Match ANY symbol, including gcc's split/cloned forms (_seam.isra.0).
        # A regex that only matched plain identifiers left `cur` pointing at the
        # previous function across those, so their `ret`s were attributed to it
        # and silently overwrote a correct reading with a wrong one.
        m = re.match(r'^[0-9a-f]+ <_?([^>]+)>:', line)
        if m:
            cur = m.group(1); continue
        if cur and re.search(r'\bret\b', line):
            mm = re.search(r'ret\s+\$0x([0-9a-f]+)', line)
            n = int(mm.group(1), 16) if mm else 0
            # A function may have several exit paths; they must agree.
            if cur in funcs and funcs[cur] != n:
                funcs[cur] = -1          # inconsistent: report rather than pick one
            else:
                funcs[cur] = n
    return funcs

def main():
    tables = json.load(open(sys.argv[1]))['tables']
    src = open(sys.argv[2]).read()
    got = ret_bytes(sys.argv[3])
    bad, checked = [], 0

    for ver, t in tables.items():
        for s in t['slots']:
            name = 'stub_%s_%d' % (ver, s['slot'])
            if name not in got:
                continue
            checked += 1
            if got[name] < 0:
                bad.append('stub %s: inconsistent ret across exit paths' % name); continue
            if got[name] != s['bytes'] - 4:
                bad.append('stub %s (%s slot %d %s): pops %d, MSVC pushes %d'
                           % (name, ver, s['slot'], s['name'], got[name], s['bytes'] - 4))

    for ver, meth, fn in re.findall(r'wire\("([^"]+)", "([^"]+)", \(const void \*\)(\w+)\)', src):
        slot = [s for s in tables.get(ver, {'slots': []})['slots'] if s['name'] == meth]
        if not slot:
            bad.append('wire %s.%s: no such method in that version' % (ver, meth)); continue
        if fn not in got:
            continue
        checked += 1
        if got[fn] < 0:
            bad.append('thunk %s: inconsistent ret across exit paths' % fn); continue
        if got[fn] != slot[0]['bytes'] - 4:
            bad.append('thunk %s (%s.%s): pops %d, MSVC pushes %d — needs a '
                       'version-specific thunk' % (fn, ver, meth, got[fn], slot[0]['bytes'] - 4))

    if bad:
        print('i386 thiscall cleanup MISMATCH (%d):' % len(bad), file=sys.stderr)
        for b in bad:
            print('  ' + b, file=sys.stderr)
        sys.exit(1)
    print('i386 thiscall cleanup verified: %d entry points match Proton exactly' % checked)

main()
