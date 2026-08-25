#!/usr/bin/env python3
"""Drift guard for the deploy contract (#32).

The manifest is only worth having if restating one of its values fails loudly.
This walks the shipped sources and fails the build when a guarded literal —
a payload path, a payload basename, a deployed dir name — appears anywhere
outside layout.json and its generated output.

Comments are exempt on purpose: prose that names `C:\\shim\\steamclient64.dll`
while explaining why the registry value exists is documentation, not a second
copy of the contract. Only code is held to it.

    python3 check.py [root]     root defaults to src/
"""
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.dirname(HERE)
SKIP_DIRS = {"dist", "gen", "__pycache__"}
SCAN_EXT = {".c", ".cpp", ".h", ".hpp", ".sh", ".py", ".vdf", ".in"}


def guarded():
    with open(os.path.join(HERE, "layout.json")) as f:
        m = json.load(f)
    out = []
    for a in m["atoms"]:
        if a.get("guard"):
            out.append((a["name"], a["value"]))
    return out


def strip_c(text):
    """Blank out /* */ and // comments, keeping line count and code intact."""
    out, i, n = [], 0, len(text)
    while i < n:
        if text.startswith("/*", i):
            end = text.find("*/", i + 2)
            end = n if end < 0 else end + 2
            out.append(re.sub(r"[^\n]", " ", text[i:end]))
            i = end
        elif text.startswith("//", i):
            end = text.find("\n", i)
            end = n if end < 0 else end
            out.append(" " * (end - i))
            i = end
        else:
            out.append(text[i])
            i += 1
    return "".join(out)


def strip_hash(text):
    """Blank out whole-line # comments. A trailing # is left alone: deciding
    whether one is a comment needs a shell parser, and a guarded literal in a
    trailing comment is rare enough to be worth rewording when it trips."""
    lines = []
    for line in text.split("\n"):
        lines.append("" if line.lstrip().startswith("#") else line)
    return "\n".join(lines)


def strip_vdf(text):
    return "\n".join("" if l.lstrip().startswith("//") else l
                     for l in text.split("\n"))


def scan(path):
    ext = os.path.splitext(path)[1]
    if ext == ".in":
        ext = os.path.splitext(os.path.splitext(path)[0])[1]
    with open(path, encoding="utf-8", errors="replace") as f:
        text = f.read()
    if ext in (".c", ".cpp", ".h", ".hpp"):
        return strip_c(text)
    if ext == ".vdf":
        return strip_vdf(text)
    return strip_hash(text)


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else SRC
    names = guarded()
    hits = []
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = [d for d in dirnames if d not in SKIP_DIRS]
        for fn in filenames:
            path = os.path.join(dirpath, fn)
            if os.path.splitext(fn)[1] not in SCAN_EXT:
                continue
            if os.path.abspath(path) == os.path.join(HERE, "gen.py"):
                continue
            if os.path.abspath(path) == os.path.join(HERE, "check.py"):
                continue
            code = scan(path)
            for lineno, line in enumerate(code.split("\n"), 1):
                for name, value in names:
                    if value in line:
                        hits.append((os.path.relpath(path, os.path.dirname(SRC)),
                                     lineno, name, value, line.strip()))
    if hits:
        print("ERROR: the deploy contract is restated outside src/layout "
              "(#32). Use the generated name instead:", file=sys.stderr)
        for path, lineno, name, value, line in hits:
            print("  %s:%d  %r -> SHIM_PATH_%s" % (path, lineno, value, name),
                  file=sys.stderr)
            print("      %s" % line[:120], file=sys.stderr)
        print("\n  C/C++: #include \"shim_paths.h\"      sh: . .../shim_paths.sh",
              file=sys.stderr)
        return 1
    print("layout: %d guarded values, no drift" % len(names))
    return 0


if __name__ == "__main__":
    sys.exit(main())
