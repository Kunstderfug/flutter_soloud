#!/bin/bash
# Build and run the standalone mixing-bus sample-rate regression test.
# Run from the flutter_soloud repository root.

set -e

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO_ROOT"

OUT="${TMPDIR:-/tmp}/mixing_bus_sample_rate_test"

c++ -std=c++17 -O2 -Wall -DWITH_NULL \
    -I src -I src/soloud/include \
    -o "$OUT" \
    test/mixing_bus_sample_rate_test.cpp \
    src/soloud/src/core/*.cpp \
    src/soloud/src/backend/null/soloud_null.cpp \
    -pthread

"$OUT"
