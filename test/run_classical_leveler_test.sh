#!/bin/bash
set -e
REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO_ROOT"
OUT="${TMPDIR:-/tmp}/classical_leveler_test"
c++ -std=c++17 -O2 -Wall -I src/soloud/include -o "$OUT" \
  test/classical_leveler_test.cpp \
  src/filters/classical_leveler_filter.cpp \
  src/filters/classical_leveler_processor.cpp \
  src/soloud/src/core/soloud_filter.cpp \
  src/soloud/src/core/soloud_fader.cpp
"$OUT"
