#!/usr/bin/env python3
"""Generate the shim's thunks from Proton's own typed signatures (#78).

#47-#77 were written as ~1,270 hand-typed thunks. They should not be: slot
order, i386 arity AND the full typed signature all come out of the same line of
the same Proton file, and every artifact the shim needs per method is a pure
function of that signature. This emits all five, per SHAPE:

    an sp_g_* params struct    (gen/shim_gen_params.h)
    an opcode                  (gen/shim_gen_opcodes.h)
    a PE thunk + its wiring    (gen/shim_gen_pe.h)
    a unix handler             (gen/shim_gen_unix.h)
    two dispatch-table entries (gen/shim_gen_dispatch.h)

plus gen/shim_gen_arity.json (what verify_abi.py re-derives from the i386 binary)
and REPORT.md (every method NOT generated, each with a named reason).

## Emit per SHAPE, not per version

The unit is (interface, method, signature) — 1269 of them across 6555 vtable
entries — because that is the unit a single C function can serve. Wiring is
still per (version, slot), resolved BY NAME here at generation time against
each version's own table, and emitted as a direct assignment. That is stricter
than wire_all's runtime byte match: two versions whose arity coincides but whose
signatures differ get different thunks, where wire_all would hand them the same
one.

## Dispatch by slot, never by class cast

The unix half calls through `vslot(handle, slot)` — the ISteamFriends pattern
(#23) — instead of casting the handle to a declared C++ class. Not a shortcut:
39 interfaces x 212 versions is not transcribable by hand, and the transcription
is what carries the risk. The slot that crosses the seam is the NATIVE one. MSVC's only reordering against
the order the dylib was compiled in is to reverse each contiguous run of
same-name overloads, so for the great majority the two indices are the same
number; for the 140 slots inside such a run they are not, and msvc_order.py
resolves the correspondence once, for every generator that needs it. Overloads
therefore generate like anything else: Proton has already disambiguated their
NAMES (`GetStat` / `GetStat_2`), which is what lets one thunk-per-shape address
them individually.

## Refuse loudly

A silent skip would recreate #43 at scale — a title getting a plausible 0 from a
method nobody wired, with nothing anywhere saying so. Every refusal lands in
REPORT.md with the reason, and every refused slot still names itself in
shim-unix.log the first time a title touches it (the #45 stub path).

Usage: gen_thunks.py <vtables.json> <structs.json> <overrides.json> <out-dir>
"""
import json, os, re, sys, collections
import msvc_order

# ---------------------------------------------------------------- type map ---
# One table, stated once (#78 step 2). C type -> (seam field code, PE thunk
# type, native argument type).
#
# The seam field is always a FIXED-WIDTH type: shim_abi.h's whole bitness-
# neutrality argument is that every params struct is laid out widest-first out
# of explicit fixed-width fields, so a 32-bit PE and the 64-bit unix half agree
# field-for-field (check_abi_layout.py proves it every build).
#
# Native argument types are deliberately coarse. Every pointer is `void *`,
# because SysV passes all pointers identically and declaring 200 Steamworks
# struct types here would be transcription — the exact thing this file exists to
# stop doing. The pointee is never dereferenced on this side.
SCALARS = {
    'int8_t':   ('b', 'int8_t',   'int8_t'),
    'uint8_t':  ('B', 'uint8_t',  'uint8_t'),
    'char':     ('b', 'char',     'char'),
    'int16_t':  ('h', 'int16_t',  'int16_t'),
    'uint16_t': ('H', 'uint16_t', 'uint16_t'),
    'int32_t':  ('i', 'int32_t',  'int32_t'),
    'uint32_t': ('I', 'uint32_t', 'uint32_t'),
    'int64_t':  ('q', 'int64_t',  'int64_t'),
    'uint64_t': ('Q', 'uint64_t', 'uint64_t'),
    'float':    ('f', 'float',    'float'),
    'double':   ('d', 'double',   'double'),
}
# CSteamID and CGameID are ONE uint64 under pack(1) — the fact steam_ifaces.h
# already asserts and relies on. By value they are a type mapping, not a hazard:
# i386 pushes 8 bytes for them exactly as for a uint64, and SysV puts an 8-byte
# POD struct in an INTEGER register exactly as for a uint64.
ID_TYPES = {'CSteamID', 'CGameID'}

