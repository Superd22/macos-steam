#!/usr/bin/env python3
"""Stamp the Wine-builtin marker into a PE's DOS stub, in place.

winebuild writes "Wine builtin DLL\0" at file offset 0x40 (inside the DOS
stub, after the 0x40-byte DOS header). ntdll checks exactly that to decide
a PE is a builtin and go hunting for its x86_64-unix/<name>.so sibling.
The stub code we overwrite is only executed under real DOS, i.e. never.
"""

import sys

MARKER = b"Wine builtin DLL\0"
OFFSET = 0x40

path = sys.argv[1]
data = bytearray(open(path, "rb").read())

e_lfanew = int.from_bytes(data[0x3C:0x40], "little")
assert e_lfanew >= OFFSET + len(MARKER), (
    f"PE header at 0x{e_lfanew:x} overlaps the marker area; refusing"
)
assert data[:2] == b"MZ"

data[OFFSET : OFFSET + len(MARKER)] = MARKER
open(path, "wb").write(bytes(data))
print(f"stamped {path}: '{MARKER[:-1].decode()}' at 0x{OFFSET:x} (e_lfanew=0x{e_lfanew:x})")
