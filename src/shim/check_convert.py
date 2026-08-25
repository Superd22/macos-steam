#!/usr/bin/env python3
"""Prove the generated layout converters copy every field (ADR 0009).

A converter for one of the few structs whose Windows and unix layouts genuinely
differ is a field-by-field copy, generated from Proton's two field lists. The
failure mode if it drops a field is the one this project keeps meeting: the call
succeeds, the struct comes back, and one member holds whatever was in the
caller's buffer beforehand. No error, a plausible value, and a title acting on
it.

So this does not read the generated converter. It reads Proton's field list from
structs.json — the same source, independently — and emits a probe that:

  fills the Windows-layout struct with pattern A
  fills the destination with pattern B
  runs w2u then u2w
  compares EVERY field Proton declares

A field the converter never copies keeps pattern B and is caught. That works
precisely because the check derives its comparison from the declaration rather
than from the converter, so the two would have to be wrong in the same way.

Usage: check_convert.py <structs.json> <gen-dir>
"""
import json, os, re, subprocess, sys, tempfile


def main():
    structs = json.load(open(sys.argv[1]))
    gendir = os.path.abspath(sys.argv[2])
    conv = open(os.path.join(gendir, 'shim_gen_convert.h')).read()
    names = sorted(set(re.findall(r'static void cvt_w2u_(\w+)\(', conv)))
    if not names:
        print('converters: none generated, nothing to check')
        return

    L = ['#include <cstdint>', '#include <cstdio>', '#include <cstring>',
         '#include "gen/shim_gen_structs.h"', '#include "gen/shim_gen_convert.h"',
         'static int bad;',
         'static void chk(const char *s, const char *f, int ok)',
         '{ if (!ok) { printf("  %s.%s NOT COPIED\\n", s, f); bad++; } }',
         'int main(void) {']
    nfields = 0
    for n in names:
        w, u = structs['structs']['w64_' + n], structs['structs']['u64_' + n]
        L.append('  { struct w64_%s a, b; struct u64_%s m;' % (n, n))
        L.append('    memset(&a, 0xA5, sizeof a); memset(&b, 0x3C, sizeof b);')
        L.append('    memset(&m, 0x00, sizeof m);')
        L.append('    cvt_w2u_%s(&a, &m); cvt_u2w_%s(&m, &b);' % (n, n))
        for typ, fld, cnt in w['fields']:
            nfields += 1
            if cnt:
                L.append('    chk("%s", "%s", !memcmp(a.%s, b.%s, sizeof a.%s));'
                         % (n, fld, fld, fld, fld))
            else:
                L.append('    chk("%s", "%s", !memcmp(&a.%s, &b.%s, sizeof a.%s));'
                         % (n, fld, fld, fld, fld))
        L.append('  }')
    L.append('  return bad; }')

    d = tempfile.mkdtemp()
    src = os.path.join(d, 'probe.cpp')
    open(src, 'w').write('\n'.join(L) + '\n')
    exe = os.path.join(d, 'probe')
    inc = os.path.dirname(gendir)
    r = subprocess.run(['clang++', '-std=c++17', '-I', inc, '-o', exe, src],
                       capture_output=True, text=True)
    if r.returncode:
        print(r.stderr, file=sys.stderr)
        sys.exit('converter probe did not compile')
    r = subprocess.run([exe], capture_output=True, text=True)
    sys.stdout.write(r.stdout)
    if r.returncode:
        sys.exit('layout converters DROP fields — see above. A dropped field '
                 'leaves the caller reading whatever was in its buffer before '
                 'the call, which is a plausible value and not an error.')
    print('converters: %d structs, %d fields round-trip w64 -> u64 -> w64 intact'
          % (len(names), nfields))


main()
