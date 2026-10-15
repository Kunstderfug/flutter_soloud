#!/bin/bash
# Build and run the standalone native retroactive re-mix tests (Phase 3a of
# the retroactive re-mixing feature). Uses the null backend, so no audio
# hardware is needed.

set -e

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO_ROOT"
source test/native_decoder_build_flags.sh

OUT="${TMPDIR:-/tmp}/retroactive_remix_test"

c++ -std=c++17 -O2 -Wall -Wextra -pthread \
    -DWITH_NULL \
    -DNO_XIPH_LIBS \
    -I src/soloud/include \
    "${NATIVE_DECODER_FLAGS[@]}" \
    "${NATIVE_DECODER_PLATFORM_SOURCES[@]}" \
    -o "$OUT" \
    test/retroactive_remix_test.cpp \
    src/soloud/src/core/*.cpp \
    src/soloud/src/audiosource/wav/*.cpp \
    src/audiobuffer/mp3_stream_decoder.cpp \
    src/audiobuffer/aac_stream_decoder.cpp \
    src/audiobuffer/m4a_metadata.cpp \
    src/soloud/src/backend/null/soloud_null.cpp

"$OUT"
