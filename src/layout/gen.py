#!/usr/bin/env python3
"""Emit the deploy contract (#32) in the three dialects that consume it.

    gen/shim_paths.h    C / C++ — narrow and wide string literals
    gen/shim_paths.sh   sh      — single-quoted assignments, dot-sourced
    gen/ShimPaths.swift Swift   — the same names, for the launcher app (#42)
    gen/shim_policy.h   C / C++ — one predicate per runtime switch (#33)
    gen/shim_policy.sh  sh      — the same predicates, as shell functions
    gen/ShimPolicy.swift Swift  — the same predicate, third dialect

One generator, one input: layout.json. Nothing here interprets a path; it only
escapes and joins, and the switch half only spells one rule out in two dialects,
so adding a name or a switch to the manifest is the whole edit. Run via
build.sh, which also runs the drift guard.
"""
import json
import os
import sys
import textwrap

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


# --- an .app somebody else installed ------------------------------------------
# Distinct from every other path here, which we deploy and therefore place. A
# user's .app can sit in EITHER Applications dir — /Applications from a DMG
# drag, ~/Applications from a per-user install — and neither is the odd one out.
# Guessing one and calling the other absent is a bug this repo has now shipped
# twice, so the search order is written once and generated into both dialects.
#
# $HOME first, because a per-user copy is the more specific answer when both
# exist. With neither, the $HOME spelling comes back so an error message can
# name a path instead of an empty string.
_APP_RESOLVER_WHY = [
    "# An .app the USER installed, resolved rather than assumed: it can be in",
    "# ~/Applications or /Applications and both are normal. First existing wins,",
    "# $HOME first. With neither, the $HOME spelling comes back, so a caller",
    "# reporting 'not found' still has a path to name.",
]
APP_RESOLVER_DOC_SH = _APP_RESOLVER_WHY


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
    out += APP_RESOLVER_DOC_SH + [
        "shim_installed_app() {",
        '    if [ -d "$HOME/$1" ]; then printf \'%s\' "$HOME/$1"',
        '    elif [ -d "/$1" ]; then printf \'%s\' "/$1"',
        '    else printf \'%s\' "$HOME/$1"',
        "    fi",
        "}",
        ""]
    return "\n".join(out)


# --- switches: the runtime policy half (#33) ---------------------------------
# A switch is one env var, one default, one predicate name. The point is not to
# save typing — it is that "unset means ON" is a rule with an owner, so no
# caller can hold a different opinion about it. Before this, five sites derived
# the overlay switch and one of them tested the variable's PRESENCE, which reads
# an explicit `SHIM_OVERLAY=0` as ON.


def wrap_what(what):
    """Fold a manifest `what` into comment-width lines. The prose is the only
    explanation a reader of the generated file gets, so it is carried over
    rather than dropped."""
    return textwrap.wrap(what, 74) or [""]


def check_switch(sw):
    """The emitters below spell out `default, then the off values, else on`. A
    default of `off` would need the mirror image — an on-value list — so it is
    rejected here rather than emitted as something that reads right and answers
    wrong, which is the failure mode #33 is about."""
    if sw["default"] != "on":
        sys.exit("layout.json: switch %s has default %r; only 'on' is emitted "
                 "(an off-by-default switch needs on_values, not off_values)"
                 % (sw["name"], sw["default"]))