# The struct model, from extract_structs.py (#82). Proton states every layout,
# and — more usefully — states which of them actually DIFFER between the Windows
# and unix forms. Our seam is x86_64 on both sides, so a family Proton marks
# identical on x86_64 needs no conversion at all: the address crosses and the
# native side reads it in place, like any other pointer.
#
#   plain / opaque   one layout everywhere
#   x64-identical    same struct on x86_64; only the 32-bit forms diverge, so
#                    the thunk is generated for x86_64 ONLY
#   x64-differs      genuinely different on x86_64; refused, named
STRUCTS = {}


def struct_class(base):
    """Proton's verdict for one struct name, or None if it never mentions it."""
    return STRUCTS.get('classes', {}).get(base)


def struct_kind(t):
    """(base name, verdict) for a declared type, pointer levels stripped."""
    base = norm(t.replace('const', '')).rstrip('* ').strip()
    if base.startswith('w_') or base.startswith('u_'):
        base = base[2:]
    return base, struct_class(base)


def struct_ctype(base, kind):
    """The C type name to actually emit for a base name. A `plain` or `opaque`
    struct is declared under its own name; an `x64-identical` FAMILY is declared
    as `w64_<base>`, with Proton's own `typedef struct w64_X u64_X` being the
    statement that the unix side sees the same layout."""
    return base if kind in ('plain', 'opaque') else 'w64_' + base

# Field code -> C type, and its width. Ordering the fields by width descending
# is what makes the struct bitness-neutral.
CODE_C = {'b': 'int8_t', 'B': 'uint8_t', 'h': 'int16_t', 'H': 'uint16_t',
          'i': 'int32_t', 'I': 'uint32_t', 'q': 'int64_t', 'Q': 'uint64_t',
          'f': 'float', 'd': 'double'}
CODE_W = {'b': 1, 'B': 1, 'h': 2, 'H': 2, 'i': 4, 'I': 4, 'q': 8, 'Q': 8,
          'f': 4, 'd': 8}


def norm(t):
    """Collapse whitespace and drop the const qualifier from a POINTER type's
    outer level, so `const char *` and `char *` land on the same field code."""
    return re.sub(r'\s+', ' ', t).strip()


class Refuse(Exception):
    """Carries the reason verbatim into REPORT.md. There is no unnamed refusal."""


def map_param(t):
    """C parameter type -> (field code, PE thunk type, native arg type, x64_only,
    by-value struct name or None).

    `x64_only` means the thunk is correct on x86_64 and NOT on i386, so it is
    generated for the 64-bit build alone and the 32-bit build keeps its logging
    stub. That is not a hedge: on i386 the PE side's layout genuinely differs
    from the native one, and half a shim that says so is better than a whole one
    that guesses (#45)."""
    t = norm(t)
    if '(*)' in t:
        raise Refuse('function-pointer parameter `%s` — calling it means a '
                     'deferred upcall from native code back into PE code, which '
                     'the seam does not carry' % t)
    if t.endswith('**'):
        # An array of pointers. On x86_64 both sides hold 8-byte pointers and the
        # array crosses verbatim; only a 32-bit PE, whose pointees are 4 bytes
        # wide, cannot be read in place. So this is an i386 problem that was
        # being charged to both bitnesses.
        return 'Q', 'void *', 'void *', True, None
    if t.endswith('*') or t.endswith(']'):
        base, kind = struct_kind(re.sub(r'\[.*$', '', t))
        if kind == 'x64-differs':
            raise Refuse('parameter points at `%s`, whose Windows and unix layouts '
                         'differ on x86_64 by Proton\'s own generated definitions — '
                         'it needs a field-by-field converter, and the element '
                         'count and direction are not in the signature' % base)
        if kind == 'x64-unknown':
            raise Refuse('parameter points at `%s`, for which Proton generates a '
                         'Windows layout but no unix counterpart, so nothing says '
                         'whether the two agree' % base)
        if kind == 'x64-identical':
            # Proton states the unix layout IS the Windows layout on x86_64.
            return 'Q', 'void *', 'void *', True, None
        # plain, opaque, or a type Proton never splits: an address the GAME owns,
        # zero-extending into the uint64 field, read or written in place — the
        # same path GetUserDataFolder has taken since #20.
        return 'Q', 'void *', 'void *', False, None
    bare = norm(t.replace('const', ''))
    if bare in ID_TYPES:
        return 'Q', 'uint64_t', 'uint64_t', False, None
    if bare in SCALARS:
        c, pe, nat = SCALARS[bare]
        return c, pe, nat, False, None
    # A by-value aggregate. It crosses as the ADDRESS of the caller's copy, and
    # the native side passes it by value again on its own side — which needs the
    # type declared, so the type is emitted from Proton's own definition rather
    # than transcribed.
    base, kind = struct_kind(bare)
    if kind in ('plain', 'opaque'):
        ct = struct_ctype(base, kind)
        return 'Q', ct, ct, False, ct
    if kind == 'x64-identical':
        ct = struct_ctype(base, kind)
        return 'Q', ct, ct, True, ct
    if kind == 'x64-differs':
        raise Refuse('by-value parameter `%s`, whose Windows and unix layouts '
                     'differ on x86_64 by Proton\'s own generated definitions' % base)
    raise Refuse('by-value aggregate parameter `%s` — Proton states no layout '
                 'for it' % t)


