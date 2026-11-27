#!/bin/bash
# Run the fork's standalone native tests (test/fork/run_*_test.sh) and print
# a PASS/FAIL/SKIPPED summary. The fork's Dart tests in this folder run with
# `flutter test`. See FORK_MAINTENANCE.md.
#   ./test/fork/all.sh [-- --asan|--tsan]
exec "$(dirname "$0")/../support/run_native_tests.sh" test/fork "$@"
