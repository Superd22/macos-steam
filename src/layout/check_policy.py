#!/usr/bin/env python3
"""Parity guard for the runtime switches (#33).

One rule, three hand-written emitters — which is two more than a rule can
have before it drifts, and drift between two dialects of the same predicate is
the exact defect this module was built to end (`GetEnvironmentVariableA(...,
NULL, 0) > 0` read an explicit `SHIM_OVERLAY=0` as ON, while the shell test two
files away read it as OFF).

So the two generated predicates are run against the same table of values and
must agree, value for value, including the three that are easy to get wrong:
unset, empty, and something that is neither the default nor an off value.

The C side is compiled for the host, so it is the getenv branch that is
executed here. The Win32 branch differs only in how it READS the environment,
and both branches feed the same generated comparisons below.

Swift joined in #42, when the launcher app became a third reader. It is checked
only when a Swift toolchain is present: every other build.sh calls this one, and
a machine that can build the C halves but not the launcher must still be able to
build the C halves. A skip says so out loud rather than passing quietly.

    python3 check_policy.py
"""
import json
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
GEN = os.path.join(HERE, "gen")

# unset is represented by None; everything else is a literal value to set.
CASES = [None, "", "0", "1", "2", "00", "0x", "no", "off", "true", "yes"]

PROBE = """
#include <stdio.h>
#include "shim_policy.h"
int main(void) { printf("%d\\n", PRED()); return 0; }
"""

SWIFT_PROBE = """
import Foundation
print(ShimPolicy.PRED() ? 1 : 0)
"""


def switches():
    with open(os.path.join(HERE, "layout.json")) as f:
        return json.load(f).get("switches", [])


def c_answers(sw, cc, tmp):
    src = os.path.join(tmp, "probe.c")
    exe = os.path.join(tmp, "probe")
    with open(src, "w") as f:
        f.write(PROBE.replace("PRED", sw["predicate"]))
    subprocess.check_call([cc, "-I", GEN, "-Wall", "-Werror", "-o", exe, src])
    out = []
    for v in CASES:
        env = dict(os.environ)
        env.pop(sw["env"], None)
        if v is not None:
            env[sw["env"]] = v
        out.append(subprocess.check_output([exe], env=env).decode().strip())
    return out


def sh_answers(sw, tmp):
    """Each case in its own `sh -c`: an unset variable cannot be expressed by
    assigning to it, and leaking one case's value into the next would make the
    unset case untestable — which is the case the divergence lived in."""
    out = []
    for v in CASES:
        script = ". '%s/shim_policy.sh'; %s && echo 1 || echo 0" % (
            GEN, sw["predicate"])
        env = dict(os.environ)
        env.pop(sw["env"], None)
        if v is not None:
            env[sw["env"]] = v
        out.append(subprocess.check_output(["sh", "-c", script],
                                           env=env).decode().strip())
    return out


def swift_ident(name):
    head, *rest = name.lower().split("_")
    return head + "".join(w.capitalize() for w in rest)


def swift_answers(sw, tmp):
    """Compiled once, run per case — the predicate reads the environment at
    call time, so the same binary answers every case."""
    # main.swift and not probe.swift: top-level code is only legal in a file
    # with that name, which is also why the launcher's entry point is one.
    src = os.path.join(tmp, "main.swift")
    exe = os.path.join(tmp, "probe-swift")
    with open(src, "w") as f:
        f.write(SWIFT_PROBE.replace("PRED", swift_ident(sw["predicate"])))
    subprocess.check_call(["swiftc", "-O", "-o", exe, src,
                           os.path.join(GEN, "ShimPolicy.swift")],
                          stdout=subprocess.DEVNULL)
    out = []
    for v in CASES:
        env = dict(os.environ)
        env.pop(sw["env"], None)
        if v is not None:
            env[sw["env"]] = v
        out.append(subprocess.check_output([exe], env=env).decode().strip())
    return out


def have_swift():
    try:
        subprocess.check_call(["swiftc", "--version"],
                              stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        return True
    except (OSError, subprocess.CalledProcessError):
        return False


def main():
    cc = os.environ.get("CC", "cc")
    swift = have_swift()
    bad = 0
    for sw in switches():
        with tempfile.TemporaryDirectory() as tmp:
            answers = {"C": c_answers(sw, cc, tmp), "sh": sh_answers(sw, tmp)}
            if swift:
                answers["Swift"] = swift_answers(sw, tmp)
        dialects = sorted(answers)
        for i, value in enumerate(CASES):
            given = {d: answers[d][i] for d in dialects}
            if len(set(given.values())) > 1:
                bad += 1
                print("ERROR: %s disagrees for %s=%s: %s"
                      % (sw["predicate"], sw["env"],
                         "(unset)" if value is None else repr(value),
                         ", ".join("%s says %s" % (d, given[d]) for d in dialects)),
                      file=sys.stderr)
        if not bad:
            print("layout: %s agrees across %s on %d values"
                  % (sw["predicate"], " and ".join(dialects), len(CASES)))
            if not swift:
                print("layout: Swift not checked (no swiftc on this machine)")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
