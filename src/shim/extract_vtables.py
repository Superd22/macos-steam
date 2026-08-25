#!/usr/bin/env python3
"""Extract MSVC vtable slot order + i386 thiscall stack sizes from Proton's
lsteamclient PE-side sources (#20).

Source of truth: ValveSoftware/Proton lsteamclient/winISteam<Iface>.c, which is
clang-generated from Valve's own SDK headers and therefore already encodes the
MSVC same-name-overload reversal. Two facts per interface version:

  __ASM_VTABLE(win<Iface>_<VER>, VTABLE_ADD_FUNC(...) ...)  -> exact slot order
  DEFINE_THISCALL_WRAPPER(<method>, <stack_bytes>)          -> i386 arg bytes,
                                                               INCLUDING `this`
  alloc_vtable(&..., <N>, ...)                              -> slot count check

The stack_bytes are what makes a 32-bit stub safe: i386 thiscall is
callee-cleanup, so a stub must pop exactly the bytes its caller pushed. On
x86_64 (caller-cleanup) the arity is harmless, so one generated table serves
both builds.

NEVER read the cpp*.cpp files instead: they omit methods Proton handles by hand
(GetAPICallResult is absent from SteamUtils010), silently shifting every later
slot (docs/research/steamworks-vtable-tables.md).

Usage: extract_vtables.py <dir-of-winISteam*.c> <Iface_VERSION> [...]
       extract_vtables.py <dir-of-winISteam*.c> --all

`--all` generates a table for EVERY interface version Proton defines, which is
what the shim ships (#29). Naming the versions explicitly makes the set of
titles that work a property of a hand-curated list, and the failure mode when a
title wants one we skipped is a null-deref deep inside the game, not a message.
Proton's sources already enumerate every version Valve has shipped, so `--all`
makes coverage a property of the generator instead.
"""
import re, sys, json, os

def parse(path):
    src = open(path, encoding='utf-8', errors='replace').read()
    sizes = dict(re.findall(r'DEFINE_THISCALL_WRAPPER\(\s*(\w+)\s*,\s*(\d+)\s*\)', src))
    sizes = {k: int(v) for k, v in sizes.items()}
    vtables = {}
    for m in re.finditer(r'__ASM_VTABLE\(\s*(\w+)\s*,(.*?)\n\s*\);', src, re.S):
        tag, body = m.group(1), m.group(2)
        vtables[tag] = re.findall(r'VTABLE_ADD_FUNC\(\s*(\w+)\s*\)', body)
    counts = {t: int(n) for t, n in re.findall(r'alloc_vtable\(\s*&(\w+)_vtable\s*,\s*(\d+)', src)}
    return vtables, sizes, counts

def main():
    d, wanted = sys.argv[1], sys.argv[2:]
    tables, problems = {}, []
    files = {}
    for fn in sorted(os.listdir(d)):
        if fn.startswith('winISteam') and fn.endswith('.c'):
            files[fn] = parse(os.path.join(d, fn))

    # --all: every version defined anywhere in the sources. The version string is
    # the tag with its interface prefix stripped -- winISteamUtils_SteamUtils010
    # -> SteamUtils010 -- which also handles the digit-less oddballs such as
    # STEAMCONTROLLER_INTERFACE_VERSION.
    if wanted == ['--all']:
        wanted = sorted({tag.split('_', 1)[1]
                         for vtables, _s, _c in files.values()
                         for tag in vtables
                         if '_' in tag})

    for want in wanted:
        hit = None
        for fn, (vtables, sizes, counts) in files.items():
            for tag, funcs in vtables.items():
                # tag looks like winISteamUtils_SteamUtils010
                if tag.split('_', 1)[1] == want:
                    hit = (fn, tag, funcs, sizes, counts)
                    break
            if hit: break
        if not hit:
            problems.append(f'{want}: no __ASM_VTABLE block found'); continue
        fn, tag, funcs, sizes, counts = hit
        slots = []
        for i, f in enumerate(funcs):
            if f not in sizes:
                problems.append(f'{want} slot {i} {f}: no DEFINE_THISCALL_WRAPPER')
                slots.append({'slot': i, 'name': f.split('_')[-1], 'bytes': None}); continue
            slots.append({'slot': i, 'name': f[len(tag) + 1:], 'bytes': sizes[f]})
        # independent cross-check: alloc_vtable's own slot count
        n = counts.get(tag)
        if n is not None and n != len(funcs):
            problems.append(f'{want}: alloc_vtable says {n} slots, vtable block has {len(funcs)}')
        tables[want] = {'source': fn, 'tag': tag, 'slots': slots,
                        'alloc_vtable_count': n}
    json.dump({'tables': tables, 'problems': problems}, sys.stdout, indent=1)
    if problems:
        print('\n-- PROBLEMS --\n' + '\n'.join(problems), file=sys.stderr)

main()