def map_ret(sig):
    """Return-type handling. Five outcomes, in the order they must be tested:

    ('void',  ...)  nothing comes back
    ('sret',  ...)  MSVC's hidden-result-pointer form for a CSteamID. Proton has
                    ALREADY rewritten every by-value CSteamID return into it,
                    which is exactly the bug that cost the most time in #11 — the
                    fix arrives encoded in the signature.
    ('agg',   ...)  the same hidden-pointer form for a real aggregate. The PE
                    half just forwards the caller's buffer; the unix half needs
                    the type declared so the compiler classifies the native
                    return correctly (InputDigitalActionData_t is 2 bytes and
                    comes back in RAX; InputMotionData_t is 40 and comes back
                    through a hidden pointer — not a thing to hand-roll).
    ('str',   ...)  const char*: the bytes live on the dylib's heap above 4 GB,
                    so a 32-bit PE takes the native_str copy-down path (#20).
    ('val',   ...)  a scalar, straight back.

    Returns (kind, field code, PE return type, native return type, x64_only,
    by-value struct name or None).
    """
    r = norm(sig['ret'])
    args = sig['args']
    if r == 'void':
        return 'void', '', 'void', 'void', False, None
    if r.endswith('*'):
        base = norm(r.rstrip('*').replace('const', ''))
        if base == 'char':
            return 'str', 'Q', 'const char *', 'uint64_t', False, None
        # Proton's sret rewrite: `T * f(w_iface *, T *_ret)`.
        if args and norm(args[0][0]).rstrip(' *') == base and args[0][1] == '_ret':
            if base in ID_TYPES:
                return 'sret', 'Q', 'uint64_t *', 'uint64_t', False, None
            b2, kind = struct_kind(base)
            if kind in ('plain', 'opaque', 'x64-identical'):
                ct = struct_ctype(b2, kind)
                return 'agg', 'Q', ct + ' *', ct, kind == 'x64-identical', ct
            if kind == 'x64-differs':
                raise Refuse('returns `%s` by value, whose Windows and unix '
                             'layouts differ on x86_64' % b2)
            raise Refuse('returns the aggregate `%s` by value and Proton states '
                         'no layout for it' % base)
        raise Refuse('returns `%s`, a native pointer — a 32-bit PE cannot hold '
                     'a macOS heap address, and there is no length to copy down '
                     'by' % r)
    if r in ID_TYPES:
        return 'val', 'Q', 'uint64_t', 'uint64_t', False, None
    if r in SCALARS:
        c, pe, nat = SCALARS[r]
        return 'val', c, pe, nat, False, None
    raise Refuse('returns the by-value aggregate `%s`' % r)


