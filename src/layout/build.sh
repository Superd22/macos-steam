#!/bin/sh
# Regenerate the deploy contract and enforce it (#32).
#
#   gen/shim_paths.h   included by the C/C++ halves
#   gen/shim_paths.sh  dot-sourced by the shell halves
#   gen/shim_policy.h  the runtime switch predicates, C/C++ side (#33)
#   gen/shim_policy.sh the same predicates as shell functions
#
# Every other build.sh runs this first, so a manifest edit reaches whatever is
# being built without anyone remembering to regenerate. The check is part of the
# same step on purpose: generating the header while a module quietly keeps its
# own copy of the literal is the exact failure the manifest exists to end.
#
# check_policy.py is the same argument one level in: the switch predicate is
# written twice, once per dialect, so the two are run against the same values
# and must agree. Two emitters of one rule is how #33 happened in the first
# place, at a larger scale.
set -eu
cd "$(dirname "$0")"
python3 gen.py
python3 check.py
python3 check_policy.py
