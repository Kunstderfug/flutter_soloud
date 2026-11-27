#!/bin/bash
# The fork's native sources and defines, added to the shared test builds
# (engine_sources.sh, plugin_build.sh) through one marked line each, so the
# upstream source lists stay as they are. Source it from the repository root,
# don't run it; the globs expand when it is sourced.
#
#   . test/support/fork_sources.sh   # sets FORK_SOURCES, FORK_DEFINES
#
# Native capture builds without the WavPack C sources here (NO_WAVPACK_LIBS).

FORK_SOURCES=(
    src/fork/*.cpp
    src/capture/*.cpp
)
FORK_DEFINES=(-DNO_WAVPACK_LIBS)