def shape_of(sig):
    """One signature -> the shape record, or Refuse. `codes` is the argument
    field-code string that names the params struct; `x64_only` is true if ANY
    part of the signature is correct on x86_64 but not on i386; `structs` is the
    set of by-value aggregate types this shape needs emitted."""
    kind, rcode, pe_ret, nat_ret, x64, rstruct = map_ret(sig)
    args = sig['args'][1:] if kind in ('sret', 'agg') else sig['args']
    codes, pe_args, nat_args, structs = '', [], [], set()
    if rstruct:
        structs.add(rstruct)
    for i, (t, name) in enumerate(args):
        c, pe, nat, ax, st = map_param(t)
        codes += c
        x64 = x64 or ax
        if st:
            structs.add(st)
        pe_args.append((pe, 'a%d' % i, norm(t), name, st))
        nat_args.append((nat, st))
    return dict(kind=kind, codes=codes, rcode=rcode, pe_ret=pe_ret,
                nat_ret=nat_ret, pe_args=pe_args, nat_args=nat_args,
                x64_only=x64, structs=structs)


def struct_name(sh):
    return 'sp_g_%s_%s' % (sh['codes'] or 'v', sh['rcode'] or 'v')


def struct_fields(sh):
    """Widest-first, which is the whole of shim_abi.h's bitness-neutrality
    argument. Field names are a pure function of the struct name, which is what
    lets 1269 shapes share ~200 structs without any of them being ambiguous."""
    f = [('Q', 'handle')]
    f += [(c, 'a%d' % i) for i, c in enumerate(sh['codes'])]
    f.append(('i', 'slot'))
    if sh['rcode']:
        f.append((sh['rcode'], 'ret'))
    return sorted(f, key=lambda x: -CODE_W[x[0]])


# ------------------------------------------------------------------- main ----
def short(iface):
    return iface[6:] if iface.startswith('ISteam') else iface