def emit_policy_h(switches):
    out = ["/* Generated from src/layout/layout.json by gen.py — do not edit.",
           " * The runtime switches (#33): one predicate per switch, so every half of",
           " * the stack answers `is the overlay on?` with the same code rather than",
           " * with five independently-written env tests.",
           " *",
           " * Both dialects of the same question live here: the unix halves read the",
           " * environment with getenv, the PE half must use GetEnvironmentVariableA",
           " * (a DLL injected into a title cannot count on that process's CRT having",
           " * been started with the environment we care about). Reading is what",
           " * differs between them; the rule must not.",
           " */",
           "#ifndef SHIM_POLICY_H",
           "#define SHIM_POLICY_H",
           "",
           "#include <string.h>",
           "#ifdef _WIN32",
           "# include <windows.h>",
           "#else",
           "# include <stdlib.h>",
           "#endif",
           "",
           "/* Returns the variable's value, or NULL when it is not set at all —",
           " * a distinction the predicates below depend on, since unset is what the",
           " * manifest's default answers and empty is a stated (off) value. A value",
           " * too long for the buffer cannot be one of the off values, so it comes",
           " * back as a placeholder rather than as unset. */",
           "static inline const char *shim_policy_read_(const char *name,",
           "                                            char *buf, unsigned cap)",
           "{",
           "#ifdef _WIN32",
           "    DWORD n = GetEnvironmentVariableA(name, buf, (DWORD)cap);",
           "    if (n == 0)",
           "        return GetLastError() == ERROR_ENVVAR_NOT_FOUND ? (const char *)0 : \"\";",
           "    if (n >= cap) return \"(long)\";",
           "    return buf;",
           "#else",
           "    (void)buf; (void)cap;",
           "    return getenv(name);",
           "#endif",
           "}",
           ""]
    for sw in switches:
        check_switch(sw)
        out += ["/* %s" % sw["env"]]
        out += [" * " + l for l in wrap_what(sw["what"])]
        out += [" *",
                " * Default when unset: %s. Off values: %s." %
                (sw["default"],
                 ", ".join(repr(v) if v else "empty" for v in sw["off_values"])),
                " */",
                '#define SHIM_ENV_%s %s' % (sw["name"], '"%s"' % sw["env"])]
        veto = sw.get("veto")
        if veto:
            out += ['#define SHIM_VETO_%s %s' % (sw["name"], '"%s"' % veto["env"])]
        out += ["static inline int %s(void)" % sw["predicate"],
                "{",
                "    char buf[32];",
                "    const char *v = shim_policy_read_(SHIM_ENV_%s, buf, sizeof buf);" % sw["name"],
                "    if (v) {   /* unset is the manifest default, so it falls through */"]
        for val in sw["off_values"]:
            if val == "":
                out.append("        if (!*v) return 0;")
            else:
                out.append('        if (strcmp(v, "%s") == 0) return 0;' % c_escape(val))
        out += ["    }"]
        if veto:
            out += ["    {",
                    "        char vbuf[32];",
                    "        const char *w = shim_policy_read_(SHIM_VETO_%s, vbuf, sizeof vbuf);" % sw["name"],
                    "        if (w) {"]
            for val in veto["on_values"]:
                out.append('            if (strcmp(w, "%s") == 0) return 0;' % c_escape(val))
            out += ["        }",
                    "    }"]
        out += ["    return 1;",
                "}",
                ""]
    out.append("#endif /* SHIM_POLICY_H */")
    return "\n".join(out) + "\n"


def emit_policy_sh(switches):
    out = ["# Generated from src/layout/layout.json by gen.py — do not edit.",
           "# The runtime switches (#33). Dot-source it and call the predicate;",
           "# `${VAR-default}` (no colon) is deliberate — it distinguishes unset,",
           "# which the manifest's default answers, from an explicitly empty value,",
           "# which is a stated `off`.",
           ""]
    for sw in switches:
        check_switch(sw)
        pattern = "|".join("''" if v == "" else v for v in sw["off_values"])
        out += ["# %s" % l for l in wrap_what(sw["what"])]
        out += ["SHIM_ENV_%s='%s'" % (sw["name"], sw["env"])]
        veto = sw.get("veto")
        if veto:
            out += ["SHIM_VETO_%s='%s'" % (sw["name"], veto["env"])]
        out += ["%s() {" % sw["predicate"],
                '    case "${%s-1}" in' % sw["env"],
                "        %s) return 1 ;;" % pattern,
                "    esac"]
        if veto:
            out += ["    # " + l for l in textwrap.wrap(veto["what"], 70)]
            out += ['    case "${%s-}" in' % veto["env"],
                    "        %s) return 1 ;;" % "|".join(veto["on_values"]),
                    "    esac"]
        out += ["    return 0",
                "}"]
        if veto:
            # Diagnostics only. The predicate above is the decision; this says
            # WHICH input made it, so a launch log can tell "the user unticked
            # Steam's box" apart from "SHIM_OVERLAY=0" apart from "no injector".
            # Without it the one site that must explain itself would have to
            # test the veto variable directly, which is the re-derivation the
            # drift guard exists to prevent.
            out += ["%s() {" % (sw["predicate"].replace("_enabled", "_vetoed")),
                    '    case "${%s-}" in' % veto["env"],
                    "        %s) return 0 ;;" % "|".join(veto["on_values"]),
                    "    esac",
                    "    return 1",
                    "}"]
        out += [
                "# State the answer to every child process, never imply it: below this",
                "# point unset means the manifest default, so omitting the variable",
                "# cannot express the non-default answer.",
                "%s_export() {" % sw["predicate"].replace("_enabled", ""),
                '    if [ "${1:-}" = 0 ]; then %s=0; else %s=1; fi' % (sw["env"], sw["env"]),
                "    export %s" % sw["env"],
                "}",
                ""]
    return "\n".join(out)


# --- Swift: the launcher app's dialect (#42) ---------------------------------
# The launcher is a Mach-O in the ship-set like any other module, so it reads
# the contract rather than restating it — check.py holds .swift to the same
# guard as .c and .sh. An enum with static lets and no cases is the Swift idiom
# for a namespace that cannot be instantiated.


def swift_escape(v):
    return v.replace("\\", "\\\\").replace('"', '\\"')


def swift_ident(name):
    """SHIM_PATH_STEAM_OSX_REL -> steamOsxRel. The generated names are the same
    names in a different dialect, not different names: a reader grepping the
    manifest for STEAM_OSX_REL must land here too, which the doc comment on
    each member preserves."""
    head, *rest = name.lower().split("_")
    return head + "".join(w.capitalize() for w in rest)


