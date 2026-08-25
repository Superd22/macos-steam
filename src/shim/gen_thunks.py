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
is what carries the risk. The MSVC slot the PE side holds IS the native Itanium
slot for every method generated here, because MSVC's only reordering is to
reverse each contiguous run of same-name overloads, and REFUSALS below drop
every method that sits in such a run.

## Refuse loudly

A silent skip would recreate #43 at scale — a title getting a plausible 0 from a
method nobody wired, with nothing anywhere saying so. Every refusal lands in
REPORT.md with the reason, and every refused slot still names itself in
shim-unix.log the first time a title touches it (the #45 stub path).

Usage: gen_thunks.py <vtables.json> <overrides.json> <out-dir>
"""
import json, os, re, sys, collections

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
    """C parameter type -> (field code, PE thunk type, native arg type)."""
    t = norm(t)
    if '(*)' in t:
        raise Refuse('function-pointer parameter `%s` — calling it means a '
                     'deferred upcall from native code back into PE code, which '
                     'the seam does not carry' % t)
    if t.endswith('**'):
        raise Refuse('pointer-to-pointer parameter `%s` — the POINTEES are 4 '
                     'bytes wide in the 32-bit PE and 8 on the native side, so '
                     'the array cannot be read in place' % t)
    if t.endswith('*') or t.endswith(']'):
        base = norm(t.rstrip('*').replace('const', ''))
        base = re.sub(r'\[.*$', '', base).strip()
        if base.startswith('w_'):
            raise Refuse('parameter `%s` points at a Proton w_-prefixed struct, '
                         'which is Proton\'s own marker for a layout that needs '
                         'converting between the Windows and native forms; we '
                         'have no converter' % t)
        # Everything else is an address the GAME owns. It zero-extends into the
        # uint64 field and the native side reads or writes through it in place —
        # the same path GetUserDataFolder has taken since #20.
        return 'Q', 'void *', 'void *'
    bare = norm(t.replace('const', ''))
    if bare in ID_TYPES:
        return 'Q', 'uint64_t', 'uint64_t'
    if bare in SCALARS:
        return SCALARS[bare]
    raise Refuse('by-value aggregate parameter `%s` — the seam carries scalars '
                 'and addresses, not struct bodies' % t)


def map_ret(sig):
    """Return-type handling. Four outcomes, in the order they must be tested:

    ('void',  ...)  nothing comes back
    ('sret',  ...)  MSVC's hidden-result-pointer form. Proton has ALREADY
                    rewritten every by-value CSteamID return into it, which is
                    exactly the bug that cost the most time in #11 — the fix
                    arrives encoded in the signature.
    ('str',   ...)  const char*: the bytes live on the dylib's heap above 4 GB,
                    so a 32-bit PE takes the native_str copy-down path (#20).
    ('val',   ...)  a scalar, straight back.
    """
    r = norm(sig['ret'])
    args = sig['args']
    if r == 'void':
        return 'void', '', 'void', 'void'
    if r.endswith('*'):
        base = norm(r.rstrip('*').replace('const', ''))
        if base == 'char':
            return 'str', 'Q', 'const char *', 'uint64_t'
        # Proton's sret rewrite: `T * f(w_iface *, T *_ret)`. Only a T that is
        # one uint64 can cross — anything wider is a struct body.
        if args and norm(args[0][0]).rstrip(' *') == base and args[0][1] == '_ret':
            if base in ID_TYPES:
                return 'sret', 'Q', 'uint64_t *', 'uint64_t'
            raise Refuse('returns the aggregate `%s` by value (MSVC hidden-'
                         'pointer form) — the seam carries no struct bodies' % base)
        raise Refuse('returns `%s`, a native pointer — a 32-bit PE cannot hold '
                     'a macOS heap address, and there is no length to copy down '
                     'by' % r)
    if r in ID_TYPES:
        return 'val', 'Q', 'uint64_t', 'uint64_t'
    if r in SCALARS:
        c, pe, nat = SCALARS[r]
        return 'val', c, pe, nat
    raise Refuse('returns the by-value aggregate `%s`' % r)


# ------------------------------------------------------------------ shapes ---
def shape_of(sig):
    """(kind, codes, pe_params, native_types, ret_*) for one signature, or
    Refuse. `codes` is the argument field-code string that names the struct."""
    kind, rcode, pe_ret, nat_ret = map_ret(sig)
    args = sig['args'][1:] if kind == 'sret' else sig['args']
    codes, pe_args, nat_args = '', [], []
    for i, (t, name) in enumerate(args):
        c, pe, nat = map_param(t)
        codes += c
        pe_args.append((pe, 'a%d' % i, norm(t), name))
        nat_args.append(nat)
    return dict(kind=kind, codes=codes, rcode=rcode, pe_ret=pe_ret,
                nat_ret=nat_ret, pe_args=pe_args, nat_args=nat_args)


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
    src, ovr_path, outdir = sys.argv[1], sys.argv[2], sys.argv[3]
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

    # ---- same-name overload sets, the one thing that breaks slot transfer ----
    #
    # MSVC lays a set of same-name overloads out in REVERSE declaration order,
    # against the order the dylib was compiled in. So for a method inside such a
    # set, the slot the PE half holds is not the slot to index the native vtable
    # with, and the whole dispatch-by-slot design does not apply. Those must be
    # refused.
    #
    # They do NOT show up as a duplicate name. Proton has already disambiguated
    # them — the SDK's two `GetStat` overloads become `GetStat` and `GetStat_2`,
    # by DECLARATION index, and the vtable then lists `GetStat_2` first because
    # that is what MSVC does. Counting duplicate names finds nothing at all and
    # would have let all 140 of them through, each dispatching to its own
    # sibling: GetStat(int) landing on GetStat(float).
    #
    # So group by the name with a trailing `_<n>` stripped. Sets found this way:
    # 140 slot entries, suffixes _2.._4.
    overloaded = set()
    for ver, t in tables.items():
        g = collections.defaultdict(list)
        for s in t['slots']:
            g[re.sub(r'_\d+$', '', s['name'])].append(s)
        for base, members in g.items():
            if len(members) < 2:
                continue
            for m in members:
                overloaded.add((iface_of[ver], m['name']))
            # The reversal is confined to the set, which is the ENTIRE reason
            # every method outside one keeps its slot. That holds only if the
            # set is contiguous. If one ever is not, the argument fails for the
            # whole version and refusing the set alone would not be enough — so
            # stop, rather than generate 6,000 slots on a broken premise.
            slots = sorted(m['slot'] for m in members)
            if slots != list(range(slots[0], slots[0] + len(slots))):
                sys.exit('%s.%s: overload set occupies non-contiguous slots %s.\n'
                         'Dispatch-by-slot assumes MSVC only ever reverses a '
                         'CONTIGUOUS run, which is what leaves every other '
                         'method\'s slot untouched. That assumption just failed; '
                         'refusing to generate.' % (ver, base, slots))

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
            if key in overloaded:
                refuse(iface, s['name'], 'one of a same-name overload set (Proton '
                       'disambiguates them with a `_n` suffix): MSVC reverses the '
                       'run against the order the dylib was compiled in, so the '
                       'slot the PE half holds is a SIBLING overload\'s slot on '
                       'the native side', [ver])
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

    os.makedirs(outdir, exist_ok=True)
    banner = ('/* GENERATED by gen_thunks.py from Proton\'s own typed signatures\n'
              ' * (lsteamclient, proton_11.0, via vtables.json). DO NOT EDIT.\n'
              ' * Regenerate: ./build.sh   —  refusals and why: gen/REPORT.md (#78) */\n')

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
        L += pe_thunk(names[gkey], g)
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
    n = 0
    for gkey, g in groups.items():
        for ver, slot, _b in g['wire']:
            L.append('    vt_%s[%d] = (const void *)g_%s;' % (ver, slot, names[gkey]))
            n += 1
    L.append('    return %d;' % n)
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
    report(outdir, tables, iface_of, groups, names, refused, n)

    print('gen_thunks: %d shapes, %d structs, %d vtable slots wired, %d methods '
          'refused (gen/REPORT.md)' % (len(groups), len(structs), n, len(refused)))


def write(outdir, name, lines):
    open(os.path.join(outdir, name), 'w').write('\n'.join(lines) + '\n')


def pe_thunk(name, g):
    """The PE half: pack the fixed-layout struct, cross the seam, hand back the
    answer in whatever form MSVC expects it."""
    sh = g['shape']
    st = struct_name(sh)
    params = ''.join(', %s %s' % (t, n) for t, n, _ct, _cn in sh['pe_args'])
    if sh['kind'] == 'sret':
        params = ', uint64_t *sret' + params
        ret_t = 'uint64_t *'
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
    for i, (t, n, _ct, _cn) in enumerate(sh['pe_args']):
        if t == 'void *':
            L.append('    p.a%d = (uint64_t)(uintptr_t)%s;' % (i, n))
        else:
            L.append('    p.a%d = %s;' % (i, n))
    L.append('    seam(C_G_%s, &p);' % name)
    if sh['kind'] == 'sret':
        # MSVC's contract: fill the caller's hidden buffer and return THAT
        # pointer, not the value. Getting it wrong makes the caller dereference
        # a SteamID as an address (#11).
        L.append('    *sret = p.ret;')
        L.append('    return sret;')
    elif sh['kind'] == 'str':
        # The bytes are on the dylib's heap, above 4 GB. native_str is the
        # copy-down on i386 and the identity on x86_64 (#20).
        L.append('    return native_str(p.ret);')
    elif sh['kind'] == 'val':
        L.append('    return (%s)p.ret;' % sh['pe_ret'])
    L.append('}')
    return L


def unix_handler(name, g):
    """The unix half: index the native vtable with the slot the PE side sent and
    call through a typed function pointer. No class cast, so no transcription."""
    sh = g['shape']
    st = struct_name(sh)
    fnt = '%s (*)(void *%s)' % (sh['nat_ret'],
                                ''.join(', ' + t for t in sh['nat_args']))
    call = ''.join(
        ', (%s)%s' % (t, ('(uintptr_t)p->a%d' % i) if t == 'void *' else ('p->a%d' % i))
        for i, t in enumerate(sh['nat_args']))
    L = ['static NTSTATUS ug_%s(void *args)' % name]
    L.append('{')
    L.append('    auto *p = (struct %s *)args;' % st)
    # One line the FIRST time a title touches this method, and never again.
    # Silence is the failure mode this whole shim keeps relearning: #43 lost a
    # session to a cloud save that was invisible because nothing said which
    # method had answered 0. #45 fixed that for slots with no thunk; this is the
    # same answer for slots that now have one. Per-call logging is not an option
    # at this scale — a per-frame method would bury the log it belongs in — but
    # "the title used this, once" costs one branch and turns a run into a record
    # of which of ~1,100 generated methods a title actually exercises.
    L.append('    static bool first = true;')
    L.append('    if (first) { first = false; ulog("first call: %s::%s (slot %%d)", p->slot); }'
             % (g['iface'], g['method']))
    L.append('    auto fn = (%s)vslot(p->handle, p->slot);' % fnt)
    # A miss means the PE side could not name the slot. There is no safe guess:
    # the wrong slot on a 60-method interface is some other method entirely.
    L.append('    if (!fn) { ulog("%s::%s: slot %%d unresolvable — call dropped", '
             'p->slot); return 0; }' % (g['iface'], g['method']))
    if sh['kind'] == 'void':
        L.append('    fn((void *)(uintptr_t)p->handle%s);' % call)
    else:
        L.append('    p->ret = (%s)fn((void *)(uintptr_t)p->handle%s);'
                 % (CODE_C[sh['rcode']], call))
    L.append('    return 0;')
    L.append('}')
    return L


def report(outdir, tables, iface_of, groups, names, refused, nwired):
    ifaces = collections.defaultdict(lambda: [0, 0])   # iface -> [wired, refused]
    for g in groups.values():
        ifaces[g['iface']][0] += 1
    for (iface, _m), r in refused.items():
        ifaces[iface][1] += 1
    total_slots = sum(len(t['slots']) for t in tables.values())

    L = ['# Generated thunks: what was emitted, and what was refused', '',
         'GENERATED by `gen_thunks.py` (#78). Do not edit — regenerate with `./build.sh`.', '',
         'Every method below is one the emitter **declined**, with the reason. A refused',
         'slot keeps its logging stub, so a title that calls one still names it in',
         '`shim-unix.log` the first time it does (#45/#46) — the refusal is loud at build',
         'time and loud again at runtime, never silent.', '',
         '## Totals', '',
         '| | |', '| --- | --- |',
         '| interface versions | %d |' % len(tables),
         '| vtable slots | %d |' % total_slots,
         '| slots wired to a generated thunk | %d |' % nwired,
         '| distinct shapes emitted | %d |' % len(groups),
         '| methods refused | %d |' % len(refused), '',
         '## Per interface', '',
         '| interface | shapes emitted | methods refused |', '| --- | ---: | ---: |']
    for iface in sorted(ifaces):
        w, r = ifaces[iface]
        L.append('| `%s` | %d | %d |' % (iface, w, r))
    L += ['', '## Refusals', '',
          '| interface | method | versions | reason |', '| --- | --- | ---: | --- |']
    for (iface, method), r in sorted(refused.items()):
        L.append('| `%s` | `%s` | %d | %s |'
                 % (iface, method, len(set(r['versions'])), r['reason']))
    L.append('')
    write(outdir, 'REPORT.md', L)


main()
