#!/usr/bin/env python3
"""Extract Steamworks struct layouts from Proton's generated headers (#82).

#78 stopped retyping the signatures Proton already states. This stops retyping
the STRUCT LAYOUTS it already states, which is what the remaining by-value and
by-pointer aggregates were refused for. Two files, neither of which the build
was fetching:

  steamclient_structs_generated.h   1,473 structs, explicit `#pragma pack` and
                                    explicit `__pad_N[]` members
  steamclient_structs.h            the opaque byte-blob typedefs
                                    (`typedef struct { uint8_t _[20]; } SteamIPAddress_t`)

## The classification is Proton's, not ours

The generated header already answers the only question that matters at our seam.
A struct appears either as ONE plain definition — Proton has determined the
Windows and unix layouts agree — or as a `w64_`/`u64_`/`w32_`/`u32_` family,
which is Proton saying they differ and here is exactly how. Within the family it
distinguishes further, and this is the part that decides most of the work:

    typedef struct w64_SteamParamStringArray_t u64_SteamParamStringArray_t;

is Proton stating that on x86_64 the unix layout IS the Windows layout, and only
the 32-bit forms diverge. Measured across the 121 families: 35 are identical on
x86_64, 86 genuinely differ. Since our seam is x86_64 on both sides, the first
group needs no conversion at all — the address crosses and the native side reads
it in place, exactly like every other pointer.

So each base name is classified as one of:

  plain          one definition; identical everywhere
  x64-identical  a w64/u64 family whose two halves are the same struct on x86_64
  x64-differs    a w64/u64 family that genuinely differs on x86_64

Bodies are captured VERBATIM rather than re-modelled into fields. Re-deriving a
layout from a parsed field list would put our arithmetic between Proton's
declaration and the compiler, which is one more place to be wrong about padding;
emitting Proton's own text keeps the compiler as the only authority. The C++
sections (`operator`, constructors) are stripped, and W64_PTR/U64_PTR are
resolved to their x86_64 expansion, which is the declaration unchanged.

Usage: extract_structs.py <dir-of-proton-lsteamclient> > structs.json
"""
import json, os, re, sys

# `#pragma pack( push, N )` / `struct X` / body / `};` / `#pragma pack( pop )`
STRUCT_RE = re.compile(
    r'#pragma pack\(\s*push,\s*(\d+)\s*\)\s*\nstruct (\w+)\s*\n\{\n(.*?)\n\};\s*\n#pragma pack\(\s*pop\s*\)',
    re.S)
# `typedef struct { uint8_t _[20]; } SteamIPAddress_t;`
OPAQUE_RE = re.compile(r'typedef struct \{\s*uint8_t _\[(\d+)\];\s*\}\s*(\w+);')
# `typedef struct A B;` with A != B
ALIAS_RE = re.compile(r'^typedef struct (\w+) (\w+);\s*$', re.M)

PREFIXES = ('w64_', 'u64_', 'w32_', 'u32_')


def strip_cxx(body):
    """Drop the C++ half of a body: `#ifdef __cplusplus` blocks, converting
    operators, defaulted constructors. What is left is the plain C layout, which
    is the only thing either half of the seam needs."""
    out, depth = [], 0
    for line in body.split('\n'):
        st = line.strip()
        if st.startswith('#if') and '__cplusplus' in st:
            depth += 1; continue
        if depth:
            if st.startswith('#if'):
                depth += 1
            elif st.startswith('#endif'):
                depth -= 1
            elif st.startswith('#else'):
                pass
            continue
        if st.startswith('#endif') or st.startswith('#else'):
            continue
        out.append(line)
    return '\n'.join(l for l in out if l.strip())


def resolve_ptr_macros(body):
    """W64_PTR(decl, name, type) -> decl, which is its x86_64 expansion verbatim
    (steamclient_structs.h: `#define W64_PTR( decl, name, type ) decl`). Only the
    32-bit forms wrap the member in a ptr32, and we do not emit those."""
    def one(m):
        inner = m.group(1)
        depth, cur = 0, ''
        for ch in inner:                       # first top-level comma
            if ch in '(<':  depth += 1
            elif ch in ')>': depth -= 1
            if ch == ',' and depth == 0:
                break
            cur += ch
        return '    ' + cur.strip() + ';'
    return re.sub(r'^\s*[WU](?:64|32)_PTR\((.*)\);\s*$', one, body, flags=re.M)


def main():
    d = sys.argv[1]
    gen = open(os.path.join(d, 'steamclient_structs_generated.h'),
               encoding='utf-8', errors='replace').read()
    base = open(os.path.join(d, 'steamclient_structs.h'),
                encoding='utf-8', errors='replace').read()

    structs, order = {}, []
    for pack, name, body in STRUCT_RE.findall(gen):
        body = resolve_ptr_macros(strip_cxx(body))
        structs[name] = {'pack': int(pack), 'body': body}
        order.append(name)

    opaque = {}
    for size, name in OPAQUE_RE.findall(base):
        opaque[name] = int(size)

    alias = {b: a for a, b in ALIAS_RE.findall(gen) if a != b}

    # Classify every base name the seam might meet.
    classes, families = {}, set()
    for name in structs:
        for p in PREFIXES:
            if name.startswith(p):
                families.add(name[len(p):])
    for b in sorted(families):
        if alias.get('u64_' + b) == 'w64_' + b:
            classes[b] = 'x64-identical'
        elif 'u64_' + b in structs:
            classes[b] = 'x64-differs'
        elif 'w64_' + b in structs:
            # A family with no u64 form at all: Proton emits no unix counterpart,
            # so there is nothing to say it converts. Report rather than guess.
            classes[b] = 'x64-unknown'
    for name in structs:
        if not name.startswith(PREFIXES):
            classes[name] = 'plain'
    for name in opaque:
        classes[name] = 'opaque'

    json.dump({'structs': structs, 'order': order, 'opaque': opaque,
               'classes': classes}, sys.stdout, indent=1, sort_keys=True)

    n = len(classes)
    counts = {}
    for v in classes.values():
        counts[v] = counts.get(v, 0) + 1
    print('extract_structs: %d names (%s)'
          % (n, ', '.join('%s=%d' % kv for kv in sorted(counts.items()))),
          file=sys.stderr)


main()
