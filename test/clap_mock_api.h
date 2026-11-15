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

    // --- mock.ext (Phase 3) ----------------------------------------------
    /// The parameter set (0: ids 10-40, 1: adds id 50, new cookies) the next
    /// deactivate() switches to; it then calls params.rescan(ALL). -1 none.
    std::atomic<int> pendingParamVariant{-1};
    /// deactivate() calls params.rescan(ALL) with the cookies unchanged.
    std::atomic<int> rescanAllInDeactivate{0};
    /// The next N inactive audio_ports.count(input) calls of mock.ext call
    /// params.rescan(ALL) (-1: every call), first switching to
    /// [portsParamVariant] when it is >= 0.
    std::atomic<int> rescanAllInPorts{0};
    std::atomic<int> portsParamVariant{-1};
    /// state.load switches to this variant and calls params.rescan(ALL).
    /// -1 keeps the variant.
    std::atomic<int> loadSwitchesVariant{-1};
    /// Appended to the name of id 10 when non-zero ("Gain 2").
    std::atomic<int> paramNameVariant{0};
    /// The max of id 10 (Gain) when above 0, in get_info and the range check.
    std::atomic<double> gainMax{0.0};
    /// The value get_value reports for the read-only meter (id 30).
    std::atomic<double> meterValue{0.0};
    /// Process calls left that each emit BEGIN, gestureValues x VALUE, END on
    /// id 10. The same from flush.
    std::atomic<int> gestureFromProcess{0};
    std::atomic<int> gestureFromFlush{0};
    std::atomic<int> gestureValues{1};
    std::atomic<int> gestureDontRecord{0};
    /// The next process call emits this many VALUE events on id 10.
    std::atomic<int> burstFromProcess{0};
    /// out_events.try_push calls that returned false.
    std::atomic<int> outPushFailures{0};
    std::atomic<int> outPushes{0};
    /// state: write/read chunk size (0: all at once), failures, extra bytes,
    /// an empty saved state.
    std::atomic<int> stateChunk{0};
    std::atomic<int> failSave{0};
    std::atomic<int> failLoad{0};
    std::atomic<int> stateExtraBytes{0};
    std::atomic<int> stateEmpty{0};
    std::atomic<int> saves{0};
    std::atomic<int> loads{0};
    /// latency.get value, and a value activate() applies and announces with
    /// latency.changed() (-1: none).
    std::atomic<int> latency{0};
    std::atomic<int> pendingLatency{-1};
    /// init() registers a timer of this period (0: none); the result.
    std::atomic<int> registerTimerMs{0};
    std::atomic<int> initTimerRegistered{0};
    std::atomic<uint32_t> initTimerId{CLAP_INVALID_ID};
    std::atomic<int> onTimerCalls{0};
    /// destroy() unregisters initTimerId; the result (1 true, 0 false, -1
    /// not tried).
    std::atomic<int> unregisterInDestroy{0};
    std::atomic<int> unregisterInDestroyResult{-1};
    /// Process calls left that each log a warning from the audio thread.
    std::atomic<int> logFromAudio{0};
    std::atomic<int> flushCalls{0};
    /// mock.ext init() returns false after its timer and log work.
    std::atomic<int> failInitExt{0};
    /// The next N start_processing() calls fail.
    std::atomic<int> failStartCount{0};

    // --- mock.ext gui (Phase 4b) -------------------------------------------
    /// The cocoa modes mock.ext offers: bit 0 embedded, bit 1 floating. 0
    /// (the default): no gui extension at all.
    std::atomic<int> guiApis{0};
    /// The next N create / get_size / set_parent / show calls fail.
    std::atomic<int> guiFailCreate{0};
    std::atomic<int> guiFailGetSize{0};
    std::atomic<int> guiFailSetParent{0};
    std::atomic<int> guiFailShow{0};
    /// The size create() starts with, and can_resize.
    std::atomic<int> guiWidth{400};
    std::atomic<int> guiHeight{300};
    std::atomic<int> guiCanResize{1};
    /// set_size returns false (the size is kept).
    std::atomic<int> guiRejectSetSize{0};
    /// get_resize_hints: 0 returns false; else the fields below.
    std::atomic<int> guiHints{0};
    std::atomic<int> guiHintHorizontal{1};
    std::atomic<int> guiHintVertical{1};
    std::atomic<int> guiHintAspectWidth{0};
    std::atomic<int> guiHintAspectHeight{0};
    /// adjust_size rounds both edges down to a multiple of this (0: false).
    std::atomic<int> guiAdjustStep{0};
    /// Counters and the gui's current state.
    std::atomic<int> guiCreates{0};
    std::atomic<int> guiDestroys{0};
    std::atomic<int> guiShows{0};
    std::atomic<int> guiHides{0};
    std::atomic<int> guiSetSizes{0};
    std::atomic<int> guiLive{0};
    std::atomic<int> guiCurrentWidth{0};
    std::atomic<int> guiCurrentHeight{0};
    std::atomic<int> guiShown{0};
    std::atomic<int> guiTransients{0};
    std::atomic<int> guiTransientNull{0};
};

/// One parameter value the mock received (process or flush).
struct MockReceived
{
    uint32_t id;
    double value;
    /// 1 process, 2 active flush (audio role), 3 inactive flush (main).
    int path;
    /// The event's time within its block.
    uint32_t time;
};

/// Called synchronously from inside a mock callback, on the calling thread,
/// with the event name ("entry_init", "entry_deinit", "create", "init",
/// "activate", "deactivate", "start", "stop", "destroy", "on_main"; for
/// mock.ext also "process", "flush", "save", "load", "on_timer",
/// "get_value", "get_info", "gui_create", "gui_show", "gui_hide",
/// "gui_set_size", "gui_destroy") and the instance's host (null for entry events). Runs before
/// the mock returns.
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
    /// Copies up to [capacity] received parameter values, oldest first, and
    /// returns the total received since the last reset.
    typedef size_t (*MockClapReceivedFn)(MockReceived *out, size_t capacity);
}