def main():
    global STRUCTS
    src, structs_path, ovr_path, outdir = sys.argv[1:5]
    STRUCTS = json.load(open(structs_path))
    data = json.load(open(src))
    if data['problems']:
        sys.exit('refusing to generate, extractor reported problems:\n  ' +
                 '\n  '.join(data['problems']))
    tables = data['tables']
    ovr = json.load(open(ovr_path))
    skip = {(o['interface'], o['method']): o for o in ovr['skip']}

    # Interface for each version, straight from Proton's own tag.
    iface_of = {v: (t['tag'].split('_', 1)[0][3:] if t['tag'].startswith('win')
                    else t['tag'].split('_', 1)[0])
                for v, t in tables.items()}

    # Resolve the MSVC -> native slot correspondence for every version, and stop
    # if any version's overload sets are not the contiguous runs the whole design
    # assumes. Nothing here consumes the answer — the PE half reads it from the
    # generated table at call time — but the generator must not emit a thousand
    # callers of a premise that has already failed.
    for ver, t in tables.items():
        try:
            msvc_order.native_slots(t['slots'])
        except msvc_order.OrderProblem as e:
            sys.exit('%s: %s' % (ver, e))

    # Group every vtable entry by (interface, method, signature).
    groups = collections.OrderedDict()
    refused = collections.OrderedDict()   # (iface, method) -> reason

    def refuse(iface, method, reason, versions):
        key = (iface, method)
        if key not in refused:
            refused[key] = {'reason': reason, 'versions': []}
        refused[key]['versions'] += versions

    for ver, t in sorted(tables.items()):
        iface = iface_of[ver]
        for s in t['slots']:
            key = (iface, s['name'])
            if key in skip:
                o = skip[key]
                refuse(iface, s['name'], '%s: %s' % (o['reason'], o['why']), [ver])
                continue
            if not s['sig']:
                refuse(iface, s['name'], 'Proton hand-writes this wrapper — there '
                       'is no one-line typed signature to read', [ver])
                continue
            if s['bytes'] is None:
                refuse(iface, s['name'], 'no DEFINE_THISCALL_WRAPPER, so the i386 '
                       'callee-cleanup byte count is unknown', [ver])
                continue
            try:
                sh = shape_of(s['sig'])
            except Refuse as e:
                refuse(iface, s['name'], str(e), [ver])
                continue
            gkey = (iface, s['name'], json.dumps(s['sig'], sort_keys=True))
            g = groups.setdefault(gkey, {'iface': iface, 'method': s['name'],
                                         'sig': s['sig'], 'shape': sh,
                                         'bytes': s['bytes'], 'wire': []})
            g['wire'].append((ver, s['slot'], s['bytes']))

    # One shape, one arity. Same signature with a different DEFINE_THISCALL_
    # WRAPPER count means one of the two readings is wrong; on i386 that is a
    # stack corruption, so it is a generation-time refusal, not a runtime risk.
    for gkey in list(groups):
        g = groups[gkey]
        sizes = {b for _v, _s, b in g['wire']}
        if len(sizes) > 1:
            refuse(g['iface'], g['method'],
                   'one signature, %d different i386 arities (%s) — Proton\'s own '
                   'two readings disagree' % (len(sizes), sorted(sizes)),
                   [v for v, _s, _b in g['wire']])
            del groups[gkey]

    # Distinct names within an interface: a second shape of the same method gets
    # a _s2 suffix rather than colliding.
    seen, names = collections.Counter(), {}
    for gkey, g in groups.items():
        base = '%s_%s' % (short(g['iface']), g['method'])
        seen[base] += 1
        names[gkey] = base if seen[base] == 1 else '%s_s%d' % (base, seen[base])

    structs = collections.OrderedDict()
    for g in groups.values():
        structs.setdefault(struct_name(g['shape']), g['shape'])

    # Every by-value aggregate any generated thunk needs, plus whatever those
    # definitions themselves reference. Emitted from Proton's own text, in
    # Proton's own order, so nothing here is a re-derivation of a layout.
    need = set()
    for g in groups.values():
        need |= g['shape']['structs']
    seen_dep = set()
    while need - seen_dep:
        for n in list(need - seen_dep):
            seen_dep.add(n)
            body = STRUCTS['structs'].get(n, {}).get('body', '')
            for tok in re.findall(r'\b(\w+)\b', body):
                if tok in STRUCTS['structs'] or tok in STRUCTS['opaque']:
                    need.add(tok)

    os.makedirs(outdir, exist_ok=True)
    banner = ('/* GENERATED by gen_thunks.py from Proton\'s own typed signatures\n'
              ' * (lsteamclient, proton_11.0, via vtables.json). DO NOT EDIT.\n'
              ' * Regenerate: ./build.sh   —  refusals and why: gen/REPORT.md (#78) */\n')

    # -- by-value aggregate definitions --------------------------------------
    L = [banner, '#pragma once', '']
    L.append('/* Steamworks aggregates that cross the seam BY VALUE, emitted verbatim')
    L.append(' * from Proton\'s steamclient_structs_generated.h rather than transcribed.')
    L.append(' * Only types Proton states are layout-identical between the Windows and')
    L.append(' * unix forms appear here, so ONE definition serves both halves; anything')
    L.append(' * whose layouts differ is refused instead (gen/REPORT.md). Emitting the')
    L.append(' * real type rather than a byte blob is what lets the compiler classify')
    L.append(' * the native call: InputDigitalActionData_t is 2 bytes and returns in')
    L.append(' * RAX, InputMotionData_t is 40 and returns through a hidden pointer. */')
    for n, size in sorted(STRUCTS['opaque'].items()):
        if n in need:
            L.append('typedef struct { uint8_t _[%d]; } %s;' % (size, n))
    # Forward declarations first. Proton's file order is not dependency order —
    # RemoteStorageUpdatePublishedFileRequest_t holds a SteamParamStringArray_t*
    # and is defined above it — and every such reference is a pointer, so a tag
    # declaration is all it needs.
    L.append('')
    for n in STRUCTS['order']:
        if n in need:
            L.append('typedef struct %s %s;' % (n, n))
    L.append('')
    for n in STRUCTS['order']:
        if n not in need:
            continue
        d = STRUCTS['structs'][n]
        L.append('#pragma pack( push, %d )' % d['pack'])
        L.append('struct %s' % n)
        L.append('{')
        L.append(d['body'])
        L.append('};')
        L.append('#pragma pack( pop )')
    write(outdir, 'shim_gen_structs.h', L)

    # -- params structs -------------------------------------------------------
    L = [banner, '#pragma once', '']
    L.append('/* Named by ARGUMENT field codes then the RETURN code, so the field')
    L.append(' * layout is a pure function of the name: Q=uint64 I/i=32 H/h=16')
    L.append(' * B/b=8 f=float d=double, v=none. Fields are emitted widest-first,')
    L.append(' * which is what keeps every offset identical under i686 and x86_64')
    L.append(' * (check_abi_layout.py proves it, every build).')
    L.append(' *')
    L.append(' * `slot` is the MSVC vtable index the PE half resolved against the')
    L.append(' * version the title actually asked for. It travels with every call')
    L.append(' * because the unix half indexes the native vtable with it rather')
    L.append(' * than casting the handle to a declared class — see gen_thunks.py. */')
    for n, sh in sorted(structs.items()):
        L.append('struct %s { %s };' % (n, ' '.join(
            '%s %s;' % (CODE_C[c], f) for c, f in struct_fields(sh))))
    write(outdir, 'shim_gen_params.h', L)

    # -- opcodes --------------------------------------------------------------
    L = [banner, '/* Appended INSIDE enum shim_call, after every hand-written opcode,',
         ' * so no existing index moves. */']
    for gkey, g in groups.items():
        L.append('    C_G_%s,' % names[gkey])
    write(outdir, 'shim_gen_opcodes.h', L)

    # -- PE thunks ------------------------------------------------------------
    L = [banner, '#pragma once', '']
    for gkey, g in groups.items():
        # A thunk whose correctness depends on 8-byte pointers is generated for
        # the 64-bit build alone. On i386 the slot keeps its logging stub, so the
        # title gets a named line in shim-unix.log instead of a wrong answer
        # (#45) — which is the whole reason refusing per-bitness is worth doing
        # rather than refusing outright for both.
        if g['shape']['x64_only']:
            L.append('#ifndef __i386__')
        L += pe_thunk(names[gkey], g)
        if g['shape']['x64_only']:
            L.append('#endif')
    L.append('')
    L.append('/* Wire every generated thunk into every version whose table declares')
    L.append(' * the method with the SAME signature. Resolution happened BY NAME at')
    L.append(' * generation time against each version\'s own table, so these are')
    L.append(' * direct assignments: nothing is matched, guessed or shape-compared')
    L.append(' * at runtime. Called BEFORE the hand-written wiring, which therefore')
    L.append(' * still wins — though nothing hand-written is generated (overrides.json).')
    L.append(' */')
    L.append('static int gen_wire_all(void)')
    L.append('{')
    n, n64 = 0, 0
    for gkey, g in groups.items():
        if g['shape']['x64_only']:
            L.append('#ifndef __i386__')
        for ver, slot, _b in g['wire']:
            L.append('    vt_%s[%d] = (const void *)g_%s;' % (ver, slot, names[gkey]))
            n += 1
            if g['shape']['x64_only']:
                n64 += 1
        if g['shape']['x64_only']:
            L.append('#endif')
    L.append('#ifdef __i386__')
    L.append('    return %d;' % (n - n64))
    L.append('#else')
    L.append('    return %d;' % n)
    L.append('#endif')
    L.append('}')
    write(outdir, 'shim_gen_pe.h', L)

    # -- unix handlers --------------------------------------------------------
    L = [banner, '#pragma once', '']
    for gkey, g in groups.items():
        L += unix_handler(names[gkey], g)
    write(outdir, 'shim_gen_unix.h', L)

    # -- dispatch entries -----------------------------------------------------
    L = [banner, '/* Order MUST match gen/shim_gen_opcodes.h — the static_assert on',
         ' * C_COUNT in shim_unix.cpp is what keeps the two in step. */']
    for gkey in groups:
        L.append('    ug_%s,' % names[gkey])
    write(outdir, 'shim_gen_dispatch.h', L)

    # -- arity, for verify_abi.py --------------------------------------------
    json.dump({'g_%s' % names[k]: g['bytes'] for k, g in groups.items()},
              open(os.path.join(outdir, 'shim_gen_arity.json'), 'w'),
              indent=1, sort_keys=True)

    # -- the report -----------------------------------------------------------
    report(outdir, tables, iface_of, groups, names, refused, n, n64)

    n64shapes = sum(1 for g in groups.values() if g['shape']['x64_only'])
    print('gen_thunks: %d shapes (%d x86_64-only), %d params structs, %d aggregates, '
          '%d vtable slots wired (%d of them x86_64-only), %d methods refused '
          '(gen/REPORT.md)'
          % (len(groups), n64shapes, len(structs), len(need), n, n64, len(refused)))


