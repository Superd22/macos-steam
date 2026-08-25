#!/bin/sh
# Regenerate the deploy contract and enforce it (#32).
#
#   gen/shim_paths.h   included by the C/C++ halves
#   gen/shim_paths.sh  dot-sourced by the shell halves
#
# Every other build.sh runs this first, so a manifest edit reaches whatever is
# being built without anyone remembering to regenerate. The check is part of the
# same step on purpose: generating the header while a module quietly keeps its
# own copy of the literal is the exact failure the manifest exists to end.
set -eu
cd "$(dirname "$0")"
python3 gen.py
python3 check.py
