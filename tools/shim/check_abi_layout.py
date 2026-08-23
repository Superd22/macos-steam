#!/usr/bin/env python3
"""Prove the seam ABI is bitness-neutral, rather than assuming it (#20).

The 32-bit shim needs no ptr32<T> conversion layer — the thing Proton's
lsteamclient spends a whole generated wow64 surface on — for one reason: every
params struct in shim_abi.h stores pointers as an explicit uint64_t and is laid
out widest-first, so a 32-bit PE zero-extends its pointers into fields that sit
at exactly the same offsets a 64-bit PE uses. (Zero-extension is the correct
value, not a truncation, because new WoW64 runs the 32-bit PE inside the 64-bit
unix process: its addresses ARE low addresses in that one address space.)

That is a property of the header, and someone will eventually add a struct with
a bare pointer or an odd field order and quietly break it. So compare every
field offset under i686-mingw against the 64-bit layout, at build time.
"""
import re, subprocess, sys, tempfile, os

HDR = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'shim_abi.h')

def structs():
    src = re.sub(r'/\*.*?\*/', '', open(HDR).read(), flags=re.S)
    out = []
    for name, body in re.findall(r'struct\s+(sp_\w+)\s*\{([^}]*)\}', src):
        fields = []
        for decl in body.split(';'):
            decl = decl.strip()
            if not decl:
                continue
            for v in ' '.join(decl.split()[1:]).split(','):
                if v.strip():
                    fields.append(v.strip())
        out.append((name, fields))
    return out

def main():
    defs = structs()
    d = tempfile.mkdtemp()
    probe = os.path.join(d, 'probe.c')
    with open(probe, 'w') as f:
        f.write('#include <stdio.h>\n#include <stddef.h>\n#include "shim_abi.h"\nint main(void){\n')
        for n, fs in defs:
            f.write('printf("%s %%zu\\n", sizeof(struct %s));\n' % (n, n))
            for x in fs:
                f.write('printf("%s.%s %%zu\\n", offsetof(struct %s, %s));\n' % (n, x, n, x))
        f.write('return 0;}\n')

    inc = os.path.dirname(HDR)
    exe = os.path.join(d, 'probe')
    subprocess.run(['clang', '-I', inc, '-o', exe, probe], check=True)
    ref = subprocess.run([exe], capture_output=True, text=True, check=True).stdout.split('\n')

    # Re-assert the same numbers under the 32-bit compiler. A mismatch is a
    # compile error naming the exact field.
    asserts = os.path.join(d, 'assert.c')
    n = 0
    with open(asserts, 'w') as f:
        f.write('#include <stddef.h>\n#include "shim_abi.h"\n')
        for line in ref:
            if not line.strip():
                continue
            what, val = line.split()
            if '.' in what:
                s, fld = what.split('.', 1)
                f.write('_Static_assert(offsetof(struct %s, %s) == %s, "%s moved under i686");\n'
                        % (s, fld, val, what))
            else:
                f.write('_Static_assert(sizeof(struct %s) == %s, "%s resized under i686");\n'
                        % (what, val, what))
            n += 1
    r = subprocess.run(['i686-w64-mingw32-gcc', '-I', inc, '-c', asserts,
                        '-o', os.path.join(d, 'a.o')], capture_output=True, text=True)
    if r.returncode:
        print(r.stderr, file=sys.stderr)
        sys.exit('seam ABI is NOT bitness-neutral — see the failed _Static_assert above.\n'
                 'A params struct must store pointers as uint64_t and be laid out widest-first.')
    print('seam ABI bitness-neutral: %d sizes/offsets identical under i686 and x86_64' % n)

main()
