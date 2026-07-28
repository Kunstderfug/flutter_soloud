#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
binary="${TMPDIR:-/tmp}/flutter_soloud_capture_writer_test"

c++ -std=c++17 -O2 -Wall -Wextra -Werror \
  -DFLUTTER_SOLOUD_CAPTURE_TESTING \
  -DNO_XIPH_LIBS \
  -DNO_WAVPACK_LIBS \
  -I "$repo_root/src" \
  "$repo_root/test/capture_writer_test.cpp" \
  "$repo_root/src/capture/capture_processing.cpp" \
  "$repo_root/src/capture/capture_writer.cpp" \
  "$repo_root/src/capture/capture_mirror.cpp" \
  "$repo_root/src/capture/wavpack_capture_encoder.cpp" \
  -o "$binary"

"$binary"
echo "capture_writer_test: PASS"
