// Shared between the mock CLAP plugin (test/clap_mock_plugin.cpp) and the
// native CLAP host harness (test/clap_host_test.cpp). The harness finds these
// hooks in the loaded bundle with CFBundleGetFunctionPointerForName.
#pragma once

#include <clap/clap.h>

#include <atomic>
#include <cstddef>
#include <cstdint>

/// Counters and knobs of one mock bundle (one per loaded DSO).
struct MockClapState
{
    // --- counters (written by the mock) ---------------------------------
    std::atomic<int> entryInits{0};
    std::atomic<int> entryDeinits{0};
    /// init/deinit ran while another call into the DSO was in flight, or
    /// init/deinit was called out of order.
    std::atomic<int> overlaps{0};
    /// A plugin call on the wrong thread (host thread-check), a call in the
    /// wrong lifecycle state, a reentrant on_main_thread, a non-zero aux
    /// input, a block larger than max_frames_count, a buffer with the wrong
    /// channel count or a steady_time gap.
    std::atomic<int> violations{0};
    std::atomic<int> liveInstances{0};
    std::atomic<int> processCalls{0};
    std::atomic<int> lastFrames{0};

    // --- knobs (written by the harness) ---------------------------------
    /// Process calls left that each call request_restart / request_callback.
    std::atomic<int> restartFromAudio{0};
    std::atomic<int> callbackFromAudio{0};
    /// The next N activate() calls fail.
    std::atomic<int> failActivateCount{0};
    /// Every Nth process call (of the whole DSO) returns CLAP_PROCESS_ERROR
    /// after writing garbage. 0 disables.
    std::atomic<int> processErrorEvery{0};
    /// Main port channels applied while deactivated (in deactivate() or on a
    /// ports query while deactivated); -1 keeps the current layout.
    std::atomic<int> pendingMainInChannels{-1};
    std::atomic<int> pendingMainOutChannels{-1};
    /// Port 0 of a side stays a main port in the mock's own processing but
    /// is reported without CLAP_AUDIO_PORT_IS_MAIN: bit 0 the input side,
    /// bit 1 the output side. Read on every ports query.
    std::atomic<int> unflaggedMainPorts{0};
};

/// Called synchronously from inside a mock callback, on the calling thread,
/// with the event name ("entry_init", "entry_deinit", "create", "init",
/// "activate", "deactivate", "start", "stop", "destroy", "on_main") and the
/// instance's host (null for entry events). Runs before the mock returns.
typedef void (*MockClapHook)(const char *event, const char *pluginId,
                             const clap_host_t *host);

extern "C"
{
    typedef MockClapState *(*MockClapStateFn)();
    typedef void (*MockClapSetHookFn)(MockClapHook hook);
    /// Copies the lifecycle log ("<id>:<event>:<main|audio|other>" lines) into
    /// [buffer] and returns the full length.
    typedef size_t (*MockClapLogFn)(char *buffer, size_t capacity);
    /// Clears the log, the counters except entry init state, and the knobs.
    typedef void (*MockClapResetFn)();
}