def write(outdir, name, lines):
    open(os.path.join(outdir, name), 'w').write('\n'.join(lines) + '\n')


def pe_thunk(name, g):
    """The PE half: pack the fixed-layout struct, cross the seam, hand back the
    answer in whatever form MSVC expects it."""
    sh = g['shape']
    st = struct_name(sh)
    params = ''.join(', %s %s' % (t, n) for t, n, _ct, _cn, _s in sh['pe_args'])
    if sh['kind'] == 'sret':
        params, ret_t = ', uint64_t *sret' + params, 'uint64_t *'
    elif sh['kind'] == 'agg':
        # MSVC's hidden result pointer for a real aggregate. The PE half never
        # needs the size: it forwards the caller's own buffer and returns it.
        params, ret_t = ', %s *sret' % _agg(sh) + params, '%s *' % _agg(sh)
    else:
        ret_t = sh['pe_ret'] if sh['kind'] != 'void' else 'void'
    L = ['/* %s::%s(%s) */' % (g['iface'], g['method'],
                               ', '.join('%s %s' % (norm(t), n) for t, n in g['sig']['args']))]
    L.append('static %s THISCALL g_%s(struct w_iface *s%s)' % (ret_t, name, params))
    L.append('{')
    L.append('    struct %s p;' % st)
    L.append('    memset(&p, 0, sizeof p);')
    L.append('    p.handle = s->handle;')
    L.append('    p.slot = native_slot(s, "%s");' % g['method'])
    if sh['kind'] == 'agg':
        L.append('    p.ret = (uint64_t)(uintptr_t)sret;')
    for i, (t, n, _ct, _cn, bystruct) in enumerate(sh['pe_args']):
        if bystruct:
            # A by-value aggregate: what crosses is the address of this thunk's
            # own copy, which the native side reads and passes by value again.
            L.append('    p.a%d = (uint64_t)(uintptr_t)&%s;' % (i, n))
        elif t == 'void *':
            L.append('    p.a%d = (uint64_t)(uintptr_t)%s;' % (i, n))
        else:
            L.append('    p.a%d = %s;' % (i, n))
    L.append('    seam(C_G_%s, &p);' % name)
    if sh['kind'] == 'sret':
        L.append('    *sret = p.ret;')
        L.append('    return sret;')
    elif sh['kind'] == 'agg':
        L.append('    return sret;')
    elif sh['kind'] == 'str':
        L.append('    return native_str(p.ret);')
    elif sh['kind'] == 'val':
        L.append('    return (%s)p.ret;' % sh['pe_ret'])
    L.append('}')
    return L


