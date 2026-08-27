#!/usr/bin/env python3
"""Every name the signed Steam client DLL imports from a shadowed library must be
one we export (ADR 0014).

The shadows are generated from the reference libraries' EXPORTS, which is a
superset of what any one client build imports -- so this normally passes with
room to spare. It exists for the case that does not: a Steam client update that
starts importing a name Valve's own library gained and ours was generated
before. The loader's answer to that is to fail the bind with no log line
anywhere, which is exactly the silent-wrong-answer failure #45 exists to kill.

    check_shadow.py <signed-client-dll> (<shadowed-dll-name> <its .def or built .dll>)...
"""
import re
import struct
import sys


def imports(path):
    """{dll -> [name, ...]} from a PE's import directory."""
    d = open(path, "rb").read()
    e = struct.unpack_from("<I", d, 0x3C)[0]
    nsec = struct.unpack_from("<H", d, e + 6)[0]
    szopt = struct.unpack_from("<H", d, e + 20)[0]
    magic = struct.unpack_from("<H", d, e + 24)[0]
    secs = []
    for i in range(nsec):
        o = e + 24 + szopt + i * 40
        vsz = struct.unpack_from("<I", d, o + 8)[0]
        va = struct.unpack_from("<I", d, o + 12)[0]
        rsz = struct.unpack_from("<I", d, o + 16)[0]
        raw = struct.unpack_from("<I", d, o + 20)[0]
        secs.append((va, max(vsz, rsz), raw))

    def off(rva):
        for va, sz, raw in secs:
            if va <= rva < va + sz:
                return raw + (rva - va)
        sys.exit("%s: RVA %#x maps to no section" % (path, rva))

    dd = e + 24 + (0x70 if magic == 0x20B else 0x60) + 8   # +8: import dir is entry 1
    imp = struct.unpack_from("<I", d, dd)[0]
    out, o = {}, off(imp)
    while True:
        olt, _, _, namerva, ft = struct.unpack_from("<IIIII", d, o)
        if not namerva:
            break
        no = off(namerva)
        dll = d[no:d.index(b"\0", no)].decode("ascii")
        names, t = [], off(olt or ft)
        while True:
            v = struct.unpack_from("<Q", d, t)[0]
            if not v:
                break
            if not v >> 63:                       # not an ordinal import
                ho = off(v & 0x7FFFFFFF)
                names.append(d[ho + 2:d.index(b"\0", ho + 2)].decode("ascii"))
            t += 8
        out[dll.lower()] = names
        o += 20
    return out


def pe_exports(path):
    """Exported names of a PE. Same walk as imports(), one directory over."""
    d = open(path, "rb").read()
    e = struct.unpack_from("<I", d, 0x3C)[0]
    nsec = struct.unpack_from("<H", d, e + 6)[0]
    szopt = struct.unpack_from("<H", d, e + 20)[0]
    magic = struct.unpack_from("<H", d, e + 24)[0]
    secs = []
    for i in range(nsec):
        o = e + 24 + szopt + i * 40
        vsz = struct.unpack_from("<I", d, o + 8)[0]
        va = struct.unpack_from("<I", d, o + 12)[0]
        rsz = struct.unpack_from("<I", d, o + 16)[0]
        raw = struct.unpack_from("<I", d, o + 20)[0]
        secs.append((va, max(vsz, rsz), raw))

    def off(rva):
        for va, sz, raw in secs:
            if va <= rva < va + sz:
                return raw + (rva - va)
        sys.exit("%s: RVA %#x maps to no section" % (path, rva))

    exp = struct.unpack_from("<I", d, e + 24 + (0x70 if magic == 0x20B else 0x60))[0]
    if not exp:
        sys.exit("%s: no export directory" % path)
    ed = off(exp)
    n = struct.unpack_from("<I", d, ed + 24)[0]
    names = struct.unpack_from("<I", d, ed + 32)[0]
    out = set()
    for i in range(n):
        rva = struct.unpack_from("<I", d, off(names) + i * 4)[0]
        o = off(rva)
        out.add(d[o:d.index(b"\0", o)].decode("ascii"))
    return out


def def_exports(path):
    out = set()
    for line in open(path):
        line = line.strip()
        if not line or line == "EXPORTS":
            continue
        out.add(re.split(r"\s*=\s*", line)[0])
    return out


def shadow_exports(path):
    """A .def before it is built, the built PE after. The second is what a user
    has, and is the one that decides whether their launch works."""
    with open(path, "rb") as f:
        return pe_exports(path) if f.read(2) == b"MZ" else def_exports(path)


def main():
    args = sys.argv[1:]
    if len(args) < 3 or len(args) % 2 != 1:
        sys.exit(__doc__)
    imp = imports(args[0])
    bad = 0
    for dll, defpath in zip(args[1::2], args[2::2]):
        want = imp.get(dll.lower())
        if want is None:
            bad += 1
            print("drm: %s imports NOTHING from %s -- this build's shadow would never"
                  % (args[0], dll))
            print("      be loaded, so the trampolines would never install and a wrapped")
            print("      title would run Valve's own client code. Re-read the import table.")
            continue
        have = shadow_exports(defpath)
        missing = [n for n in want if n not in have]
        if missing:
            bad += 1
            print("drm: %s imports %d name(s) from %s that the shadow does not export:"
                  % (args[0], len(missing), dll))
            for n in missing[:12]:
                print("      %s" % n)
            if len(missing) > 12:
                print("      ... and %d more" % (len(missing) - 12))
            print("      run ./build.sh --regen against this client build")
        else:
            print("drm: %s covers all %d imports from %s" % (defpath, len(want), dll))
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
