// One hosted CLAP effect instance: its clap_host_t, the plugin, port layout,
// scratch buffers, event lists and lifecycle state. ClapHostManager drives
// every lifecycle sequence; see docs/clap_hosting_plan.md, Phase 2.
//
// Threads:
// - Lifecycle calls (create, init, ports, activate, deactivate, on_main_thread,
//   destroy) run on the main thread, inside a manager sequence that holds
//   mLifecycleBusy.
// - start_processing, process and stop_processing run in this instance's audio
//   role (mAudioRoleOwner), under mProcessMutex: normally on the DSP thread, or
//   on main while a restart or teardown stops processing.
#pragma once

#ifdef SOLOUD_ENABLE_CLAP

#include "../simd/soloud_simd.h"
#include "clap_event_list.h"
#include "clap_module.h"

#include <clap/clap.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
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

        // --- host callbacks -------------------------------------------------
        bool isAudioThread() const noexcept;

        /// Test introspection: the last process() call's steady_time.
        int64_t lastSteadyTime() const noexcept
        {
            return mLastSteadyTime.load(std::memory_order_acquire);
        }

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

        static const void *CLAP_ABI hostGetExtension(const clap_host_t *host,
                                                     const char *id);
        static void CLAP_ABI hostRequestRestart(const clap_host_t *host);
        static void CLAP_ABI hostRequestProcess(const clap_host_t *host);
        static void CLAP_ABI hostRequestCallback(const clap_host_t *host);
        static bool CLAP_ABI hostIsMainThread(const clap_host_t *host);
        static bool CLAP_ABI hostIsAudioThread(const clap_host_t *host);
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
        OutputEventList mOutputEvents;
    };

#ifdef SOLOUD_CLAP_TEST_HOOKS
    namespace test
    {
        /// Makes the scratch allocation [nth] allocations from now fail (0:
        /// the next one, an installLayout's input buffer; 1: its output
        /// buffer). Negative disarms.
        void failScratchAllocation(int nth);
    } // namespace test
#endif
} // namespace soloud_clap

#endif // SOLOUD_ENABLE_CLAP
