// One hosted CLAP effect instance: its clap_host_t, the plugin, port layout,
// scratch buffers, event lists and lifecycle state. ClapHostManager drives
// every lifecycle sequence; see docs/clap_hosting_plan.md, Phases 2 and 3.
//
// Threads:
// - Lifecycle calls (create, init, ports, activate, deactivate, on_main_thread,
//   destroy) run on the main thread, inside a manager sequence that holds
//   mLifecycleBusy.
// - start_processing, process and stop_processing run in this instance's audio
//   role (mAudioRoleOwner), under mProcessMutex: normally on the DSP thread, or
//   on main while a restart or teardown stops processing.
// - Phase 3 adds the host extensions (log, params, state, latency,
//   timer-support, audio-ports), the parameter cache, the host -> plugin edit
//   queue, the plugin -> host event ring and the coalesced notifications.
//   Lock order: mProcessMutex -> mEditProducerMutex -> mParamsMutex, and
//   mDrainMutex -> mParamsMutex. DSP takes only the first; no plugin call runs
//   under mEditProducerMutex or mParamsMutex.
#pragma once

#ifdef SOLOUD_ENABLE_CLAP

#include "../enums.h"
#include "../simd/soloud_simd.h"
#include "clap_event_list.h"
#include "clap_module.h"
#include "clap_spsc.h"

