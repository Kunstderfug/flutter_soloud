#!/bin/bash
# Source after changing to the repository root. These mirror the platform
# decoder dependencies selected by the production build hook.
NATIVE_DECODER_FLAGS=(-I src/native_decoder)
case "$(uname -s)" in
    Darwin)
        NATIVE_DECODER_PLATFORM_SOURCES=(
            src/native_decoder/os_decoder_apple.mm
            src/audiobuffer/aac_stream_decoder_apple.mm
        )
        NATIVE_DECODER_FLAGS+=(-framework AudioToolbox -framework CoreFoundation)
        ;;
    Linux)
        NATIVE_DECODER_PLATFORM_SOURCES=(
            src/native_decoder/os_decoder_linux.cpp
            src/audiobuffer/aac_stream_decoder_linux.cpp
        )
        NATIVE_DECODER_FLAGS+=(-ldl)
        ;;
    *)
        echo "Standalone decoder tests require macOS or Linux" >&2
        exit 1
        ;;
esac