def emit_paths_swift(order):
    out = ["// Generated from src/layout/layout.json by gen.py — do not edit.",
           "// The deploy contract (#32), for the launcher app (#42). Same names as",
           "// shim_paths.h and shim_paths.sh, lowerCamelCased; the manifest name each",
           "// one came from is on the line above it, so a grep for either spelling lands.",
           "",
           "import Foundation",
           "",
           "enum ShimPath {"]
    for name, value, what in order:
        out.append("    /// %s" % what)
        out.append("    /// Manifest: `%s%s`" % (PREFIX, name))
        out.append('    static let %s = "%s"' % (swift_ident(name), swift_escape(value)))
        out.append("")
    out += ["    /// Every *_REL name is relative to a root the caller supplies — $HOME for",
            "    /// the macOS ones. The manifest deliberately holds no absolute unix path,",
            "    /// so joining one is the caller's job and this is the only way to do it.",
            "    static func inHome(_ rel: String) -> String {",
            "        (NSHomeDirectory() as NSString).appendingPathComponent(rel)",
            "    }",
            ""] + ["    /// " + l[2:] for l in _APP_RESOLVER_WHY] + [
            "    static func installedApp(_ rel: String) -> String {",
            "        let home = inHome(rel)",
            "        if FileManager.default.fileExists(atPath: home) { return home }",
            "        if FileManager.default.fileExists(atPath: \"/\" + rel) { return \"/\" + rel }",
            "        return home",
            "    }",
            "}"]
    return "\n".join(out) + "\n"


def emit_policy_swift(switches):
    out = ["// Generated from src/layout/layout.json by gen.py — do not edit.",
           "// The runtime switches (#33, ADR 0006), third dialect. check_policy.py runs",
           "// this one against the C and sh emitters on the same table of values: three",
           "// hand-written emitters of one rule is two more than a rule can have before",
           "// it drifts, so they are made to answer identically instead.",
           "",
           "import Foundation",
           "",
           "enum ShimPolicy {"]
    for sw in switches:
        check_switch(sw)
        out += ["    /// " + l for l in wrap_what(sw["what"])]
        out += ["    ///",
                "    /// Default when unset: %s. Off values: %s." %
                (sw["default"],
                 ", ".join(repr(v) if v else "empty" for v in sw["off_values"])),
                '    static let env%s = "%s"' % (sw["name"].capitalize(), sw["env"])]
        veto = sw.get("veto")
        if veto:
            out += ["    /// " + l for l in wrap_what(veto["what"])]
            out += ['    static let veto%s = "%s"' % (sw["name"].capitalize(), veto["env"])]
        out += ["",
                "    /// Reads the rule, never re-derives it. `env` defaults to the process",
                "    /// environment; passing one in is what lets the launcher decide what a",
                "    /// CHILD will see before it spawns it.",
                "    static func %s(in env: [String: String] = ProcessInfo.processInfo.environment) -> Bool {" % swift_ident(sw["predicate"]),
                "        if let v = env[env%s] {   // unset is the default, so it falls through" % sw["name"].capitalize()]
        for val in sw["off_values"]:
            if val == "":
                out.append('            if v.isEmpty { return false }')
            else:
                out.append('            if v == "%s" { return false }' % swift_escape(val))
        out += ["        }"]
        if veto:
            out += ["        if let w = env[veto%s] {" % sw["name"].capitalize()]
            for val in veto["on_values"]:
                out.append('            if w == "%s" { return false }' % swift_escape(val))
            out += ["        }"]
        out += ["        return true",
                "    }",
                "",
                "    /// State the answer to every child process, never imply it: below this",
                "    /// point unset means the manifest default, so omitting the variable",
                "    /// cannot express the non-default answer.",
                "    static func %sExport(_ on: Bool, into env: inout [String: String]) {" % swift_ident(sw["name"].lower()),
                '        env[env%s] = on ? "1" : "0"' % sw["name"].capitalize(),
                "    }",
                ""]
    out.append("}")
    return "\n".join(out) + "\n"


def main():
    m, _, order = load()
    switches = m.get("switches", [])
    gen = os.path.join(HERE, "gen")
    os.makedirs(gen, exist_ok=True)
    for base, text in (("shim_paths.h", emit_h(order)),
                       ("shim_paths.sh", emit_sh(order)),
                       ("ShimPaths.swift", emit_paths_swift(order)),
                       ("shim_policy.h", emit_policy_h(switches)),
                       ("shim_policy.sh", emit_policy_sh(switches)),
                       ("ShimPolicy.swift", emit_policy_swift(switches))):
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
            n = len(switches) if "olicy" in base else len(order)
            print("layout: wrote gen/%s (%d names)" % (base, n))


if __name__ == "__main__":
    main()