#include <clap/clap.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace soloud_clap
{
    /// Read lock-free by DSP. Retired is terminal and set only under the
    /// manager mutex; every other transition is a compare-exchange, so none can
    /// leave Retired.
    enum class Disposition : int
    {
        /// Eligible: DSP try-locks, starts lazily and processes.
        active,
        /// A restart is in progress: DSP leaves the block dry.
        servicing,
        /// Start failed, the layout is unroutable or activate failed: DSP
        /// writes silence until a restart succeeds.
        failed,
        /// Unloaded: DSP does nothing; the drain tears it down.
        retired,
    };

    /// Main-only record of the last completed lifecycle call. Whether
    /// processing is started is mStarted (under mProcessMutex).
    enum class Lifecycle : int
    {
        /// create_plugin succeeded; init not called or failed.
        created,
        inited,
        activated,
        destroyed,
    };

    /// The port layout read from `audio-ports` while deactivated.
    struct PortLayout
    {
        std::vector<uint32_t> inputChannels;  // per declared input port
        std::vector<uint32_t> outputChannels; // per declared output port
        /// Port 0 of that side carries CLAP_AUDIO_PORT_IS_MAIN. A main port,
        /// if any, is at index 0 (audio-ports.h); without one every port of
        /// that side is auxiliary.
        bool hasMainInput = false;
        bool hasMainOutput = false;

        uint32_t mainInputChannels() const
        {
            return hasMainInput ? inputChannels[0] : 0;
        }
        uint32_t mainOutputChannels() const
        {
            return hasMainOutput ? outputChannels[0] : 0;
        }
        uint32_t totalInputChannels() const;
        uint32_t totalOutputChannels() const;
    };

    /// Layout limits (plan Phase 2): per side at most this many ports and
    /// channels; the main ports carry 1 or 2 channels.
    constexpr uint32_t kMaxPortsPerSide = 16;
    constexpr uint32_t kMaxChannelsPerSide = 32;

    // --- Phase 3 limits ---------------------------------------------------------
    /// Host -> plugin edit queue (and the retained REQUIRES_PROCESS store).
    constexpr uint32_t kEditCapacity = 1024;
    /// Entries DSP examines per processChunk (stale drops included).
    constexpr uint32_t kMaxEditsExaminedPerChunk = 256;
    /// Plugin -> host event ring.
    constexpr uint32_t kOutRingCapacity = 2048;
    constexpr uint32_t kMaxTimers = 64;
    constexpr uint32_t kMinTimerPeriodMs = 16;
    constexpr uint64_t kDefaultIdleNs = 200ull * 1000ull * 1000ull;
    constexpr uint64_t kDefaultStateLimit = 64ull * 1024ull * 1024ull;
    constexpr uint32_t kMaxParams = 65536;

    /// Event types drainEvents reports (mirrors ClapHostEventType in enums.h).
    enum class HostEventType : int
    {
        paramValue = 1,
        gestureBegin = 2,
        gestureEnd = 3,
        resync = 4,
        paramsRescan = 5,
        latencyChanged = 6,
        restarted = 7,
        restartFailed = 8,
        stateDirty = 9,
        editorClosed = 10,
    };

    /// `resync` flags.
    constexpr unsigned kResyncOverflow = 1u << 0;
    constexpr unsigned kResyncParamsReset = 1u << 1;

    /// One parameter as the plugin described it (copied).
    struct ParamEntry
    {
        clap_id id = CLAP_INVALID_ID;
        uint32_t flags = 0;
        void *cookie = nullptr;
        std::string name;
        std::string module;
        double min = 0;
        double max = 0;
        double def = 0;
    };

    /// An immutable snapshot of the parameter list. [pending] means a
    /// rescan(ALL) invalidated the list and the rebuild has not published yet.
    struct ParamCache
    {
        uint64_t generation = 0;
        bool pending = false;
        std::vector<ParamEntry> entries;
        std::unordered_map<clap_id, size_t> byId;

        const ParamEntry *find(clap_id id) const
        {
            auto it = byId.find(id);
            return it != byId.end() ? &entries[it->second] : nullptr;
        }
    };

    /// A queued host -> plugin value. Delivered only while [generation] and
    /// [epoch] equal the instance's current ones.
    struct Edit
    {
        clap_id id = CLAP_INVALID_ID;
        void *cookie = nullptr;
        double value = 0;
        bool requiresProcess = false;
        uint64_t generation = 0;
        uint64_t epoch = 0;
    };

    /// A plugin -> host param event in the out ring.
    struct OutEvent
    {
        HostEventType kind = HostEventType::paramValue;
        bool dontRecord = false;
        clap_id paramId = CLAP_INVALID_ID;
        double value = 0;
    };

    /// Main-only: the plugin call in progress, for the legality of rescans
    /// and latency.changed (plan Phase 3, R7).
    enum class CallPhase : int
    {
        idle,
        initing,
        activating,
        deactivating,
        loadingState,
        savingState,
    };

    /// Coalesced notification bits (drained in this order).
    enum NotificationBit : uint32_t
    {
        kNotifyRestartFailed = 1u << 0,
        kNotifyRestarted = 1u << 1,
        kNotifyLatencyChanged = 1u << 2,
        kNotifyParamsRescan = 1u << 3,
        kNotifyStateDirty = 1u << 4,
    };

    class ClapInstance
    {
    public:
        ClapInstance(ModuleRef module, std::string pluginId, double sampleRate,
                     uint32_t maxBlockSize);
        ~ClapInstance();
        ClapInstance(const ClapInstance &) = delete;
        ClapInstance &operator=(const ClapInstance &) = delete;

        // --- identity -------------------------------------------------------
        int handle() const noexcept
        {
            return mHandle.load(std::memory_order_acquire);
        }
        void setHandle(int handle) noexcept
        {
            mHandle.store(handle, std::memory_order_release);
        }
        const std::string &pluginId() const noexcept { return mPluginId; }
        const clap_host_t *host() const noexcept { return &mHost; }
        const clap_plugin_t *plugin() const noexcept { return mPlugin; }

        // --- state ----------------------------------------------------------
        Disposition disposition() const noexcept
        {
            return mDisposition.load(std::memory_order_acquire);
        }
        /// Compare-exchange; never succeeds once Retired.
        bool transition(Disposition from, Disposition to) noexcept;
        /// Manager-mutex only.
        void retire() noexcept
        {
            mDisposition.store(Disposition::retired, std::memory_order_release);
        }

        Lifecycle lifecycle() const noexcept
        {
            return mLifecycle.load(std::memory_order_acquire);
        }
        bool started();

        uint64_t restartCount() const noexcept
        {
            return mRestartCount.load(std::memory_order_acquire);
        }
        void bumpRestartCount() noexcept
        {
            mRestartCount.fetch_add(1, std::memory_order_acq_rel);
        }
        bool processRequested() const noexcept
        {
            return mProcessRequested.load(std::memory_order_acquire);
        }
        uint64_t processRequestCount() const noexcept
        {
            return mProcessRequestCount.load(std::memory_order_acquire);
        }

        // Pending requests from the plugin (any thread). exchange(false) to
        // take one, so a request made while it is serviced stays pending.
        std::atomic<bool> mRestartRequested{false};
        std::atomic<bool> mCallbackRequested{false};

        /// Main-only: a load, restart, teardown or callback sequence is
        /// running. The pump and the drain skip a busy instance.
        bool mLifecycleBusy = false;

        // --- main-thread lifecycle calls (record the resulting state) --------
        bool create();
        bool init();
        bool activate();
        void deactivate();
        void destroy();
        void onMainThread();
        /// Drops the module reference (after destroy()). The last reference
        /// runs the module's deinit() on this thread.
        void releaseModule() { mModule.reset(); }

        /// Reads and validates the port layout (deactivated). Writes a reason
        /// on failure.
        bool readPorts(PortLayout &outLayout, std::string &outError);
        /// Installs [layout] with new scratch buffers. Builds the whole
        /// configuration first and swaps it in under mProcessMutex, so DSP
        /// never sees a half-built one. False (nothing changed) when the
        /// scratch cannot be allocated.
        bool installLayout(const PortLayout &layout);
        PortLayout layout();

        /// Takes mProcessMutex (blocking) and, if processing was started,
        /// calls stop_processing in this instance's audio role.
        void stopProcessingExclusive();

        // --- DSP ------------------------------------------------------------
        void processAudio(float *buffer, unsigned samples, unsigned bufferSize,
                          unsigned channels) noexcept;

        // --- Phase 3: extensions, params, state, events -------------------
        /// Main, after init: caches the plugin extensions the host calls.
        void cacheExtensions();
        /// Main: rebuilds the parameter cache from the plugin and publishes
        /// it (plan Phase 3, B2). The caller owns mLifecycleBusy.
        void rebuildParams();
        /// Main, after a host-initiated plugin call returned (under the
        /// guard): applies a pending or latched rescan at this safe point.
        void afterPluginCall();
        /// Main, inited, just before activate: services a rescan raised by
        /// the plugin calls since the last rebuild (port queries) while still
        /// inactive. False when the list is still pending (plan B2).
        bool settleParamsBeforeActivate();
        /// Main: reads the latency after a successful activate and stores it;
        /// with [notify], posts latencyChanged when it changed.
        void publishLatency(bool notify);
        /// Main: posts a coalesced notification.
        void notify(uint32_t bits) noexcept;
        void notifyParamsRescan(uint32_t clapFlags) noexcept;

        /// Main: one flush pass if the instance is idle and has work (plan
        /// Phase 3, §3.4). The caller holds the call guard.
        void serviceFlush(int64_t nowNs);
        /// Main: fires due timers. The caller holds the call guard.
        void fireTimers(int64_t nowNs);
        /// Main: whether a pump rebuild step has work.
        bool rebuildStepNeeded() const noexcept;

        /// Main: teardown started. Disables timers; rescans only bump.
        void beginTeardown() noexcept;
        /// Main, after destroy(): drops the timer registrations.
        void clearTimers() noexcept { mTimers.clear(); }
        bool timersEnabled() const noexcept { return mTimersEnabled; }
        void enableTimers() noexcept { mTimersEnabled = true; }

        /// Any thread: the current parameter snapshot (never null).
        std::shared_ptr<const ParamCache> params() const;
        /// Any non-audio thread, never blocks on main.
        PlayerErrors setParameterValue(clap_id id, double value);
        uint32_t latency() const noexcept
        {
            return mLatency.load(std::memory_order_acquire);
        }
        bool hasParamsExtension() const noexcept { return mPluginParams != nullptr; }
        bool hasStateExtension() const noexcept { return mPluginState != nullptr; }

        // Main-hop operations; the caller resolved the instance and holds
        // the guard.
        PlayerErrors getParameterValue(clap_id id, double *outValue);
        PlayerErrors valueToText(clap_id id, double value, char *outText,
                                 int textLen);
        PlayerErrors textToValue(clap_id id, const char *text, double *outValue);
        PlayerErrors saveState(std::vector<unsigned char> &out);
        PlayerErrors loadState(const unsigned char *data, size_t size);

        /// Any non-audio thread: drains plugin events and notifications (plan
        /// Phase 3, B1 consumer). Returns the number written.
        int drainEvents(int capacity, int *outTypes, unsigned *outParamIds,
                        double *outValues, unsigned *outFlags);

        /// Whether the call may proceed: plugin present, inited or activated,
        /// not retired or tearing down.
        bool callable() const noexcept;

        // --- host callbacks -------------------------------------------------
        bool isAudioThread() const noexcept;

        /// Test introspection: the last process() call's steady_time.
        int64_t lastSteadyTime() const noexcept
        {
            return mLastSteadyTime.load(std::memory_order_acquire);
        }

        // --- introspection (tests, diagnostics) -----------------------------
        uint64_t paramsGeneration() const noexcept
        {
            return mParamsGeneration.load(std::memory_order_acquire);
        }
        uint64_t editEpoch() const noexcept
        {
            return mEditEpoch.load(std::memory_order_acquire);
        }
        bool paramsPending() const noexcept
        {
            return mParamsPending.load(std::memory_order_acquire);
        }
        uint32_t maxExaminedPerChunk() const noexcept
        {
            return mMaxExamined.load(std::memory_order_acquire);
        }
        uint64_t dryBlocksFromServicing() const noexcept
        {
            return mDryBlocksFromServicing.load(std::memory_order_acquire);
        }
        uint64_t droppedAudioLogs() const noexcept
        {
            return mDroppedAudioLogs.load(std::memory_order_acquire);
        }
        uint64_t misbehavingCount() const noexcept
        {
            return mMisbehavingCount.load(std::memory_order_acquire);
        }
        uint64_t flushCount() const noexcept
        {
            return mFlushCount.load(std::memory_order_acquire);
        }
        uint32_t retainedCount() const noexcept
        {
            return mRetainedCount.load(std::memory_order_acquire);
        }
        uint64_t discardedOutEvents() const noexcept
        {
            return mDiscardedOutEvents.load(std::memory_order_acquire);
        }
        uint64_t editRingReadable() const noexcept
        {
            return mEditRing.readable();
        }
        /// Main only.
        size_t timerCount() const noexcept { return mTimers.size(); }
        /// Main only. The clamped period of timer [id], 0 if unknown.
        uint32_t timerPeriodMs(clap_id id) const noexcept
        {
            for (const Timer &t : mTimers)
                if (t.id == id)
                    return t.periodMs;
            return 0;
        }
        /// Main only. True while a stalled flush is still blocking (the
        /// retained list has not drained and no epoch or generation moved).
        bool flushStalled() const noexcept { return stallActive(); }
        bool outBlocked() const noexcept;

    private:
        class AudioRole;

        void setLifecycle(Lifecycle state) noexcept
        {
            mLifecycle.store(state, std::memory_order_release);
        }
        static void silence(float *buffer, unsigned samples, unsigned bufferSize,
                            unsigned channels) noexcept;
        void processChunk(float *buffer, unsigned offset, unsigned frames,
                          unsigned bufferSize, unsigned busChannels) noexcept;

        class CallPhaseScope;

        // Edit consumption (mProcessMutex held).
        bool editStale(const Edit &edit) const noexcept;
        bool pushEditEvent(const Edit &edit) noexcept;
        void consumeEditsDsp() noexcept;
        void retainedPush(const Edit &edit) noexcept;
        const Edit *retainedFront() const noexcept;
        void retainedPop() noexcept;
        void compactRetained() noexcept;
        bool outPush(const OutEvent &event) noexcept;
        /// Main: sets the out ring's paramsReset block, holding mProcessMutex
        /// (taken here unless main already holds it).
        void issueOutReset();
        void outResetLocked() noexcept;

        /// Main: legality of [main-thread] callbacks (B2 guard). Logs and
        /// returns false on a wrong thread.
        bool mainCallbackAllowed(const char *what) noexcept;
        /// Plan R7: whether the plugin counts as inactive right now.
        bool inactiveForRescan() const noexcept;
        void logMisbehaving(const char *what) noexcept;
        void handleRescanAll();
        std::shared_ptr<ParamCache> buildParamCache();
        bool criticalChange(const ParamCache &from, const ParamCache &to) const;
        void stallRecord() noexcept;
        bool stallActive() const noexcept;
        /// Main: queries timer-support once (lazily, so init may register).
        void resolveTimerExtension();

        static const void *CLAP_ABI hostGetExtension(const clap_host_t *host,
                                                     const char *id);
        static void CLAP_ABI hostRequestRestart(const clap_host_t *host);
        static void CLAP_ABI hostRequestProcess(const clap_host_t *host);
        static void CLAP_ABI hostRequestCallback(const clap_host_t *host);
        static bool CLAP_ABI hostIsMainThread(const clap_host_t *host);
        static bool CLAP_ABI hostIsAudioThread(const clap_host_t *host);
        static void CLAP_ABI hostLog(const clap_host_t *host,
                                     clap_log_severity severity,
                                     const char *msg);
        static void CLAP_ABI hostParamsRescan(const clap_host_t *host,
                                              clap_param_rescan_flags flags);
        static void CLAP_ABI hostParamsClear(const clap_host_t *host,
                                             clap_id paramId,
                                             clap_param_clear_flags flags);
        static void CLAP_ABI hostParamsRequestFlush(const clap_host_t *host);
        static void CLAP_ABI hostStateMarkDirty(const clap_host_t *host);
        static void CLAP_ABI hostLatencyChanged(const clap_host_t *host);
        static bool CLAP_ABI hostRegisterTimer(const clap_host_t *host,
                                               uint32_t periodMs,
                                               clap_id *timerId);
        static bool CLAP_ABI hostUnregisterTimer(const clap_host_t *host,
                                                 clap_id timerId);
        static bool CLAP_ABI hostAudioPortsIsRescanFlagSupported(
            const clap_host_t *host, uint32_t flag);
        static void CLAP_ABI hostAudioPortsRescan(const clap_host_t *host,
                                                  uint32_t flags);
        static bool CLAP_ABI outTryPush(const clap_output_events_t *list,
                                        const clap_event_header_t *event);
        static ClapInstance *from(const clap_host_t *host) noexcept;

        ModuleRef mModule;
        const std::string mPluginId;
        const double mSampleRate;
        const uint32_t mMaxBlockSize;

        clap_host_t mHost{};
        const clap_plugin_t *mPlugin = nullptr;

        std::atomic<int> mHandle{0};
        std::atomic<Disposition> mDisposition{Disposition::active};
        std::atomic<Lifecycle> mLifecycle{Lifecycle::created};
        std::atomic<std::thread::id> mAudioRoleOwner{};
        std::atomic<uint64_t> mRestartCount{0};
        std::atomic<bool> mProcessRequested{false};
        std::atomic<uint64_t> mProcessRequestCount{0};
        std::atomic<int64_t> mLastSteadyTime{-1};

        // Guarded by mProcessMutex.
        std::mutex mProcessMutex;
        bool mStarted = false;
        int64_t mSteadyTime = 0;
        PortLayout mLayout;
        soloud_simd::AlignedFloatPtr mInputScratch;
        soloud_simd::AlignedFloatPtr mOutputScratch;
        std::vector<float *> mInputPointers;
        std::vector<float *> mOutputPointers;
        std::vector<clap_audio_buffer_t> mInputBuffers;
        std::vector<clap_audio_buffer_t> mOutputBuffers;
        InputEventList mInputEvents;
        /// Plugin out_events: param events go straight to mOutRing.
        clap_output_events_t mOutEvents{};
        /// Consumer-owned FIFO of REQUIRES_PROCESS edits an active flush could
        /// not deliver (plan R2). Guarded by mProcessMutex.
        std::unique_ptr<Edit[]> mRetained;
        uint32_t mRetainedHead = 0;
        uint32_t mRetainedSize = 0;
        /// Set while main holds mProcessMutex (flush service, exclusive stop).
        bool mMainHoldsProcessMutex = false;

        // --- plugin extensions (main) ---------------------------------------
        const clap_plugin_params_t *mPluginParams = nullptr;
        const clap_plugin_state_t *mPluginState = nullptr;
        const clap_plugin_latency_t *mPluginLatency = nullptr;
        const clap_plugin_timer_support_t *mPluginTimer = nullptr;
        bool mTimerExtResolved = false;

        // --- parameters -----------------------------------------------------
        /// Never held by DSP. Taken after mEditProducerMutex.
        mutable std::mutex mParamsMutex;
        std::shared_ptr<const ParamCache> mParamsCache;
        /// Main-only: the last published non-pending cache.
        std::shared_ptr<const ParamCache> mLastRealCache;
        std::atomic<uint64_t> mParamsGeneration{0};
        std::atomic<uint64_t> mEditEpoch{0};
        std::atomic<bool> mParamsPending{true};
        /// Rescan flags waiting for a safe point (INFO).
        std::atomic<uint32_t> mLatchedRescan{0};
        /// Serialises edit producers. Never held by DSP.
        std::mutex mEditProducerMutex;
        SpscRing<Edit> mEditRing{kEditCapacity};
        std::atomic<uint32_t> mRetainedCount{0};
        std::atomic<bool> mFlushRequested{false};
        std::atomic<int64_t> mLastProcessNs{-1};
        std::atomic<uint32_t> mMaxExamined{0};
        std::atomic<uint64_t> mDryBlocksFromServicing{0};
        std::atomic<uint64_t> mFlushCount{0};

        // Main-only.
        CallPhase mPhase = CallPhase::idle;
        bool mTearingDown = false;
        bool mFlushStalled = false;
        uint32_t mStallRetained = 0;
        uint64_t mStallEpoch = 0;
        uint64_t mStallGeneration = 0;

        // --- out ring and notifications -------------------------------------
        SpscRing<OutEvent> mOutRing;
        /// bit 63 blocked, bits 62-61 reasons (overflow, paramsReset),
        /// bits 31-0 dropped count (saturating).
        std::atomic<uint64_t> mOutState{0};
        std::atomic<uint64_t> mDiscardedOutEvents{0};
        std::atomic<uint32_t> mNotifyFlags{0};
        std::atomic<uint32_t> mRescanNotifyFlags{0};
        std::mutex mDrainMutex;
        // Guarded by mDrainMutex.
        bool mDrainNotifications = false;
        uint64_t mPrefixEnd = 0;
        bool mPrefixValid = false;
        bool mResyncAtEnd = false;
        uint32_t mNotifyPending = 0;
        uint32_t mRescanPending = 0;

        // --- latency, log, timers -------------------------------------------
        std::atomic<uint32_t> mLatency{0};
        std::atomic<uint64_t> mDroppedAudioLogs{0};
        std::atomic<uint64_t> mMisbehavingCount{0};
        struct Timer
        {
            clap_id id;
            uint32_t periodMs;
            int64_t dueNs;
        };
        // Main-only.
        std::vector<Timer> mTimers;
        clap_id mNextTimerId = 1;
        bool mTimersEnabled = false;
    };

    /// Main-only RAII: takes mLifecycleBusy for one host-initiated plugin call
    /// (plan R8). It fails, changing nothing, when the instance is already
    /// busy, and clears the flag only if it set it.
    class InstanceCallGuard
    {
    public:
        explicit InstanceCallGuard(ClapInstance &instance) noexcept
            : mInstance(instance), mOwned(!instance.mLifecycleBusy)
        {
            if (mOwned)
                mInstance.mLifecycleBusy = true;
        }
        ~InstanceCallGuard()
        {
            if (mOwned)
                mInstance.mLifecycleBusy = false;
        }
        InstanceCallGuard(const InstanceCallGuard &) = delete;
        InstanceCallGuard &operator=(const InstanceCallGuard &) = delete;
        bool acquired() const noexcept { return mOwned; }

    private:
        ClapInstance &mInstance;
        const bool mOwned;
    };

    /// Steady-clock nanoseconds.
    int64_t nowNs() noexcept;

#ifdef SOLOUD_CLAP_TEST_HOOKS
    namespace test
    {
        /// Makes the scratch allocation [nth] allocations from now fail (0:
        /// the next one, an installLayout's input buffer; 1: its output
        /// buffer). Negative disarms.
        void failScratchAllocation(int nth);
        /// Replaces the log sink (null restores stderr).
        void setLogSink(void (*sink)(const char *pluginId, int severity,
                                     const char *msg));
        /// The idle threshold of the flush service (0 restores 200 ms).
        void setIdleThresholdNs(uint64_t ns);
        /// The out ring capacity of instances constructed afterwards (0
        /// restores 2048).
        void setOutRingCapacity(uint32_t capacity);
        /// The GetState size limit (0 restores 64 MiB).
        void setStateSizeLimit(uint64_t bytes);
        /// Runs inside setParameterValue's critical section, after the
        /// snapshot and before the push (null to clear).
        void setEditProducerHook(void (*hook)());
        /// Runs in drainEvents after the resync's word load and before its
        /// critical section (null to clear).
        void setDrainBeforeAckHook(void (*hook)());
        /// Runs in drainEvents under mParamsMutex, between the pending check
        /// and the acknowledging CAS. It must not take mParamsMutex.
        void setDrainInsideAckHook(void (*hook)());
        /// Runs in handleRescanAll after the pending cache is published and
        /// before the out ring reset (null to clear).
        void setRescanAfterPendingHook(void (*hook)());
        /// Plugin calls made while holding mEditProducerMutex or mParamsMutex.
        uint64_t lockOrderViolations();
        void resetLockOrderViolations();
    } // namespace test
#endif
} // namespace soloud_clap

#endif // SOLOUD_ENABLE_CLAP
