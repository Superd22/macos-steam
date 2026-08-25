#!/usr/bin/env python3
"""Emit the deploy contract (#32) in the two dialects that consume it.

    gen/shim_paths.h   C / C++ — narrow and wide string literals
    gen/shim_paths.sh  sh      — single-quoted assignments, dot-sourced

One generator, two outputs, one input: layout.json. Nothing here interprets a
path; it only escapes and joins, so adding a name to the manifest is the whole
edit. Run via build.sh, which also runs the drift guard.
"""
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
PREFIX = "SHIM_PATH_"


def load():
    with open(os.path.join(HERE, "layout.json")) as f:
        m = json.load(f)
    values = {}
    order = []
    for a in m["atoms"]:
        values[a["name"]] = a["value"]
        order.append((a["name"], a["value"], a["what"]))
    for j in m["joins"]:
        sep = "\\" if j["sep"] == "win" else "/"
        try:
            parts = [values[n] for n in j["of"]]
        except KeyError as e:
            sys.exit("layout.json: %s joins unknown name %s" % (j["name"], e))
        v = sep.join(parts)
        values[j["name"]] = v
        order.append((j["name"], v, j["what"]))
    return m, values, order


def c_escape(v):
    return v.replace("\\", "\\\\").replace('"', '\\"')


def emit_h(order):
    out = ["/* Generated from src/layout/layout.json by gen.py — do not edit.",
           " * The deploy contract (#32): every path the shipped stack agrees on.",
           " * Each name comes in a narrow and a wide (_W) form; the wide one is what",
           " * the PE side wants, since Win32's W entry points are the real ones.",
           " */",
           "#ifndef SHIM_PATHS_H",
           "#define SHIM_PATHS_H",
           ""]
    for name, value, what in order:
        out.append("/* %s */" % what)
        out.append('#define %s%-16s "%s"' % (PREFIX, name, c_escape(value)))
        out.append('#define %s%-16s L"%s"' % (PREFIX, name + "_W", c_escape(value)))
        out.append("")
    out.append("#endif /* SHIM_PATHS_H */")
    return "\n".join(out) + "\n"


def emit_sh(order):
    out = ["# Generated from src/layout/layout.json by gen.py — do not edit.",
           "# The deploy contract (#32). Dot-source it; every name is a plain",
           "# single-quoted assignment, so it is also safe to grep or parse.",
           ""]
    for name, value, what in order:
        if "'" in value:
            sys.exit("layout.json: %s contains a single quote, which the sh "
                     "fragment's quoting cannot carry" % name)
        out.append("# %s" % what)
        out.append("%s%s='%s'" % (PREFIX, name, value))
        out.append("")
    return "\n".join(out)


def main():
    _, _, order = load()
    gen = os.path.join(HERE, "gen")
    os.makedirs(gen, exist_ok=True)
    for base, text in (("shim_paths.h", emit_h(order)),
                       ("shim_paths.sh", emit_sh(order))):
        path = os.path.join(gen, base)
        # Only rewrite on change: these are build inputs, and a fresh mtime on
        # every run would make install.sh's staleness check rebuild the world.
        old = None
        if os.path.exists(path):
            with open(path) as f:
                old = f.read()
        if old != text:
            with open(path, "w") as f:
                f.write(text)
            print("layout: wrote gen/%s (%d names)" % (base, len(order)))


if __name__ == "__main__":
    main()
