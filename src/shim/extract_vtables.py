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
  <ret> __thiscall win<Iface>_<VER>_<Method>(...)           -> the FULL typed
                                                               signature (#78)

The signature is the third fact, and it was being thrown away. It sits on the
very next line of the same file as the arity, and every artifact the shim needs
per method — params struct, PE thunk, unix handler — is a pure function of it
(#78). Captured here as `sig` on each slot: {ret, args}, args EXCLUDING `_this`
and with their declared Proton types verbatim. Deciding what those types mean is
gen_thunks.py's job, not this one's; the extractor stays a transcriber.

A slot with no parseable signature gets `sig: null` rather than a problem entry:
Proton hand-writes ~25 wrappers (multi-line signatures, or bodies with real
logic behind them), and that is a fact about the source, not a fault in reading
it. gen_thunks.py refuses those by name.

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

# `<ret> __thiscall <name>(<args>)` on ONE line, which is how clang-format
# leaves every generated wrapper. Comments are stripped first: the ISteamClient
# getters declare their return as `void /*ISteamUser*/ *`, and with the comment
# left in place all 212 of them read as unparseable.
SIG_RE = re.compile(
    r'^([A-Za-z_][A-Za-z0-9_ ]*?[ *]*)\s*__thiscall\s+(winISteam\w+)\((.*?)\)\s*$', re.M)

def split_args(s):
    """Top-level comma split. Function-pointer and array declarators carry
    nested parens/brackets, so a plain s.split(',') tears them in half."""
    depth, cur, out = 0, '', []
    for ch in s:
        if ch in '([':   depth += 1
        elif ch in ')]': depth -= 1
        if ch == ',' and depth == 0:
            out.append(cur); cur = ''
        else:
            cur += ch
    out.append(cur)
    return [p.strip() for p in out if p.strip()]

def split_decl(decl):
    """`const char *pchFile` -> ('const char *', 'pchFile'). Handles the two
    declarator forms Proton emits where the name is not the last token:
    `void (*W_CDECL pFunction)(int32_t)` and `char (*errMsg)[1024]`."""
    m = re.match(r'^(.*?)\(\s*\*\s*(?:W_CDECL\s+)?(\w+)\s*\)(.*)$', decl, re.S)
    if m:
        return (m.group(1).strip() + ' (*)' + m.group(3).strip()), m.group(2)
    m = re.match(r'^(.*?[\s*])(\w+)\s*(\[[^\]]*\])?$', decl, re.S)
    if not m:
        return decl.strip(), ''
    typ = m.group(1).strip() + (m.group(3) or '')
    return typ, m.group(2)

def parse(path):
    src = open(path, encoding='utf-8', errors='replace').read()
    sizes = dict(re.findall(r'DEFINE_THISCALL_WRAPPER\(\s*(\w+)\s*,\s*(\d+)\s*\)', src))
    sizes = {k: int(v) for k, v in sizes.items()}
    vtables = {}
    for m in re.finditer(r'__ASM_VTABLE\(\s*(\w+)\s*,(.*?)\n\s*\);', src, re.S):
        tag, body = m.group(1), m.group(2)
        vtables[tag] = re.findall(r'VTABLE_ADD_FUNC\(\s*(\w+)\s*\)', body)
    counts = {t: int(n) for t, n in re.findall(r'alloc_vtable\(\s*&(\w+)_vtable\s*,\s*(\d+)', src)}
    nocomment = re.sub(r'/\*.*?\*/', '', src, flags=re.S)
    sigs = {}
    for m in SIG_RE.finditer(nocomment):
        args = split_args(m.group(3))[1:]          # drop `struct w_iface *_this`
        sigs[m.group(2)] = {'ret': m.group(1).strip(),
                            'args': [list(split_decl(a)) for a in args]}
    return vtables, sizes, counts, sigs

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
                         for vtables, _s, _c, _g in files.values()
                         for tag in vtables
                         if '_' in tag})

    for want in wanted:
        hit = None
        for fn, (vtables, sizes, counts, sigs) in files.items():
            for tag, funcs in vtables.items():
                # tag looks like winISteamUtils_SteamUtils010
                if tag.split('_', 1)[1] == want:
                    hit = (fn, tag, funcs, sizes, counts, sigs)
                    break
            if hit: break
        if not hit:
            problems.append(f'{want}: no __ASM_VTABLE block found'); continue
        fn, tag, funcs, sizes, counts, sigs = hit
        slots = []
        for i, f in enumerate(funcs):
            if f not in sizes:
                problems.append(f'{want} slot {i} {f}: no DEFINE_THISCALL_WRAPPER')
                slots.append({'slot': i, 'name': f.split('_')[-1], 'bytes': None,
                              'sig': sigs.get(f)}); continue
            slots.append({'slot': i, 'name': f[len(tag) + 1:], 'bytes': sizes[f],
                          'sig': sigs.get(f)})
        # independent cross-check: alloc_vtable's own slot count
        n = counts.get(tag)
        if n is not None and n != len(funcs):
            problems.append(f'{want}: alloc_vtable says {n} slots, vtable block has {len(funcs)}')
        tables[want] = {'source': fn, 'tag': tag, 'slots': slots,
                        'alloc_vtable_count': n}
    # One line per SLOT. Indenting all the way down turns the six thousand
    # signatures into ninety thousand lines, and this file is committed data
    # that gets reviewed as a diff — a moved slot should be one changed line.
    marks = {}
    for t in tables.values():
        for i, sl in enumerate(t['slots']):
            # A plain-ASCII sentinel on purpose: json escapes control
            # characters, so a \x00 marker comes back out as \u0000 and the
            # substitution below silently misses every slot.
            key = '@@SLOT%d@@' % len(marks)
            marks[key] = json.dumps(sl, sort_keys=True)
            t['slots'][i] = key
    out = json.dumps({'tables': tables, 'problems': problems}, indent=1)
    for key, compact in marks.items():
        out = out.replace('"%s"' % key, compact)
    sys.stdout.write(out)
    if problems:
        print('\n-- PROBLEMS --\n' + '\n'.join(problems), file=sys.stderr)

main()