def _agg(sh):
    """The aggregate type an 'agg' shape returns."""
    return sh['nat_ret']


def unix_handler(name, g):
    """The unix half: index the native vtable with the slot the PE side sent and
    call through a typed function pointer. No class cast, so no transcription."""
    sh = g['shape']
    st = struct_name(sh)
    nat_ret = sh['nat_ret'] if sh['kind'] != 'agg' else sh['nat_ret']
    fnt = '%s (*)(void *%s)' % (nat_ret,
                                ''.join(', ' + t for t, _s in sh['nat_args']))
    call = ''
    for i, (t, bystruct) in enumerate(sh['nat_args']):
        if bystruct:
            call += ', *(%s *)(uintptr_t)p->a%d' % (t, i)
        elif t == 'void *':
            call += ', (void *)(uintptr_t)p->a%d' % i
        else:
            call += ', (%s)p->a%d' % (t, i)
    L = ['static NTSTATUS ug_%s(void *args)' % name]
    L.append('{')
    L.append('    auto *p = (struct %s *)args;' % st)
    L.append('    static bool first = true;')
    L.append('    if (first) { first = false; ulog("first call: %s::%s (slot %%d)", p->slot); }'
             % (g['iface'], g['method']))
    L.append('    auto fn = (%s)vslot(p->handle, p->slot);' % fnt)
    L.append('    if (!fn) { ulog("%s::%s: slot %%d unresolvable — call dropped", '
             'p->slot); return 0; }' % (g['iface'], g['method']))
    if sh['kind'] == 'agg':
        # The native side returns the aggregate BY VALUE. Letting the compiler
        # make that call from the declared type is the whole point: SysV
        # classification depends on the struct's size and field classes, and
        # `p->ret` is the caller's own buffer, forwarded by the PE half.
        L.append('    %s v = fn((void *)(uintptr_t)p->handle%s);' % (nat_ret, call))
        L.append('    if (p->ret) memcpy((void *)(uintptr_t)p->ret, &v, sizeof v);')
    elif sh['kind'] == 'void':
        L.append('    fn((void *)(uintptr_t)p->handle%s);' % call)
    else:
        L.append('    p->ret = (%s)fn((void *)(uintptr_t)p->handle%s);'
                 % (CODE_C[sh['rcode']], call))
    L.append('    return 0;')
    L.append('}')
    return L


