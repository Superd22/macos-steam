#!/usr/bin/env python3
"""Parity guard for the runtime switches (#33).

One rule, two hand-written emitters — which is one emitter more than a rule can
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


def main():
    cc = os.environ.get("CC", "cc")
    bad = 0
    for sw in switches():
        with tempfile.TemporaryDirectory() as tmp:
            c = c_answers(sw, cc, tmp)
            sh = sh_answers(sw, tmp)
        for value, a, b in zip(CASES, c, sh):
            if a != b:
                bad += 1
                print("ERROR: %s disagrees for %s=%s: C says %s, sh says %s"
                      % (sw["predicate"], sw["env"],
                         "(unset)" if value is None else repr(value), a, b),
                      file=sys.stderr)
        if not bad:
            print("layout: %s agrees across C and sh on %d values"
                  % (sw["predicate"], len(CASES)))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
