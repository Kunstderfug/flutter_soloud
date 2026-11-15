#!/bin/bash
# Build and run the standalone native CLAP host tests (macOS only):
#
#   ./test/run_clap_host_test.sh [--asan|--tsan] [test name]
#
# It builds the host core (src/clap/*.cpp) and its C ABI
# (src/clap_bindings.cpp) with SOLOUD_CLAP_TEST_HOOKS, the
# harness (test/clap_host_test.cpp) and the mock bundles
# (test/clap_mock_plugin.cpp), then runs the harness. No Flutter, SoLoud
# engine or SDK download is needed: the CLAP headers are vendored.
#
# --asan builds under AddressSanitizer and UndefinedBehaviorSanitizer,
# --tsan under ThreadSanitizer.
#
# Elsewhere it prints a SKIP line and exits 0.

set -e

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO_ROOT"

if [ "$(uname)" != "Darwin" ]; then
    echo "SKIP: The CLAP host tests run on macOS only."
    exit 0
fi

CLAP_INCLUDE="$REPO_ROOT/src/clap/third_party/clap/include"
SANITIZER=(-O1 -g)
WORK="${TMPDIR:-/tmp}/clap_host_test"
case "$1" in
    --asan)
        SANITIZER=(-O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
        WORK="${WORK}_asan"
        shift
        ;;
    --tsan)
        SANITIZER=(-O1 -g -fsanitize=thread)
        WORK="${WORK}_tsan"
        shift
        ;;
esac
rm -rf "$WORK"
mkdir -p "$WORK"

make_bundle() {
    local bundle="$1" name="$2"
    mkdir -p "$bundle/Contents/MacOS"
    cat > "$bundle/Contents/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleExecutable</key>
    <string>$name</string>
    <key>CFBundleIdentifier</key>
    <string>dev.flutter-soloud.test.$name</string>
    <key>CFBundlePackageType</key>
    <string>BNDL</string>
</dict>
</plist>
EOF
}

build_mock() {
    local name="$1"
    shift
    make_bundle "$WORK/$name.clap" "$name"
    c++ -std=c++17 -Wall -Wextra -bundle "${SANITIZER[@]}" "$@" \
        -I test \
        -isystem "$CLAP_INCLUDE" \
        -o "$WORK/$name.clap/Contents/MacOS/$name" \
        test/clap_mock_plugin.cpp
}

build_mock MockClap
build_mock MockClapBadVersion -DMOCK_CLAP_BAD_VERSION

# A bundle that exports no clap_entry.
make_bundle "$WORK/NoEntry.clap" NoEntry
echo 'extern "C" int no_clap_entry_here() { return 0; }' > "$WORK/no_entry.cpp"
c++ -std=c++17 -bundle "${SANITIZER[@]}" \
    -o "$WORK/NoEntry.clap/Contents/MacOS/NoEntry" "$WORK/no_entry.cpp"

# An alias of MockClap: the registry resolves it to the same module.
ln -s "$WORK/MockClap.clap" "$WORK/MockClapAlias.clap"

c++ -std=c++17 -Wall -Wextra "${SANITIZER[@]}" \
    -DSOLOUD_ENABLE_CLAP -DSOLOUD_CLAP_TEST_HOOKS -DSOLOUD_ENABLE_PLUGIN_UI \
    -I test -I src \
    -isystem "$CLAP_INCLUDE" \
    -o "$WORK/clap_host_test" \
    test/clap_host_test.cpp \
    src/clap_bindings.cpp \
    src/clap/clap_host.cpp \
    src/clap/clap_instance.cpp \
    src/clap/clap_module.cpp \
    src/clap/clap_editor.cpp \
    test/clap_fake_window.cpp \
    -framework CoreFoundation

"$WORK/clap_host_test" "$WORK" "$@"