def report(outdir, tables, iface_of, groups, names, refused, nwired, n64):
    ifaces = collections.defaultdict(lambda: [0, 0, 0])   # iface -> [shapes, x64only, refused]
    for g in groups.values():
        ifaces[g['iface']][0] += 1
        if g['shape']['x64_only']:
            ifaces[g['iface']][1] += 1
    for (iface, _m), r in refused.items():
        ifaces[iface][2] += 1
    total_slots = sum(len(t['slots']) for t in tables.values())
    n64shapes = sum(1 for g in groups.values() if g['shape']['x64_only'])

    L = ['# Generated thunks: what was emitted, and what was refused', '',
         'GENERATED by `gen_thunks.py` (#78, #82). Do not edit — regenerate with `./build.sh`.', '',
         'Every method below is one the emitter **declined**, with the reason. A refused',
         'slot keeps its logging stub, so a title that calls one still names it in',
         '`shim-unix.log` the first time it does (#45/#46) — the refusal is loud at build',
         'time and loud again at runtime, never silent.', '',
         'A refusal can be PARTIAL. The unit of generation is (interface, method,',
         'signature), so a method whose older versions Proton hand-writes and whose newer',
         'ones carry a readable signature is generated for the second set and refused for',
         'the first — `ISteamMatchmakingServers::RequestInternetServerList` is refused for',
         '2 versions and generated for the rest. The versions column is that count.', '',
         '## Totals', '',
         '| | |', '| --- | --- |',
         '| interface versions | %d |' % len(tables),
         '| vtable slots | %d |' % total_slots,
         '| slots wired to a generated thunk | %d |' % nwired,
         '| — of those, x86_64 only | %d |' % n64,
         '| distinct shapes emitted | %d |' % len(groups),
         '| — of those, x86_64 only | %d |' % n64shapes,
         '| methods refused | %d |' % len(refused), '',
         '## x86_64-only shapes', '',
         'A shape is generated for the 64-bit build alone when its correctness depends on',
         'pointers being 8 bytes wide on both sides of the seam — an array of pointers, or a',
         'struct Proton states is identical on x86_64 and divergent on i386. On the 32-bit',
         'build those slots keep their stub and report themselves the first time a title',
         'calls one, which is a named line rather than a wrong answer.', '',
         '## Per interface', '',
         '| interface | shapes emitted | x86_64 only | methods refused |',
         '| --- | ---: | ---: | ---: |']
    for iface in sorted(ifaces):
        w, x, r = ifaces[iface]
        L.append('| `%s` | %d | %d | %d |' % (iface, w, x, r))
    L += ['', '## Refusals', '',
          '| interface | method | versions | reason |', '| --- | --- | ---: | --- |']
    for (iface, method), r in sorted(refused.items()):
        L.append('| `%s` | `%s` | %d | %s |'
                 % (iface, method, len(set(r['versions'])), r['reason']))
    L.append('')
    write(outdir, 'REPORT.md', L)


main()
