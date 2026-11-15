#ifdef SOLOUD_ENABLE_CLAP

#include "clap_instance.h"

#include "clap_editor.h"

#include <pthread.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

#if defined(SOLOUD_ENABLE_PLUGIN_UI) && defined(__APPLE__)
#define SOLOUD_CLAP_GUI 1
#else
namespace soloud_clap
{
    /// No plugin UI in this build: the gui extension is not offered.
    class ClapEditor
    {
    };
} // namespace soloud_clap
#endif

namespace soloud_clap
{
    namespace
    {
        constexpr const char *kHostName = "flutter_soloud";
        constexpr const char *kHostVendor = "flutter_soloud";
        constexpr const char *kHostUrl = "";
        constexpr const char *kHostVersion = "1.0.0";

        uint32_t sum(const std::vector<uint32_t> &values)
        {
            uint32_t total = 0;
            for (uint32_t v : values)
                total += v;
            return total;
        }

#ifdef SOLOUD_CLAP_TEST_HOOKS
        std::atomic<int> gScratchFailCountdown{-1};
#endif

        using LogSink = void (*)(const char *, int, const char *);

        const char *severityName(int severity)
        {
            switch (severity)
            {
            case CLAP_LOG_DEBUG:
                return "debug";
            case CLAP_LOG_INFO:
                return "info";
            case CLAP_LOG_WARNING:
                return "warning";
            case CLAP_LOG_ERROR:
                return "error";
            case CLAP_LOG_FATAL:
                return "fatal";
            case CLAP_LOG_HOST_MISBEHAVING:
                return "host misbehaving";
            case CLAP_LOG_PLUGIN_MISBEHAVING:
                return "plugin misbehaving";
            default:
                return "log";
            }
        }

        void stderrSink(const char *pluginId, int severity, const char *msg)
        {
            std::fprintf(stderr, "[CLAP %s] %s: %s\n", pluginId,
                         severityName(severity), msg);
        }

        // Defaults; test hooks replace them.
        std::atomic<LogSink> gLogSink{nullptr};
        std::atomic<uint64_t> gIdleNs{kDefaultIdleNs};
        std::atomic<uint32_t> gOutRingCapacity{kOutRingCapacity};
        std::atomic<uint64_t> gStateLimit{kDefaultStateLimit};
#ifdef SOLOUD_CLAP_TEST_HOOKS
        std::atomic<void (*)()> gEditProducerHook{nullptr};
        std::atomic<void (*)()> gDrainBeforeAckHook{nullptr};
        std::atomic<void (*)()> gDrainInsideAckHook{nullptr};
        std::atomic<void (*)()> gRescanAfterPendingHook{nullptr};
        std::atomic<void (*)()> gSlotWriteHook{nullptr};
        std::atomic<void (*)()> gSlotReadHook{nullptr};
#endif
        std::atomic<uint64_t> gLockOrderViolations{0};

        void emitLog(const char *pluginId, int severity, const char *msg)
        {
            LogSink sink = gLogSink.load(std::memory_order_acquire);
            (sink != nullptr ? sink : &stderrSink)(pluginId, severity, msg);
        }

        // Plan B2 lock order: no plugin call while this thread holds
        // mEditProducerMutex or mParamsMutex. GuardedLock counts them.
        thread_local int tGuardedLockDepth = 0;

        class GuardedLock
        {
        public:
            explicit GuardedLock(std::mutex &mutex) : mLock(mutex)
            {
                ++tGuardedLockDepth;
            }
            ~GuardedLock() { --tGuardedLockDepth; }
            GuardedLock(const GuardedLock &) = delete;
            GuardedLock &operator=(const GuardedLock &) = delete;

        private:
            std::unique_lock<std::mutex> mLock;
        };

        /// At every plugin call site.
        inline void checkPluginCall() noexcept
        {
            if (tGuardedLockDepth > 0)
                gLockOrderViolations.fetch_add(1, std::memory_order_acq_rel);
        }

        // mOutState layout (plan B1).
        constexpr uint64_t kOutBlocked = 1ull << 63;
        constexpr uint64_t kOutReasonOverflow = 1ull << 62;
        constexpr uint64_t kOutReasonReset = 1ull << 61;
        constexpr uint64_t kOutCountMask = 0xffffffffull;

        uint64_t withDrop(uint64_t word) noexcept
        {
            return (word & kOutCountMask) < kOutCountMask ? word + 1 : word;
        }

        constexpr uint32_t kSupportedPortRescans =
            CLAP_AUDIO_PORTS_RESCAN_NAMES | CLAP_AUDIO_PORTS_RESCAN_FLAGS |
            CLAP_AUDIO_PORTS_RESCAN_CHANNEL_COUNT |
            CLAP_AUDIO_PORTS_RESCAN_PORT_TYPE |
            CLAP_AUDIO_PORTS_RESCAN_IN_PLACE_PAIR |
            CLAP_AUDIO_PORTS_RESCAN_LIST;

        /// The flags RESCAN_INFO may change without a generation bump.
        constexpr uint32_t kNonCriticalParamFlags =
            CLAP_PARAM_IS_PERIODIC | CLAP_PARAM_IS_HIDDEN;

        std::string boundedString(const char *s, size_t capacity)
        {
            size_t n = 0;
            while (n < capacity && s[n] != '\0')
                ++n;
            return std::string(s, n);
        }

        const std::shared_ptr<const ParamCache> &pendingCache()
        {
            static const std::shared_ptr<const ParamCache> kPending = [] {
                auto cache = std::make_shared<ParamCache>();
                cache->pending = true;
                return std::shared_ptr<const ParamCache>(std::move(cache));
            }();
            return kPending;
        }

        struct OstreamContext
        {
            std::vector<unsigned char> *buffer;
            uint64_t limit;
            bool overLimit;
        };

        int64_t CLAP_ABI ostreamWrite(const clap_ostream_t *stream,
                                      const void *data, uint64_t size)
        {
            auto *ctx = static_cast<OstreamContext *>(stream->ctx);
            if (size == 0)
                return 0;
            if (data == nullptr)
                return -1;
            if (size > ctx->limit - ctx->buffer->size())
            {
                ctx->overLimit = true;
                return -1;
            }
            const auto *bytes = static_cast<const unsigned char *>(data);
            try
            {
                ctx->buffer->insert(ctx->buffer->end(), bytes, bytes + size);
            }
            catch (const std::bad_alloc &)
            {
                ctx->overLimit = true;
                return -1;
            }
            return static_cast<int64_t>(size);
        }

        struct IstreamContext
        {
            const unsigned char *data;
            size_t size;
            size_t pos;
        };

        int64_t CLAP_ABI istreamRead(const clap_istream_t *stream, void *buffer,
                                     uint64_t size)
        {
            auto *ctx = static_cast<IstreamContext *>(stream->ctx);
            if (size == 0 || ctx->pos >= ctx->size)
                return 0;
            if (buffer == nullptr)
                return -1;
            const size_t n = static_cast<size_t>(
                std::min<uint64_t>(size, ctx->size - ctx->pos));
            std::memcpy(buffer, ctx->data + ctx->pos, n);
            ctx->pos += n;
            return static_cast<int64_t>(n);
        }

        void copyTruncated(const char *src, char *dst, int dstLen)
        {
            if (dst == nullptr || dstLen <= 0)
                return;
            const size_t cap = static_cast<size_t>(dstLen) - 1;
            size_t n = 0;
            while (n < cap && src[n] != '\0')
                ++n;
            std::memcpy(dst, src, n);
            dst[n] = '\0';
        }

        soloud_simd::AlignedFloatPtr allocateScratch(size_t count)
        {
#ifdef SOLOUD_CLAP_TEST_HOOKS
            int n = gScratchFailCountdown.load(std::memory_order_acquire);
            while (n >= 0 && !gScratchFailCountdown.compare_exchange_weak(
                                 n, n - 1, std::memory_order_acq_rel))
            {
            }
            if (n == 0)
                return nullptr;
#endif
            return soloud_simd::makeAlignedFloats(count);
        }
    } // namespace

#ifdef SOLOUD_CLAP_TEST_HOOKS
    void test::failScratchAllocation(int nth)
    {
        gScratchFailCountdown.store(nth < 0 ? -1 : nth,
                                    std::memory_order_release);
    }
    void test::setLogSink(void (*sink)(const char *, int, const char *))
    {
        gLogSink.store(sink, std::memory_order_release);
    }
    void test::setIdleThresholdNs(uint64_t ns)
    {
        gIdleNs.store(ns == 0 ? kDefaultIdleNs : ns, std::memory_order_release);
    }
    void test::setOutRingCapacity(uint32_t capacity)
    {
        gOutRingCapacity.store(capacity == 0 ? kOutRingCapacity : capacity,
                               std::memory_order_release);
    }
    void test::setStateSizeLimit(uint64_t bytes)
    {
        gStateLimit.store(bytes == 0 ? kDefaultStateLimit : bytes,
                          std::memory_order_release);
    }
    void test::setSlotWriteHook(void (*hook)())
    {
        gSlotWriteHook.store(hook, std::memory_order_release);
    }
    void test::setSlotReadHook(void (*hook)())
    {
        gSlotReadHook.store(hook, std::memory_order_release);
    }
    void test::setEditProducerHook(void (*hook)())
    {
        gEditProducerHook.store(hook, std::memory_order_release);
    }
    void test::setDrainBeforeAckHook(void (*hook)())
    {
        gDrainBeforeAckHook.store(hook, std::memory_order_release);
    }
    void test::setDrainInsideAckHook(void (*hook)())
    {
        gDrainInsideAckHook.store(hook, std::memory_order_release);
    }
    void test::setRescanAfterPendingHook(void (*hook)())
    {
        gRescanAfterPendingHook.store(hook, std::memory_order_release);
    }
    uint64_t test::lockOrderViolations()
    {
        return gLockOrderViolations.load(std::memory_order_acquire);
    }
    void test::resetLockOrderViolations()
    {
        gLockOrderViolations.store(0, std::memory_order_release);
    }
#endif

    int64_t nowNs() noexcept
    {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }

    uint32_t PortLayout::totalInputChannels() const { return sum(inputChannels); }
    uint32_t PortLayout::totalOutputChannels() const
    {
        return sum(outputChannels);
    }

    /// Holds this instance's audio role on the calling thread, so the
    /// plugin's thread-check sees an audio thread for start, process and stop.
    class ClapInstance::AudioRole
    {
    public:
        explicit AudioRole(ClapInstance &instance) : mInstance(instance)
        {
            mInstance.mAudioRoleOwner.store(std::this_thread::get_id(),
                                            std::memory_order_release);
        }
        ~AudioRole()
        {
            mInstance.mAudioRoleOwner.store(std::thread::id(),
                                            std::memory_order_release);
        }
        AudioRole(const AudioRole &) = delete;
        AudioRole &operator=(const AudioRole &) = delete;

    private:
        ClapInstance &mInstance;
    };

    /// Main-only: records the plugin call in progress (plan R7).
    class ClapInstance::CallPhaseScope
    {
    public:
        CallPhaseScope(ClapInstance &instance, CallPhase phase)
            : mInstance(instance), mPrevious(instance.mPhase)
        {
            mInstance.mPhase = phase;
        }
        ~CallPhaseScope() { mInstance.mPhase = mPrevious; }
        CallPhaseScope(const CallPhaseScope &) = delete;
        CallPhaseScope &operator=(const CallPhaseScope &) = delete;

    private:
        ClapInstance &mInstance;
        const CallPhase mPrevious;
    };

    ClapInstance::ClapInstance(ModuleRef module, std::string pluginId,
                               double sampleRate, uint32_t maxBlockSize)
        : mModule(std::move(module)), mPluginId(std::move(pluginId)),
          mSampleRate(sampleRate), mMaxBlockSize(maxBlockSize),
          mOutRing(gOutRingCapacity.load(std::memory_order_acquire))
    {
        mRetained.reset(new Edit[kEditCapacity]);
        mParamsCache = pendingCache();
        mOutEvents.ctx = this;
        mOutEvents.try_push = &ClapInstance::outTryPush;
        mHost.clap_version = CLAP_VERSION;
        mHost.host_data = this;
        mHost.name = kHostName;
        mHost.vendor = kHostVendor;
        mHost.url = kHostUrl;
        mHost.version = kHostVersion;
        mHost.get_extension = &ClapInstance::hostGetExtension;
        mHost.request_restart = &ClapInstance::hostRequestRestart;
        mHost.request_process = &ClapInstance::hostRequestProcess;
        mHost.request_callback = &ClapInstance::hostRequestCallback;
    }

    // The manager destroys the plugin on main before the last reference to
    // the instance goes. If that did not happen (a bug), the plugin is leaked
    // rather than destroyed from an arbitrary thread.
    ClapInstance::~ClapInstance() = default;

    bool ClapInstance::transition(Disposition from, Disposition to) noexcept
    {
        if (from == Disposition::retired)
            return false;
        return mDisposition.compare_exchange_strong(from, to,
                                                    std::memory_order_acq_rel);
    }

    bool ClapInstance::started()
    {
        std::lock_guard<std::mutex> lock(mProcessMutex);
        return mStarted;
    }

    // --- lifecycle (main) --------------------------------------------------

    bool ClapInstance::create()
    {
        const clap_plugin_factory_t *factory = mModule.factory();
        if (factory == nullptr)
            return false;
        checkPluginCall();
        const clap_plugin_t *plugin =
            factory->create_plugin(factory, &mHost, mPluginId.c_str());
        if (plugin == nullptr)
            return false;
        if (plugin->destroy == nullptr)
            return false; // Cannot be cleaned up: leak rather than crash.
        mPlugin = plugin;
        setLifecycle(Lifecycle::created);
        if (plugin->init == nullptr || plugin->activate == nullptr ||
            plugin->deactivate == nullptr || plugin->start_processing == nullptr ||
            plugin->stop_processing == nullptr || plugin->process == nullptr ||
            plugin->get_extension == nullptr || plugin->on_main_thread == nullptr)
        {
            destroy();
            return false;
        }
        return true;
    }

    bool ClapInstance::init()
    {
        CallPhaseScope phase(*this, CallPhase::initing);
        checkPluginCall();
        if (!mPlugin->init(mPlugin))
            return false;
        setLifecycle(Lifecycle::inited);
        return true;
    }

    bool ClapInstance::activate()
    {
        CallPhaseScope phase(*this, CallPhase::activating);
        checkPluginCall();
        if (!mPlugin->activate(mPlugin, mSampleRate, 1, mMaxBlockSize))
            return false;
        setLifecycle(Lifecycle::activated);
        return true;
    }

    void ClapInstance::deactivate()
    {
        {
            CallPhaseScope phase(*this, CallPhase::deactivating);
            checkPluginCall();
            mPlugin->deactivate(mPlugin);
        }
        setLifecycle(Lifecycle::inited);
        mFlushStalled = false;
    }

    void ClapInstance::destroy()
    {
        const clap_plugin_t *plugin = mPlugin;
        checkPluginCall();
        mPlugin->destroy(plugin);
        setLifecycle(Lifecycle::destroyed);
    }

    void ClapInstance::onMainThread()
    {
        checkPluginCall();
        mPlugin->on_main_thread(mPlugin);
    }

    bool ClapInstance::readPorts(PortLayout &outLayout, std::string &outError)
    {
        outLayout = PortLayout();
        checkPluginCall();
        const auto *ports = static_cast<const clap_plugin_audio_ports_t *>(
            mPlugin->get_extension(mPlugin, CLAP_EXT_AUDIO_PORTS));
        if (ports == nullptr || ports->count == nullptr || ports->get == nullptr)
        {
            // audio-ports.h: without the extension the plugin has no ports.
            outError = "the plugin has no audio ports";
            return false;
        }
        for (const bool isInput : {true, false})
        {
            std::vector<uint32_t> &channels =
                isInput ? outLayout.inputChannels : outLayout.outputChannels;
            const uint32_t count = ports->count(mPlugin, isInput);
            if (count > kMaxPortsPerSide)
            {
                outError = "too many audio ports";
                return false;
            }
            uint32_t total = 0;
            for (uint32_t i = 0; i < count; ++i)
            {
                clap_audio_port_info_t info{};
                if (!ports->get(mPlugin, i, isInput, &info))
                {
                    outError = "audio port info unavailable";
                    return false;
                }
                if (i == 0 && (info.flags & CLAP_AUDIO_PORT_IS_MAIN) != 0)
                    (isInput ? outLayout.hasMainInput
                             : outLayout.hasMainOutput) = true;
                if (info.channel_count > kMaxChannelsPerSide ||
                    total + info.channel_count > kMaxChannelsPerSide)
                {
                    outError = "too many audio channels";
                    return false;
                }
                total += info.channel_count;
                channels.push_back(info.channel_count);
            }
        }
        // Only a port flagged CLAP_AUDIO_PORT_IS_MAIN, at index 0, is routed
        // to the bus. The bus needs a main output; without a main input the
        // effect gets silence on every input, like any auxiliary port.
        if (!outLayout.hasMainOutput)
        {
            outError = "the plugin has no main output port";
            return false;
        }
        const uint32_t mainOut = outLayout.mainOutputChannels();
        if (mainOut < 1 || mainOut > 2)
        {
            outError = "the main output must have 1 or 2 channels";
            return false;
        }
        const uint32_t mainIn = outLayout.mainInputChannels();
        if (outLayout.hasMainInput && (mainIn < 1 || mainIn > 2))
        {
            outError = "the main input must have 1 or 2 channels";
            return false;
        }
        return true;
    }

    bool ClapInstance::installLayout(const PortLayout &layout)
    {
        const size_t frames = mMaxBlockSize;
        const uint32_t inChannels = layout.totalInputChannels();
        const uint32_t outChannels = layout.totalOutputChannels();
        soloud_simd::AlignedFloatPtr inputScratch =
            allocateScratch(std::max<size_t>(1, inChannels * frames));
        if (!inputScratch)
            return false;
        soloud_simd::AlignedFloatPtr outputScratch =
            allocateScratch(std::max<size_t>(1, outChannels * frames));
        if (!outputScratch)
            return false;

        std::vector<float *> inputPointers(inChannels, nullptr);
        for (uint32_t c = 0; c < inChannels; ++c)
            inputPointers[c] = inputScratch.get() + c * frames;
        std::vector<float *> outputPointers(outChannels, nullptr);
        for (uint32_t c = 0; c < outChannels; ++c)
            outputPointers[c] = outputScratch.get() + c * frames;

        // The buffers point into the pointer vectors, whose storage moves
        // with them below.
        auto buildBuffers = [](const std::vector<uint32_t> &portChannels,
                               std::vector<float *> &pointers) {
            std::vector<clap_audio_buffer_t> buffers(portChannels.size(),
                                                     clap_audio_buffer_t{});
            size_t offset = 0;
            for (size_t p = 0; p < portChannels.size(); ++p)
            {
                buffers[p].data32 = portChannels[p] > 0 ? &pointers[offset]
                                                        : nullptr;
                buffers[p].data64 = nullptr;
                buffers[p].channel_count = portChannels[p];
                buffers[p].latency = 0;
                buffers[p].constant_mask = 0;
                offset += portChannels[p];
            }
            return buffers;
        };
        std::vector<clap_audio_buffer_t> inputBuffers =
            buildBuffers(layout.inputChannels, inputPointers);
        std::vector<clap_audio_buffer_t> outputBuffers =
            buildBuffers(layout.outputChannels, outputPointers);

        std::lock_guard<std::mutex> lock(mProcessMutex);
        mLayout = layout;
        mInputScratch = std::move(inputScratch);
        mOutputScratch = std::move(outputScratch);
        mInputPointers = std::move(inputPointers);
        mOutputPointers = std::move(outputPointers);
        mInputBuffers = std::move(inputBuffers);
        mOutputBuffers = std::move(outputBuffers);
        return true;
    }

    PortLayout ClapInstance::layout()
    {
        std::lock_guard<std::mutex> lock(mProcessMutex);
        return mLayout;
    }

    void ClapInstance::stopProcessingExclusive()
    {
        std::lock_guard<std::mutex> lock(mProcessMutex);
        if (!mStarted)
            return;
        mMainHoldsProcessMutex = true;
        {
            AudioRole role(*this);
            checkPluginCall();
            mPlugin->stop_processing(mPlugin);
        }
        mStarted = false;
        mMainHoldsProcessMutex = false;
    }

    // --- DSP ---------------------------------------------------------------

    void ClapInstance::silence(float *buffer, unsigned samples,
                               unsigned bufferSize, unsigned channels) noexcept
    {
        const unsigned owned = std::min(channels, 2u);
        for (unsigned c = 0; c < owned; ++c)
            std::memset(buffer + static_cast<size_t>(c) * bufferSize, 0,
                        samples * sizeof(float));
    }

    void ClapInstance::processAudio(float *buffer, unsigned samples,
                                    unsigned bufferSize, unsigned channels,
                                    const float *slotParams,
                                    unsigned slotParamCount,
                                    uint64_t slotChangedMask) noexcept
    {
        if (buffer == nullptr || samples == 0 || channels == 0)
            return;
        Disposition d = disposition();
        if (d == Disposition::failed)
        {
            silence(buffer, samples, bufferSize, channels);
            return;
        }
        if (d != Disposition::active)
            return; // Servicing (dry) or Retired (no-op).

        std::unique_lock<std::mutex> lock(mProcessMutex, std::try_to_lock);
        if (!lock.owns_lock())
        {
            // Exclusive servicing in progress: a dry block.
            mDryBlocksFromServicing.fetch_add(1, std::memory_order_acq_rel);
            return;
        }
        d = disposition();
        if (d == Disposition::failed)
        {
            silence(buffer, samples, bufferSize, channels);
            return;
        }
        if (d != Disposition::active)
            return;

        AudioRole role(*this);
        soloud_simd::ScopedFlushDenormals flushDenormals;
        if (!mStarted)
        {
            checkPluginCall();
            if (!mPlugin->start_processing(mPlugin))
            {
                // Muted until a restart succeeds (plan Phase 2, F5).
                transition(Disposition::active, Disposition::failed);
                silence(buffer, samples, bufferSize, channels);
                return;
            }
            mStarted = true;
        }

        const unsigned busChannels = std::min(channels, 2u);
        for (unsigned offset = 0; offset < samples; offset += mMaxBlockSize)
        {
            const unsigned frames =
                std::min<unsigned>(mMaxBlockSize, samples - offset);
            // Slot values go with the first chunk only, at time 0.
            const bool first = offset == 0;
            processChunk(buffer, offset, frames, bufferSize, busChannels,
                         first ? slotParams : nullptr,
                         first ? slotParamCount : 0,
                         first ? slotChangedMask : 0);
        }
    }

    void ClapInstance::processChunk(float *buffer, unsigned offset,
                                    unsigned frames, unsigned bufferSize,
                                    unsigned busChannels,
                                    const float *slotParams,
                                    unsigned slotParamCount,
                                    uint64_t slotChangedMask) noexcept
    {
        float *left = buffer + offset;
        float *right = busChannels == 2 ? buffer + bufferSize + offset : left;
        const size_t bytes = frames * sizeof(float);

        // Inputs: the main port from the bus, aux ports zeroed every chunk.
        const uint32_t mainIn = mLayout.mainInputChannels();
        for (size_t c = 0; c < mInputPointers.size(); ++c)
        {
            if (c >= mainIn)
                std::memset(mInputPointers[c], 0, bytes);
        }
        if (mainIn == 2)
        {
            std::memcpy(mInputPointers[0], left, bytes);
            std::memcpy(mInputPointers[1], right, bytes);
        }
        else if (mainIn == 1)
        {
            float *dst = mInputPointers[0];
            if (busChannels == 2)
            {
                for (unsigned i = 0; i < frames; ++i)
                    dst[i] = 0.5f * (left[i] + right[i]);
            }
            else
            {
                std::memcpy(dst, left, bytes);
            }
        }

        mInputEvents.clear();
        consumeEditsDsp();
        if (slotParams != nullptr)
            pushSlotEventsDsp(slotParams, slotParamCount, slotChangedMask);

        clap_process_t process{};
        process.steady_time = mSteadyTime;
        process.frames_count = frames;
        process.transport = nullptr;
        process.audio_inputs = mInputBuffers.empty() ? nullptr : mInputBuffers.data();
        process.audio_outputs =
            mOutputBuffers.empty() ? nullptr : mOutputBuffers.data();
        process.audio_inputs_count = static_cast<uint32_t>(mInputBuffers.size());
        process.audio_outputs_count =
            static_cast<uint32_t>(mOutputBuffers.size());
        process.in_events = mInputEvents.vtable();
        process.out_events = &mOutEvents;

        // A process call answers any pending request_process (plan F7) and
        // request_flush (plan Phase 3, §3.3).
        mFlushRequested.store(false, std::memory_order_release);
        mProcessRequested.store(false, std::memory_order_release);
        checkPluginCall();
        const clap_process_status status = mPlugin->process(mPlugin, &process);
        mLastProcessNs.store(nowNs(), std::memory_order_release);
        mLastSteadyTime.store(mSteadyTime, std::memory_order_release);
        mSteadyTime += frames;

        if (status == CLAP_PROCESS_ERROR)
        {
            std::memset(left, 0, bytes);
            if (busChannels == 2)
                std::memset(right, 0, bytes);
            return;
        }

        // Outputs: the main port to the bus; aux outputs are discarded.
        const uint32_t mainOut = mLayout.mainOutputChannels();
        const float *out0 = mOutputPointers[0];
        const float *out1 = mainOut == 2 ? mOutputPointers[1] : out0;
        if (busChannels == 2)
        {
            std::memcpy(left, out0, bytes);
            std::memcpy(right, out1, bytes);
        }
        else if (mainOut == 2)
        {
            for (unsigned i = 0; i < frames; ++i)
                left[i] = 0.5f * (out0[i] + out1[i]);
        }
        else
        {
            std::memcpy(left, out0, bytes);
        }
    }

    // --- host callbacks ------------------------------------------------------

    bool ClapInstance::isAudioThread() const noexcept
    {
        return mAudioRoleOwner.load(std::memory_order_acquire) ==
               std::this_thread::get_id();
    }

    ClapInstance *ClapInstance::from(const clap_host_t *host) noexcept
    {
        return host != nullptr ? static_cast<ClapInstance *>(host->host_data)
                               : nullptr;
    }

    const void *CLAP_ABI ClapInstance::hostGetExtension(const clap_host_t *host,
                                                        const char *id)
    {
        static const clap_host_thread_check_t kThreadCheck = {
            &ClapInstance::hostIsMainThread,
            &ClapInstance::hostIsAudioThread,
        };
        if (from(host) == nullptr || id == nullptr)
            return nullptr;
        static const clap_host_log_t kLog = {&ClapInstance::hostLog};
        static const clap_host_params_t kParams = {
            &ClapInstance::hostParamsRescan,
            &ClapInstance::hostParamsClear,
            &ClapInstance::hostParamsRequestFlush,
        };
        static const clap_host_state_t kState = {
            &ClapInstance::hostStateMarkDirty};
        static const clap_host_latency_t kLatency = {
            &ClapInstance::hostLatencyChanged};
        static const clap_host_timer_support_t kTimer = {
            &ClapInstance::hostRegisterTimer,
            &ClapInstance::hostUnregisterTimer,
        };
        static const clap_host_audio_ports_t kAudioPorts = {
            &ClapInstance::hostAudioPortsIsRescanFlagSupported,
            &ClapInstance::hostAudioPortsRescan,
        };
        if (std::strcmp(id, CLAP_EXT_THREAD_CHECK) == 0)
            return &kThreadCheck;
        if (std::strcmp(id, CLAP_EXT_LOG) == 0)
            return &kLog;
        if (std::strcmp(id, CLAP_EXT_PARAMS) == 0)
            return &kParams;
        if (std::strcmp(id, CLAP_EXT_STATE) == 0)
            return &kState;
        if (std::strcmp(id, CLAP_EXT_LATENCY) == 0)
            return &kLatency;
        if (std::strcmp(id, CLAP_EXT_TIMER_SUPPORT) == 0)
            return &kTimer;
        if (std::strcmp(id, CLAP_EXT_AUDIO_PORTS) == 0)
            return &kAudioPorts;
#ifdef SOLOUD_CLAP_GUI
        static const clap_host_gui_t kGui = {
            &ClapInstance::hostGuiResizeHintsChanged,
            &ClapInstance::hostGuiRequestResize,
            &ClapInstance::hostGuiRequestShow,
            &ClapInstance::hostGuiRequestHide,
            &ClapInstance::hostGuiClosed,
        };
        if (std::strcmp(id, CLAP_EXT_GUI) == 0)
            return &kGui;
#endif
        return nullptr;
    }

    void CLAP_ABI ClapInstance::hostRequestRestart(const clap_host_t *host)
    {
        if (ClapInstance *self = from(host))
            self->mRestartRequested.store(true, std::memory_order_release);
    }

    // v1 calls process() on every eligible block, so a request only stays
    // pending (and counted) until the next process() call. While processing
    // is not guaranteed, the pump's flush service answers it (plan §3.4).
    void CLAP_ABI ClapInstance::hostRequestProcess(const clap_host_t *host)
    {
        if (ClapInstance *self = from(host))
        {
            self->mProcessRequestCount.fetch_add(1, std::memory_order_acq_rel);
            self->mProcessRequested.store(true, std::memory_order_release);
        }
    }

    void CLAP_ABI ClapInstance::hostRequestCallback(const clap_host_t *host)
    {
        if (ClapInstance *self = from(host))
            self->mCallbackRequested.store(true, std::memory_order_release);
    }

    // While main holds an instance's audio role (an exclusive stop or an
    // active flush), it is that instance's audio thread, not its main thread.
    bool CLAP_ABI ClapInstance::hostIsMainThread(const clap_host_t *host)
    {
        const ClapInstance *self = from(host);
        return pthread_main_np() != 0 &&
               !(self != nullptr && self->isAudioThread());
    }

    bool CLAP_ABI ClapInstance::hostIsAudioThread(const clap_host_t *host)
    {
        const ClapInstance *self = from(host);
        return self != nullptr && self->isAudioThread();
    }

    // --- Phase 3: legality helpers -----------------------------------------

    void ClapInstance::logMisbehaving(const char *what) noexcept
    {
        mMisbehavingCount.fetch_add(1, std::memory_order_acq_rel);
        if (!isAudioThread())
            emitLog(mPluginId.c_str(), CLAP_LOG_PLUGIN_MISBEHAVING, what);
    }

    bool ClapInstance::mainCallbackAllowed(const char *what) noexcept
    {
        // Plan B2: a [main-thread] callback from the thread holding this
        // instance's audio role is rejected even when that thread is main.
        if (pthread_main_np() != 0 && !isAudioThread())
            return true;
        logMisbehaving(what);
        return false;
    }

    bool ClapInstance::inactiveForRescan() const noexcept
    {
        if (mPhase == CallPhase::initing || mPhase == CallPhase::deactivating)
            return true;
        return lifecycle() == Lifecycle::inited &&
               mPhase != CallPhase::activating;
    }

    bool ClapInstance::callable() const noexcept
    {
        const Lifecycle l = lifecycle();
        return mPlugin != nullptr &&
               (l == Lifecycle::inited || l == Lifecycle::activated) &&
               disposition() != Disposition::retired && !mTearingDown;
    }

    void ClapInstance::beginTeardown() noexcept
    {
        mTimersEnabled = false;
        mTearingDown = true;
    }

    // --- Phase 3: plugin extensions ----------------------------------------

    void ClapInstance::resolveTimerExtension()
    {
        if (mTimerExtResolved)
            return;
        mTimerExtResolved = true;
        checkPluginCall();
        const auto *timer = static_cast<const clap_plugin_timer_support_t *>(
            mPlugin->get_extension(mPlugin, CLAP_EXT_TIMER_SUPPORT));
        if (timer == nullptr)
            return;
        if (timer->on_timer == nullptr)
        {
            logMisbehaving("incomplete timer-support extension");
            return;
        }
        mPluginTimer = timer;
    }

    void ClapInstance::cacheExtensions()
    {
        checkPluginCall();
        const auto *params = static_cast<const clap_plugin_params_t *>(
            mPlugin->get_extension(mPlugin, CLAP_EXT_PARAMS));
        if (params != nullptr)
        {
            if (params->count != nullptr && params->get_info != nullptr &&
                params->get_value != nullptr &&
                params->value_to_text != nullptr &&
                params->text_to_value != nullptr && params->flush != nullptr)
                mPluginParams = params;
            else
                logMisbehaving("incomplete params extension");
        }
        checkPluginCall();
        const auto *state = static_cast<const clap_plugin_state_t *>(
            mPlugin->get_extension(mPlugin, CLAP_EXT_STATE));
        if (state != nullptr)
        {
            if (state->save != nullptr && state->load != nullptr)
                mPluginState = state;
            else
                logMisbehaving("incomplete state extension");
        }
        checkPluginCall();
        const auto *latency = static_cast<const clap_plugin_latency_t *>(
            mPlugin->get_extension(mPlugin, CLAP_EXT_LATENCY));
        if (latency != nullptr)
        {
            if (latency->get != nullptr)
                mPluginLatency = latency;
            else
                logMisbehaving("incomplete latency extension");
        }
        resolveTimerExtension();
        cacheGuiExtension();
    }

    // --- Phase 4b: gui -------------------------------------------------------

    void ClapInstance::cacheGuiExtension()
    {
#ifdef SOLOUD_CLAP_GUI
        checkPluginCall();
        const auto *gui = static_cast<const clap_plugin_gui_t *>(
            mPlugin->get_extension(mPlugin, CLAP_EXT_GUI));
        if (gui == nullptr)
            return;
        if (gui->is_api_supported == nullptr || gui->create == nullptr ||
            gui->destroy == nullptr || gui->show == nullptr ||
            gui->hide == nullptr)
        {
            logMisbehaving("incomplete gui extension");
            return;
        }
        const bool embeddedCalls = gui->get_size != nullptr &&
                                   gui->can_resize != nullptr &&
                                   gui->set_size != nullptr &&
                                   gui->set_parent != nullptr;
        const bool floatingCalls =
            gui->set_transient != nullptr && gui->suggest_title != nullptr;
        checkPluginCall();
        bool embedded = gui->is_api_supported(mPlugin, CLAP_WINDOW_API_COCOA, false);
        checkPluginCall();
        bool floating = gui->is_api_supported(mPlugin, CLAP_WINDOW_API_COCOA, true);
        if ((embedded && !embeddedCalls) || (floating && !floatingCalls))
            logMisbehaving("incomplete gui extension");
        embedded = embedded && embeddedCalls;
        floating = floating && floatingCalls;
        if (!embedded && !floating)
            return;
        mPluginGui = gui;
        mGuiEmbedded = embedded;
        mGuiFloating = floating;
        mHasEditor.store(true, std::memory_order_release);
#endif
    }

    bool ClapInstance::guiCallable() const noexcept
    {
        const Lifecycle l = lifecycle();
        return mPlugin != nullptr &&
               (l == Lifecycle::inited || l == Lifecycle::activated) &&
               !mTearingDown;
    }

    void ClapInstance::postGuiRequest(uint32_t bits) noexcept
    {
        mGuiRequests.fetch_or(bits, std::memory_order_acq_rel);
    }

    void ClapInstance::storeNewestGuiResize(uint64_t packed) noexcept
    {
        // Concurrent requests: the newest wins.
        uint64_t stored = mGuiRequestedSize.load(std::memory_order_acquire);
        while (guiResizeNewer(guiResizeSerial(packed), guiResizeSerial(stored)) &&
               !mGuiRequestedSize.compare_exchange_weak(
                   stored, packed, std::memory_order_acq_rel,
                   std::memory_order_acquire))
        {
        }
    }

    PlayerErrors ClapInstance::openEditor(const std::string &title)
    {
#ifdef SOLOUD_CLAP_GUI
        if (mPluginGui == nullptr || !callable())
            return invalidParameter;
        if (!mEditor)
            mEditor = std::make_unique<ClapEditor>(*this, mPluginGui,
                                                   mGuiEmbedded, mGuiFloating);
        const bool opened = mEditor->open(title);
        afterPluginCall();
        return opened ? noError : unknownError;
#else
        (void)title;
        return invalidParameter;
#endif
    }

    PlayerErrors ClapInstance::closeEditor()
    {
#ifdef SOLOUD_CLAP_GUI
        if (mPluginGui == nullptr)
            return invalidParameter;
        if (mEditor)
        {
            mEditor->close();
            afterPluginCall();
        }
#endif
        return noError;
    }

    void ClapInstance::serviceEditor()
    {
        const uint32_t bits = mGuiRequests.exchange(0, std::memory_order_acq_rel);
#ifdef SOLOUD_CLAP_GUI
        const uint64_t resize =
            mGuiRequestedSize.load(std::memory_order_acquire);
        if (bits == 0 || !mEditor || !guiCallable())
            return;
        mEditor->service(bits, resize);
        afterPluginCall();
#else
        (void)bits;
#endif
    }

    void ClapInstance::destroyEditor()
    {
#ifdef SOLOUD_CLAP_GUI
        if (mEditor)
        {
            mEditor->close();
            mEditor.reset();
        }
#endif
        mGuiRequests.store(0, std::memory_order_release);
        mEditorVisible.store(false, std::memory_order_release);
        mEditorCreated.store(false, std::memory_order_release);
    }

    // Thread-safe gui callbacks: posted for the pump, except a request_resize
    // on main, which is answered now (plan Phase 4b).

    void CLAP_ABI ClapInstance::hostGuiResizeHintsChanged(const clap_host_t *host)
    {
        if (ClapInstance *self = from(host))
            self->postGuiRequest(kGuiRequestHints);
    }

    bool CLAP_ABI ClapInstance::hostGuiRequestResize(const clap_host_t *host,
                                                     uint32_t width,
                                                     uint32_t height)
    {
        ClapInstance *self = from(host);
        if (self == nullptr || !self->hasEditor() ||
            !self->mEditorCreated.load(std::memory_order_acquire))
            return false;
        // One order for every request, on any thread: a queued request older
        // than one applied on main is dropped (it is superseded, so the
        // plugin already knows the newer size).
        const uint32_t serial =
            self->mGuiResizeSerial.fetch_add(1, std::memory_order_acq_rel) + 1;
        const uint64_t packed = packGuiResize(serial, width, height);
#ifdef SOLOUD_CLAP_GUI
        if (pthread_main_np() != 0 && !self->isAudioThread())
        {
            if (!self->mEditor ||
                !self->mEditor->requestResizeOnMain(width, height, serial))
                return false;
            // Keeps the stored serial recent, so the wrap-safe comparison
            // holds however many requests come on main.
            self->storeNewestGuiResize(packed);
            return true;
        }
#endif
        self->storeNewestGuiResize(packed);
        self->postGuiRequest(kGuiRequestResize);
        return true; // Acknowledged: applied or reverted by the pump.
    }

    bool CLAP_ABI ClapInstance::hostGuiRequestShow(const clap_host_t *host)
    {
        // Only a gui the host created can be shown: the plugin cannot open
        // the editor itself (a close racing this request still wins).
        ClapInstance *self = from(host);
        if (self == nullptr ||
            !self->mEditorCreated.load(std::memory_order_acquire))
            return false;
        self->mGuiRequests.fetch_and(~static_cast<uint32_t>(kGuiRequestHide),
                                     std::memory_order_acq_rel);
        self->postGuiRequest(kGuiRequestShow);
        return true;
    }

    bool CLAP_ABI ClapInstance::hostGuiRequestHide(const clap_host_t *host)
    {
        ClapInstance *self = from(host);
        if (self == nullptr ||
            !self->mEditorCreated.load(std::memory_order_acquire))
            return false;
        self->mGuiRequests.fetch_and(~static_cast<uint32_t>(kGuiRequestShow),
                                     std::memory_order_acq_rel);
        self->postGuiRequest(kGuiRequestHide);
        return true;
    }

    void CLAP_ABI ClapInstance::hostGuiClosed(const clap_host_t *host,
                                              bool wasDestroyed)
    {
        if (ClapInstance *self = from(host))
            self->postGuiRequest(kGuiRequestClosed |
                                 (wasDestroyed ? kGuiRequestDestroyed : 0u));
    }

    // --- Phase 3: parameter cache ------------------------------------------

    std::shared_ptr<const ParamCache> ClapInstance::params() const
    {
        std::lock_guard<std::mutex> lock(mParamsMutex);
        return mParamsCache;
    }

    std::shared_ptr<ParamCache> ClapInstance::buildParamCache()
    {
        auto cache = std::make_shared<ParamCache>();
        if (mPluginParams == nullptr)
            return cache;
        checkPluginCall();
        uint32_t count = mPluginParams->count(mPlugin);
        if (count > kMaxParams)
        {
            logMisbehaving("too many parameters; the list is truncated");
            count = kMaxParams;
        }
        cache->entries.reserve(count);
        for (uint32_t i = 0; i < count; ++i)
        {
            clap_param_info_t info{};
            info.id = CLAP_INVALID_ID;
            checkPluginCall();
            if (!mPluginParams->get_info(mPlugin, i, &info))
            {
                logMisbehaving("params.get_info failed for an index below count");
                continue;
            }
            if (info.id == CLAP_INVALID_ID || !std::isfinite(info.min_value) ||
                !std::isfinite(info.max_value) ||
                !std::isfinite(info.default_value) ||
                info.min_value > info.max_value)
            {
                logMisbehaving("invalid parameter info skipped");
                continue;
            }
            if (cache->byId.count(info.id) != 0)
            {
                logMisbehaving("duplicate parameter id skipped");
                continue;
            }
            ParamEntry entry;
            entry.id = info.id;
            entry.flags = info.flags;
            entry.cookie = info.cookie;
            entry.name = boundedString(info.name, CLAP_NAME_SIZE);
            entry.module = boundedString(info.module, CLAP_PATH_SIZE);
            entry.min = info.min_value;
            entry.max = info.max_value;
            entry.def =
                std::min(std::max(info.default_value, info.min_value),
                         info.max_value);
            cache->byId.emplace(entry.id, cache->entries.size());
            cache->entries.push_back(std::move(entry));
        }
        return cache;
    }

    bool ClapInstance::criticalChange(const ParamCache &from,
                                      const ParamCache &to) const
    {
        if (from.entries.size() != to.entries.size())
            return true;
        for (const ParamEntry &entry : to.entries)
        {
            const ParamEntry *old = from.find(entry.id);
            if (old == nullptr || old->cookie != entry.cookie ||
                old->min != entry.min || old->max != entry.max ||
                (old->flags & ~kNonCriticalParamFlags) !=
                    (entry.flags & ~kNonCriticalParamFlags))
                return true;
        }
        return false;
    }

    namespace
    {
        /// Whether any field Dart shows differs (names, modules, defaults,
        /// non-critical flags), given the same id set.
        bool infoDiffers(const ParamCache &from, const ParamCache &to)
        {
            for (const ParamEntry &entry : to.entries)
            {
                const ParamEntry *old = from.find(entry.id);
                if (old == nullptr || old->name != entry.name ||
                    old->module != entry.module || old->def != entry.def ||
                    old->flags != entry.flags)
                    return true;
            }
            return false;
        }
    } // namespace

    void ClapInstance::outResetLocked() noexcept
    {
        mOutState.fetch_or(kOutBlocked | kOutReasonReset,
                           std::memory_order_acq_rel);
    }

    void ClapInstance::issueOutReset()
    {
        if (mMainHoldsProcessMutex)
        {
            outResetLocked();
            return;
        }
        std::lock_guard<std::mutex> lock(mProcessMutex);
        outResetLocked();
    }

    void ClapInstance::rebuildParams()
    {
        constexpr int kMaxAttempts = 3;
        for (int attempt = 0; attempt < kMaxAttempts; ++attempt)
        {
            // Never rebuilt in place while active with a pending cache: the
            // restart's rebuild, between deactivate and activate, does it.
            if (lifecycle() == Lifecycle::activated &&
                mParamsPending.load(std::memory_order_acquire))
            {
                mRestartRequested.store(true, std::memory_order_release);
                return;
            }
            const uint32_t flags =
                mLatchedRescan.exchange(0, std::memory_order_acq_rel);
            const uint64_t generation =
                mParamsGeneration.load(std::memory_order_acquire);
            const bool pending = mParamsPending.load(std::memory_order_acquire);

            // Plugin calls, outside every lock. A rescan(ALL) from inside
            // them bumps the generation, which the check below detects.
            std::shared_ptr<ParamCache> cache = buildParamCache();

            const bool critical =
                mLastRealCache != nullptr && criticalChange(*mLastRealCache, *cache);
            const bool bump = (pending && mLastRealCache != nullptr) || critical;
            const bool infoChanged = mLastRealCache != nullptr && !critical &&
                                     infoDiffers(*mLastRealCache, *cache);

            // A bump resets the out ring, under mProcessMutex (plan B1).
            std::unique_lock<std::mutex> processLock(mProcessMutex,
                                                     std::defer_lock);
            if (bump && !mMainHoldsProcessMutex)
                processLock.lock();
            bool published = false;
            {
                GuardedLock producer(mEditProducerMutex);
                GuardedLock paramsLock(mParamsMutex);
                if (mParamsGeneration.load(std::memory_order_acquire) == generation)
                {
                    if (!pending && critical)
                    {
                        cache->generation = generation + 1;
                        mParamsGeneration.store(generation + 1,
                                                std::memory_order_release);
                        mEditEpoch.fetch_add(1, std::memory_order_acq_rel);
                    }
                    else
                    {
                        // A pending cache was bumped by the rescan(ALL).
                        cache->generation = generation;
                    }
                    mParamsCache = cache;
                    mParamsPending.store(false, std::memory_order_release);
                    if (bump)
                        outResetLocked();
                    published = true;
                }
            }
            if (processLock.owns_lock())
                processLock.unlock();
            if (published)
            {
                mLastRealCache = cache;
                resolveSlotMappings();
                if (bump)
                    notifyParamsRescan(CLAP_PARAM_RESCAN_ALL);
                else if ((flags & CLAP_PARAM_RESCAN_INFO) != 0 || infoChanged)
                    notifyParamsRescan(CLAP_PARAM_RESCAN_INFO);
                return;
            }
            // Discarded: keep the flags for the retry or the next safe point.
            mLatchedRescan.fetch_or(flags, std::memory_order_acq_rel);
        }
        logMisbehaving("the parameter list kept changing during a rebuild");
        if (lifecycle() == Lifecycle::activated)
            mRestartRequested.store(true, std::memory_order_release);
    }

    bool ClapInstance::settleParamsBeforeActivate()
    {
        if (mParamsPending.load(std::memory_order_acquire) ||
            mLatchedRescan.load(std::memory_order_acquire) != 0)
            rebuildParams();
        return !mParamsPending.load(std::memory_order_acquire);
    }

    void ClapInstance::afterPluginCall()
    {
        if (disposition() == Disposition::retired || mTearingDown)
            return;
        const Lifecycle l = lifecycle();
        if (l != Lifecycle::inited && l != Lifecycle::activated)
            return;
        const bool pending = mParamsPending.load(std::memory_order_acquire);
        const bool latched =
            mLatchedRescan.load(std::memory_order_acquire) != 0;
        if (!pending && !latched)
            return;
        if (l == Lifecycle::activated && pending)
        {
            mRestartRequested.store(true, std::memory_order_release);
            return;
        }
        rebuildParams();
    }

    bool ClapInstance::rebuildStepNeeded() const noexcept
    {
        if (disposition() == Disposition::retired || mTearingDown)
            return false;
        const Lifecycle l = lifecycle();
        if (l != Lifecycle::inited && l != Lifecycle::activated)
            return false;
        return mParamsPending.load(std::memory_order_acquire) ||
               mLatchedRescan.load(std::memory_order_acquire) != 0;
    }

    void ClapInstance::handleRescanAll()
    {
        {
            GuardedLock producer(mEditProducerMutex);
            GuardedLock paramsLock(mParamsMutex);
            mParamsGeneration.fetch_add(1, std::memory_order_acq_rel);
            mEditEpoch.fetch_add(1, std::memory_order_acq_rel);
            mParamsCache = pendingCache();
            mParamsPending.store(true, std::memory_order_release);
        }
#ifdef SOLOUD_CLAP_TEST_HOOKS
        if (auto hook = gRescanAfterPendingHook.load(std::memory_order_acquire))
            hook();
#endif
        if (lifecycle() == Lifecycle::created ||
            disposition() == Disposition::retired || mTearingDown)
            return;
        if (inactiveForRescan())
        {
            issueOutReset();
            return;
        }
        // Illegal while active: the restart's publication resets the ring.
        logMisbehaving("params.rescan(ALL) while the plugin is active");
        mRestartRequested.store(true, std::memory_order_release);
    }

    void ClapInstance::publishLatency(bool notifyChange)
    {
        uint32_t value = 0;
        if (mPluginLatency != nullptr)
        {
            checkPluginCall();
            value = mPluginLatency->get(mPlugin);
        }
        const uint32_t old = mLatency.exchange(value, std::memory_order_acq_rel);
        if (notifyChange && old != value)
            notify(kNotifyLatencyChanged);
    }

    void ClapInstance::notify(uint32_t bits) noexcept
    {
        mNotifyFlags.fetch_or(bits, std::memory_order_acq_rel);
    }

    void ClapInstance::notifyParamsRescan(uint32_t clapFlags) noexcept
    {
        // The flags first: a drain that sees the bit also sees them.
        mRescanNotifyFlags.fetch_or(clapFlags, std::memory_order_acq_rel);
        notify(kNotifyParamsRescan);
    }

    // --- Phase 3: host -> plugin edits -------------------------------------

    PlayerErrors ClapInstance::setParameterValue(clap_id id, double value)
    {
        if (!std::isfinite(value))
            return PlayerErrors::invalidParameter;
        // One critical section (plan R1): snapshot, epoch, push.
        GuardedLock producer(mEditProducerMutex);
        std::shared_ptr<const ParamCache> cache;
        {
            GuardedLock paramsLock(mParamsMutex);
            cache = mParamsCache;
        }
        if (cache->pending)
            return PlayerErrors::unknownError;
        const ParamEntry *entry = cache->find(id);
        if (entry == nullptr || (entry->flags & CLAP_PARAM_IS_READONLY) != 0)
            return PlayerErrors::invalidParameter;
        double plain = std::min(std::max(value, entry->min), entry->max);
        if ((entry->flags & (CLAP_PARAM_IS_STEPPED | CLAP_PARAM_IS_ENUM |
                             CLAP_PARAM_IS_BYPASS)) != 0)
            plain = std::min(std::max(std::round(plain), entry->min), entry->max);
        Edit edit;
        edit.id = id;
        edit.cookie = entry->cookie;
        edit.value = plain;
        edit.requiresProcess =
            (entry->flags & CLAP_PARAM_REQUIRES_PROCESS) != 0;
        edit.generation = cache->generation;
        edit.epoch = mEditEpoch.load(std::memory_order_acquire);
#ifdef SOLOUD_CLAP_TEST_HOOKS
        if (auto hook = gEditProducerHook.load(std::memory_order_acquire))
            hook();
#endif
        if (!mEditRing.tryPush(edit))
            return PlayerErrors::outOfMemory;
        return PlayerErrors::noError;
    }

    bool ClapInstance::editStale(const Edit &edit) const noexcept
    {
        for (int pass = 0; pass < 2; ++pass)
        {
            if (edit.generation ==
                    mParamsGeneration.load(std::memory_order_acquire) &&
                edit.epoch == mEditEpoch.load(std::memory_order_acquire))
                return false;
        }
        return true;
    }

    bool ClapInstance::pushEditEvent(const Edit &edit) noexcept
    {
        clap_event_param_value_t event{};
        event.header.size = sizeof(event);
        event.header.time = 0;
        event.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
        event.header.type = CLAP_EVENT_PARAM_VALUE;
        event.header.flags = 0;
        event.param_id = edit.id;
        event.cookie = edit.cookie;
        event.note_id = -1;
        event.port_index = -1;
        event.channel = -1;
        event.key = -1;
        event.value = edit.value;
        return mInputEvents.tryPush(&event.header);
    }

    void ClapInstance::retainedPush(const Edit &edit) noexcept
    {
        mRetained[(mRetainedHead + mRetainedSize) % kEditCapacity] = edit;
        ++mRetainedSize;
        mRetainedCount.store(mRetainedSize, std::memory_order_release);
    }

    const Edit *ClapInstance::retainedFront() const noexcept
    {
        return mRetainedSize > 0 ? &mRetained[mRetainedHead] : nullptr;
    }

    void ClapInstance::retainedPop() noexcept
    {
        mRetainedHead = (mRetainedHead + 1) % kEditCapacity;
        --mRetainedSize;
        mRetainedCount.store(mRetainedSize, std::memory_order_release);
    }

    void ClapInstance::compactRetained() noexcept
    {
        uint32_t kept = 0;
        for (uint32_t i = 0; i < mRetainedSize; ++i)
        {
            const Edit &edit = mRetained[(mRetainedHead + i) % kEditCapacity];
            if (editStale(edit))
                continue;
            if (kept != i)
                mRetained[(mRetainedHead + kept) % kEditCapacity] = edit;
            ++kept;
        }
        mRetainedSize = kept;
        if (kept == 0)
            mRetainedHead = 0;
        mRetainedCount.store(mRetainedSize, std::memory_order_release);
    }

    void ClapInstance::consumeEditsDsp() noexcept
    {
        uint32_t examined = 0;
        bool full = false;
        // Retained edits are older than every ring entry (plan R2).
        while (examined < kMaxEditsExaminedPerChunk)
        {
            const Edit *edit = retainedFront();
            if (edit == nullptr)
                break;
            ++examined;
            if (!editStale(*edit) && !pushEditEvent(*edit))
            {
                full = true;
                break;
            }
            retainedPop();
        }
        if (!full && mRetainedSize == 0)
        {
            while (examined < kMaxEditsExaminedPerChunk)
            {
                const Edit *edit = mEditRing.peek();
                if (edit == nullptr)
                    break;
                ++examined;
                if (!editStale(*edit) && !pushEditEvent(*edit))
                    break;
                mEditRing.pop();
            }
        }
        if (examined > mMaxExamined.load(std::memory_order_relaxed))
            mMaxExamined.store(examined, std::memory_order_release);
    }

    // --- Phase 5a: filter-slot parameter mapping ---------------------------

    namespace
    {
        /// The plain value a slot value (0..1) maps to: linear over the
        /// parameter's range, rounded for stepped, enum and bypass ones.
        double slotPlain(float slotValue, const SlotMapping &m) noexcept
        {
            const double v =
                std::min(std::max(static_cast<double>(slotValue), 0.0), 1.0);
            if (!(m.max > m.min))
                return m.min;
            double plain = m.min + v * (m.max - m.min);
            if ((m.flags & (CLAP_PARAM_IS_STEPPED | CLAP_PARAM_IS_ENUM |
                            CLAP_PARAM_IS_BYPASS)) != 0)
                plain = std::min(std::max(std::round(plain), m.min), m.max);
            return plain;
        }
    } // namespace

    void ClapInstance::writeSlotLocked(unsigned slot,
                                       const SlotMapping &mapping) noexcept
    {
        SlotCell &cell = mSlotCells[slot];
        const uint32_t seq = cell.seq.load(std::memory_order_relaxed);
        cell.seq.store(seq + 1, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        cell.id.store(mapping.id, std::memory_order_relaxed);
#ifdef SOLOUD_CLAP_TEST_HOOKS
        if (auto hook = gSlotWriteHook.load(std::memory_order_acquire))
            hook();
#endif
        cell.cookie.store(mapping.cookie, std::memory_order_relaxed);
        cell.min.store(mapping.min, std::memory_order_relaxed);
        cell.max.store(mapping.max, std::memory_order_relaxed);
        cell.flags.store(mapping.flags, std::memory_order_relaxed);
        cell.generation.store(mapping.generation, std::memory_order_relaxed);
        cell.version.store(mapping.version, std::memory_order_relaxed);
        cell.baseline.store(mapping.baseline, std::memory_order_relaxed);
        cell.seq.store(seq + 2, std::memory_order_release);
        const uint32_t bit = 1u << slot;
        if (mapping.id != CLAP_INVALID_ID)
            mSlotMappedBits.fetch_or(bit, std::memory_order_acq_rel);
        else
            mSlotMappedBits.fetch_and(~bit, std::memory_order_acq_rel);
    }

    bool ClapInstance::readSlotDsp(unsigned slot,
                                   SlotMapping &out) const noexcept
    {
        const SlotCell &cell = mSlotCells[slot];
        const uint32_t before = cell.seq.load(std::memory_order_acquire);
        if ((before & 1u) != 0)
            return false;
        out.id = cell.id.load(std::memory_order_relaxed);
#ifdef SOLOUD_CLAP_TEST_HOOKS
        if (auto hook = gSlotReadHook.load(std::memory_order_acquire))
            hook();
#endif
        out.cookie = cell.cookie.load(std::memory_order_relaxed);
        out.min = cell.min.load(std::memory_order_relaxed);
        out.max = cell.max.load(std::memory_order_relaxed);
        out.flags = cell.flags.load(std::memory_order_relaxed);
        out.generation = cell.generation.load(std::memory_order_relaxed);
        out.version = cell.version.load(std::memory_order_relaxed);
        out.baseline = cell.baseline.load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        return cell.seq.load(std::memory_order_relaxed) == before;
    }

    SlotMapping ClapInstance::slotMapping(unsigned slot) const
    {
        SlotMapping m;
        if (slot == 0 || slot >= kMaxSlotParams)
            return m;
        GuardedLock lock(mSlotMapMutex);
        const SlotCell &cell = mSlotCells[slot];
        m.id = cell.id.load(std::memory_order_relaxed);
        m.cookie = cell.cookie.load(std::memory_order_relaxed);
        m.min = cell.min.load(std::memory_order_relaxed);
        m.max = cell.max.load(std::memory_order_relaxed);
        m.flags = cell.flags.load(std::memory_order_relaxed);
        m.generation = cell.generation.load(std::memory_order_relaxed);
        m.version = cell.version.load(std::memory_order_relaxed);
        m.baseline = cell.baseline.load(std::memory_order_relaxed);
        return m;
    }

    PlayerErrors ClapInstance::mapSlotParameter(unsigned slot, clap_id id,
                                                float baseline)
    {
        if (slot == 0 || slot >= kMaxSlotParams || id == CLAP_INVALID_ID)
            return PlayerErrors::invalidParameter;
        GuardedLock lock(mSlotMapMutex);
        std::shared_ptr<const ParamCache> cache;
        {
            GuardedLock paramsLock(mParamsMutex);
            cache = mParamsCache;
        }
        if (cache->pending)
            return PlayerErrors::unknownError;
        const ParamEntry *entry = cache->find(id);
        if (entry == nullptr || (entry->flags & CLAP_PARAM_IS_READONLY) != 0)
            return PlayerErrors::invalidParameter;
        SlotMapping m;
        m.id = id;
        m.cookie = entry->cookie;
        m.min = entry->min;
        m.max = entry->max;
        m.flags = entry->flags;
        m.generation = cache->generation;
        m.baseline = std::isfinite(baseline)
                         ? baseline
                         : std::numeric_limits<float>::quiet_NaN();
        if (++mSlotVersionCounter == 0)
            ++mSlotVersionCounter;
        m.version = mSlotVersionCounter;
        writeSlotLocked(slot, m);
        return PlayerErrors::noError;
    }

    PlayerErrors ClapInstance::unmapSlotParameter(unsigned slot)
    {
        if (slot == 0 || slot >= kMaxSlotParams)
            return PlayerErrors::invalidParameter;
        GuardedLock lock(mSlotMapMutex);
        SlotMapping m;
        if (++mSlotVersionCounter == 0)
            ++mSlotVersionCounter;
        m.version = mSlotVersionCounter;
        writeSlotLocked(slot, m);
        return PlayerErrors::noError;
    }

    void ClapInstance::resolveSlotMappings()
    {
        GuardedLock lock(mSlotMapMutex);
        if (mSlotMappedBits.load(std::memory_order_acquire) == 0)
            return;
        std::shared_ptr<const ParamCache> cache;
        {
            GuardedLock paramsLock(mParamsMutex);
            cache = mParamsCache;
        }
        // A newer rescan(ALL) made the list pending again: its rebuild
        // resolves; until then the stale generation keeps DSP out.
        if (cache->pending)
            return;
        for (unsigned slot = 1; slot < kMaxSlotParams; ++slot)
        {
            const SlotCell &cell = mSlotCells[slot];
            const clap_id id = cell.id.load(std::memory_order_relaxed);
            if (id == CLAP_INVALID_ID)
                continue;
            SlotMapping m;
            m.version = cell.version.load(std::memory_order_relaxed);
            const ParamEntry *entry = cache->find(id);
            if (entry == nullptr ||
                (entry->flags & CLAP_PARAM_IS_READONLY) != 0)
            {
                // Gone: the slot is unmapped (a new version).
                if (++mSlotVersionCounter == 0)
                    ++mSlotVersionCounter;
                m.version = mSlotVersionCounter;
                writeSlotLocked(slot, m);
                continue;
            }
            m.id = id;
            m.cookie = entry->cookie;
            m.min = entry->min;
            m.max = entry->max;
            m.flags = entry->flags;
            m.generation = cache->generation;
            m.baseline = cell.baseline.load(std::memory_order_relaxed);
            if (m.cookie == cell.cookie.load(std::memory_order_relaxed) &&
                m.min == cell.min.load(std::memory_order_relaxed) &&
                m.max == cell.max.load(std::memory_order_relaxed) &&
                m.flags == cell.flags.load(std::memory_order_relaxed) &&
                m.generation ==
                    cell.generation.load(std::memory_order_relaxed))
                continue;
            writeSlotLocked(slot, m);
        }
    }

    void ClapInstance::pushSlotEventsDsp(const float *slotParams,
                                         unsigned slotParamCount,
                                         uint64_t slotChangedMask) noexcept
    {
        const uint32_t bits = mSlotMappedBits.load(std::memory_order_acquire);
        if (bits == 0)
            return;
        const uint64_t generation =
            mParamsGeneration.load(std::memory_order_acquire);
        const unsigned count = std::min(slotParamCount, kMaxSlotParams);
        for (unsigned slot = 1; slot < count; ++slot)
        {
            if ((bits & (1u << slot)) == 0)
                continue;
            SlotMapping m;
            if (!readSlotDsp(slot, m) || m.id == CLAP_INVALID_ID)
                continue;
            const float value = slotParams[slot];
            if (!std::isfinite(value))
                continue;
            // Mapped against an older list: wait for the re-resolve, leaving
            // the arming and dedupe state alone (its range may change).
            if (m.generation != generation)
                continue;
            const double plain = slotPlain(value, m);
            if (mSlotArmedVersion[slot] != m.version)
            {
                // A new mapping. With a baseline, the value it was made
                // against counts as sent: a different one goes out once, even
                // when the blocks (and changed bits) in between were skipped.
                // Without one, the standing value is not sent (the plugin
                // keeps its own) unless it changed since the last block.
                mSlotArmedVersion[slot] = m.version;
                mSlotHasLast[slot] = false;
                mSlotUnsentValue[slot] = std::numeric_limits<float>::quiet_NaN();
                if (std::isfinite(m.baseline))
                    mSlotUnsentValue[slot] = m.baseline;
                else if ((slotChangedMask & (1ull << slot)) == 0)
                    mSlotUnsentValue[slot] = value;
            }
            if (std::isfinite(mSlotUnsentValue[slot]))
            {
                // Nothing sent yet: compared as slot values under the
                // current mapping.
                if (plain == slotPlain(mSlotUnsentValue[slot], m))
                    continue;
            }
            else if (mSlotHasLast[slot] && plain == mSlotLastPlain[slot])
            {
                continue;
            }
            clap_event_param_value_t event{};
            event.header.size = sizeof(event);
            event.header.time = 0;
            event.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
            event.header.type = CLAP_EVENT_PARAM_VALUE;
            event.header.flags = 0;
            event.param_id = m.id;
            event.cookie = m.cookie;
            event.note_id = -1;
            event.port_index = -1;
            event.channel = -1;
            event.key = -1;
            event.value = plain;
            if (!mInputEvents.tryPush(&event.header))
                continue; // Full: retried next block; later slots still arm.
            mSlotLastPlain[slot] = plain;
            mSlotHasLast[slot] = true;
            mSlotUnsentValue[slot] = std::numeric_limits<float>::quiet_NaN();
        }
    }

    void ClapInstance::stallRecord() noexcept
    {
        mFlushStalled = true;
        mStallRetained = mRetainedSize;
        mStallEpoch = mEditEpoch.load(std::memory_order_acquire);
        mStallGeneration = mParamsGeneration.load(std::memory_order_acquire);
    }

    bool ClapInstance::stallActive() const noexcept
    {
        return mFlushStalled && lifecycle() == Lifecycle::activated &&
               mRetainedCount.load(std::memory_order_acquire) >= mStallRetained &&
               mEditEpoch.load(std::memory_order_acquire) == mStallEpoch &&
               mParamsGeneration.load(std::memory_order_acquire) ==
                   mStallGeneration;
    }

    void ClapInstance::serviceFlush(int64_t now)
    {
        const Disposition d = disposition();
        if (d == Disposition::retired || d == Disposition::servicing)
            return;
        const Lifecycle l = lifecycle();
        if (l != Lifecycle::inited && l != Lifecycle::activated)
            return;
        if (mPluginParams == nullptr)
        {
            // Nothing to flush; request_process stays pending for the next
            // processed block (Phase 2 contract).
            mFlushRequested.store(false, std::memory_order_release);
            return;
        }
        // A scheduling heuristic, not proof DSP stopped (plan R10).
        const int64_t last = mLastProcessNs.load(std::memory_order_acquire);
        const int64_t idle =
            static_cast<int64_t>(gIdleNs.load(std::memory_order_acquire));
        if (last >= 0 && now - last <= idle)
            return;
        const bool work =
            (!mEditRing.empty() && !stallActive()) ||
            mFlushRequested.load(std::memory_order_acquire) ||
            mProcessRequested.load(std::memory_order_acquire) ||
            (l == Lifecycle::inited &&
             mRetainedCount.load(std::memory_order_acquire) > 0);
        if (!work)
            return;

        std::unique_lock<std::mutex> lock(mProcessMutex, std::try_to_lock);
        if (!lock.owns_lock())
            return; // DSP is processing right now.
        mMainHoldsProcessMutex = true;
        // Taken before the call: a request made inside it stays pending.
        mFlushRequested.exchange(false, std::memory_order_acq_rel);
        mProcessRequested.exchange(false, std::memory_order_acq_rel);
        mFlushStalled = false;
        mInputEvents.clear();

        if (l == Lifecycle::activated)
        {
            AudioRole role(*this);
            compactRetained();
            for (uint64_t n = mEditRing.readable(); n > 0; --n)
            {
                const Edit *edit = mEditRing.peek();
                if (edit == nullptr)
                    break;
                if (editStale(*edit))
                {
                    mEditRing.pop();
                    continue;
                }
                if (edit->requiresProcess)
                {
                    // Not deliverable through an active flush (params.h).
                    if (mRetainedSize >= kEditCapacity)
                    {
                        stallRecord();
                        break;
                    }
                    retainedPush(*edit);
                    mEditRing.pop();
                    continue;
                }
                if (!pushEditEvent(*edit))
                    break;
                mEditRing.pop();
            }
            checkPluginCall();
            mPluginParams->flush(mPlugin, mInputEvents.vtable(), &mOutEvents);
        }
        else
        {
            // Inactive: REQUIRES_PROCESS is deliverable; a main-thread call.
            bool full = false;
            while (const Edit *edit = retainedFront())
            {
                if (!editStale(*edit) && !pushEditEvent(*edit))
                {
                    full = true;
                    break;
                }
                retainedPop();
            }
            if (!full && mRetainedSize == 0)
            {
                for (uint64_t n = mEditRing.readable(); n > 0; --n)
                {
                    const Edit *edit = mEditRing.peek();
                    if (edit == nullptr)
                        break;
                    if (!editStale(*edit) && !pushEditEvent(*edit))
                        break;
                    mEditRing.pop();
                }
            }
            checkPluginCall();
            mPluginParams->flush(mPlugin, mInputEvents.vtable(), &mOutEvents);
        }
        mInputEvents.clear();
        mFlushCount.fetch_add(1, std::memory_order_acq_rel);
        mMainHoldsProcessMutex = false;
        lock.unlock();
        afterPluginCall();
    }

    // --- Phase 3: plugin -> host events ------------------------------------

    bool ClapInstance::outBlocked() const noexcept
    {
        return (mOutState.load(std::memory_order_acquire) & kOutBlocked) != 0;
    }

    bool ClapInstance::outPush(const OutEvent &event) noexcept
    {
        for (;;)
        {
            uint64_t word = mOutState.load(std::memory_order_acquire);
            if ((word & kOutBlocked) != 0)
            {
                if (mOutState.compare_exchange_weak(word, withDrop(word),
                                                    std::memory_order_acq_rel,
                                                    std::memory_order_acquire))
                    return false;
                continue; // Changed (perhaps cleared by the consumer).
            }
            if (mOutRing.tryPush(event))
                return true;
            if (mOutState.compare_exchange_strong(
                    word, withDrop(word | kOutBlocked | kOutReasonOverflow),
                    std::memory_order_acq_rel, std::memory_order_acquire))
                return false;
        }
    }

    bool CLAP_ABI ClapInstance::outTryPush(const clap_output_events_t *list,
                                           const clap_event_header_t *event)
    {
        if (list == nullptr || event == nullptr || list->ctx == nullptr)
            return false;
        auto *self = static_cast<ClapInstance *>(list->ctx);
        OutEvent out;
        bool carried = false;
        if (event->space_id == CLAP_CORE_EVENT_SPACE_ID)
        {
            switch (event->type)
            {
            case CLAP_EVENT_PARAM_VALUE:
                if (event->size >= sizeof(clap_event_param_value_t))
                {
                    const auto *value =
                        reinterpret_cast<const clap_event_param_value_t *>(event);
                    out.kind = HostEventType::paramValue;
                    out.paramId = value->param_id;
                    out.value = value->value;
                    carried = true;
                }
                break;
            case CLAP_EVENT_PARAM_GESTURE_BEGIN:
            case CLAP_EVENT_PARAM_GESTURE_END:
                if (event->size >= sizeof(clap_event_param_gesture_t))
                {
                    const auto *gesture =
                        reinterpret_cast<const clap_event_param_gesture_t *>(
                            event);
                    out.kind = event->type == CLAP_EVENT_PARAM_GESTURE_BEGIN
                                   ? HostEventType::gestureBegin
                                   : HostEventType::gestureEnd;
                    out.paramId = gesture->param_id;
                    out.value = 0;
                    carried = true;
                }
                break;
            default:
                break;
            }
        }
        if (!carried)
        {
            // v1 hosts effects: other events are accepted and discarded.
            self->mDiscardedOutEvents.fetch_add(1, std::memory_order_acq_rel);
            return true;
        }
        out.dontRecord = (event->flags & CLAP_EVENT_DONT_RECORD) != 0;
        return self->outPush(out);
    }

    int ClapInstance::drainEvents(int capacity, int *outTypes,
                                  unsigned *outParamIds, double *outValues,
                                  unsigned *outFlags)
    {
        if (capacity <= 0 || outTypes == nullptr || outParamIds == nullptr ||
            outValues == nullptr || outFlags == nullptr)
            return 0;
        std::lock_guard<std::mutex> lock(mDrainMutex);
        int written = 0;
        auto emit = [&](HostEventType type, unsigned paramId, double value,
                        unsigned flags) {
            outTypes[written] = static_cast<int>(type);
            outParamIds[written] = paramId;
            outValues[written] = value;
            outFlags[written] = flags;
            ++written;
        };
        bool enteredNotifications = false;
        for (;;)
        {
            if (!mDrainNotifications)
            {
                // Ring phase (plan arbitration B1).
                if (!mPrefixValid)
                {
                    const uint64_t word = mOutState.load(std::memory_order_acquire);
                    mPrefixEnd = mOutRing.writePos();
                    mResyncAtEnd = (word & kOutBlocked) != 0;
                    mPrefixValid = true;
                }
                for (;;)
                {
                    while (mOutRing.readPos() < mPrefixEnd)
                    {
                        if (written >= capacity)
                            return written;
                        const OutEvent &event = mOutRing.at(mOutRing.readPos());
                        emit(event.kind, event.paramId, event.value,
                             event.dontRecord ? 1u : 0u);
                        mOutRing.pop();
                    }
                    if (mResyncAtEnd)
                    {
                        if (written >= capacity)
                            return written;
                        uint64_t word = mOutState.load(std::memory_order_acquire);
#ifdef SOLOUD_CLAP_TEST_HOOKS
                        if (auto hook = gDrainBeforeAckHook.load(
                                std::memory_order_acquire))
                            hook();
#endif
                        {
                            // Never announce a reset the cache cannot answer
                            // yet. The pending check and the acknowledgement
                            // are one critical section with every pending
                            // transition (handleRescanAll and rebuildParams
                            // change mParamsPending under mParamsMutex), so a
                            // rescan(ALL), inactive or active, cannot slip
                            // between them: a reset made outside the lock
                            // follows a pending store made inside it. A CAS
                            // failure (a DSP drop, a reset) re-checks here.
                            // mParamsMutex is a leaf: no plugin call, no
                            // other lock under it.
                            GuardedLock paramsLock(mParamsMutex);
#ifdef SOLOUD_CLAP_TEST_HOOKS
                            bool hookRan = false;
#endif
                            for (;;)
                            {
                                if ((word & kOutReasonReset) != 0 &&
                                    mParamsPending.load(std::memory_order_acquire))
                                    return written;
#ifdef SOLOUD_CLAP_TEST_HOOKS
                                if (!hookRan)
                                {
                                    hookRan = true;
                                    if (auto hook = gDrainInsideAckHook.load(
                                            std::memory_order_acquire))
                                        hook();
                                }
#endif
                                if (mOutState.compare_exchange_weak(
                                        word, 0, std::memory_order_acq_rel,
                                        std::memory_order_acquire))
                                    break;
                            }
                        }
                        unsigned reasons = 0;
                        if ((word & kOutReasonOverflow) != 0)
                            reasons |= kResyncOverflow;
                        if ((word & kOutReasonReset) != 0)
                            reasons |= kResyncParamsReset;
                        emit(HostEventType::resync, 0,
                             static_cast<double>(word & kOutCountMask), reasons);
                        mResyncAtEnd = false;
                    }
                    mNotifyPending |=
                        mNotifyFlags.exchange(0, std::memory_order_acq_rel);
                    mRescanPending |=
                        mRescanNotifyFlags.exchange(0, std::memory_order_acq_rel);
                    const uint64_t word = mOutState.load(std::memory_order_acquire);
                    if ((word & kOutBlocked) == 0)
                        break;
                    // Blocked since the prefix: extend it to the block.
                    mPrefixEnd = mOutRing.writePos();
                    mResyncAtEnd = true;
                }
                mPrefixValid = false;
                mDrainNotifications = true;
                enteredNotifications = true;
            }

            // Notifications phase, in R11 order.
            static constexpr uint32_t kOrder[] = {
                kNotifyRestartFailed, kNotifyRestarted, kNotifyLatencyChanged,
                kNotifyParamsRescan, kNotifyStateDirty, kNotifyEditorClosed};
            for (const uint32_t bit : kOrder)
            {
                if ((mNotifyPending & bit) == 0)
                    continue;
                if (bit == kNotifyParamsRescan && mRescanPending == 0)
                {
                    mNotifyPending &= ~bit;
                    continue;
                }
                if (written >= capacity)
                    return written;
                switch (bit)
                {
                case kNotifyRestartFailed:
                    emit(HostEventType::restartFailed, 0, 0, 0);
                    break;
                case kNotifyRestarted:
                    emit(HostEventType::restarted, 0,
                         static_cast<double>(restartCount()), 0);
                    break;
                case kNotifyLatencyChanged:
                    emit(HostEventType::latencyChanged, 0,
                         static_cast<double>(latency()), 0);
                    break;
                case kNotifyParamsRescan:
                    emit(HostEventType::paramsRescan, 0, 0, mRescanPending);
                    mRescanPending = 0;
                    break;
                case kNotifyEditorClosed:
                    emit(HostEventType::editorClosed, 0, 0, 0);
                    break;
                default:
                    emit(HostEventType::stateDirty, 0, 0, 0);
                    break;
                }
                mNotifyPending &= ~bit;
            }
            mDrainNotifications = false;
            if (enteredNotifications)
                return written;
            // Finished notifications left over from an earlier call: drain
            // the ring again in this one.
        }
    }

    // --- Phase 3: main hops -------------------------------------------------

    PlayerErrors ClapInstance::getParameterValue(clap_id id, double *outValue)
    {
        if (outValue != nullptr)
            *outValue = 0;
        if (outValue == nullptr)
            return PlayerErrors::invalidParameter;
        if (mPluginParams == nullptr)
            return PlayerErrors::notImplemented;
        double value = 0;
        checkPluginCall();
        const bool ok = mPluginParams->get_value(mPlugin, id, &value);
        afterPluginCall();
        if (!ok || !std::isfinite(value))
            return PlayerErrors::invalidParameter;
        *outValue = value;
        return PlayerErrors::noError;
    }

    PlayerErrors ClapInstance::valueToText(clap_id id, double value,
                                           char *outText, int textLen)
    {
        if (outText != nullptr && textLen > 0)
            outText[0] = '\0';
        if (mPluginParams == nullptr)
            return PlayerErrors::notImplemented;
        if (!std::isfinite(value))
            return PlayerErrors::invalidParameter;
        char text[512] = {};
        checkPluginCall();
        const bool ok = mPluginParams->value_to_text(mPlugin, id, value, text,
                                                     sizeof(text));
        afterPluginCall();
        if (!ok)
            return PlayerErrors::invalidParameter;
        text[sizeof(text) - 1] = '\0';
        copyTruncated(text, outText, textLen);
        return PlayerErrors::noError;
    }

    PlayerErrors ClapInstance::textToValue(clap_id id, const char *text,
                                           double *outValue)
    {
        if (outValue != nullptr)
            *outValue = 0;
        if (text == nullptr || outValue == nullptr)
            return PlayerErrors::invalidParameter;
        if (mPluginParams == nullptr)
            return PlayerErrors::notImplemented;
        double value = 0;
        checkPluginCall();
        const bool ok = mPluginParams->text_to_value(mPlugin, id, text, &value);
        afterPluginCall();
        if (!ok || !std::isfinite(value))
            return PlayerErrors::invalidParameter;
        *outValue = value;
        return PlayerErrors::noError;
    }

    PlayerErrors ClapInstance::saveState(std::vector<unsigned char> &out)
    {
        out.clear();
        if (mPluginState == nullptr)
            return PlayerErrors::notImplemented;
        OstreamContext context{&out, gStateLimit.load(std::memory_order_acquire),
                               false};
        clap_ostream_t stream{&context, &ostreamWrite};
        bool ok = false;
        {
            CallPhaseScope phase(*this, CallPhase::savingState);
            checkPluginCall();
            ok = mPluginState->save(mPlugin, &stream);
        }
        afterPluginCall();
        if (context.overLimit)
        {
            out.clear();
            return PlayerErrors::outOfMemory;
        }
        if (!ok)
        {
            out.clear();
            return PlayerErrors::unknownError;
        }
        return PlayerErrors::noError;
    }

    PlayerErrors ClapInstance::loadState(const unsigned char *data, size_t size)
    {
        if (data == nullptr && size > 0)
            return PlayerErrors::invalidParameter;
        if (mPluginState == nullptr)
            return PlayerErrors::notImplemented;
        IstreamContext context{data, size, 0};
        clap_istream_t stream{&context, &istreamRead};
        bool ok = false;
        {
            CallPhaseScope phase(*this, CallPhase::loadingState);
            checkPluginCall();
            ok = mPluginState->load(mPlugin, &stream);
        }
        if (ok)
        {
            // Edits queued before the load must not override it.
            {
                GuardedLock producer(mEditProducerMutex);
                mEditEpoch.fetch_add(1, std::memory_order_acq_rel);
            }
            notifyParamsRescan(CLAP_PARAM_RESCAN_VALUES | CLAP_PARAM_RESCAN_TEXT);
        }
        afterPluginCall();
        return ok ? PlayerErrors::noError : PlayerErrors::unknownError;
    }

    // --- Phase 3: timers ----------------------------------------------------

    void ClapInstance::fireTimers(int64_t now)
    {
        if (!mTimersEnabled || mTimers.empty() || mPluginTimer == nullptr ||
            !callable())
            return;
        // Due ids now; a timer registered during this pass waits a tick.
        clap_id due[kMaxTimers];
        size_t dueCount = 0;
        for (const Timer &timer : mTimers)
        {
            if (timer.dueNs <= now && dueCount < kMaxTimers)
                due[dueCount++] = timer.id;
        }
        for (size_t i = 0; i < dueCount; ++i)
        {
            if (!mTimersEnabled || !callable())
                break;
            auto it = std::find_if(mTimers.begin(), mTimers.end(),
                                   [&](const Timer &t) { return t.id == due[i]; });
            if (it == mTimers.end())
                continue; // Unregistered by an earlier on_timer.
            const int64_t period =
                static_cast<int64_t>(it->periodMs) * 1000 * 1000;
            it->dueNs += period;
            if (it->dueNs <= now)
                it->dueNs = now + period; // No catch-up bursts.
            checkPluginCall();
            mPluginTimer->on_timer(mPlugin, due[i]);
            afterPluginCall();
            if (disposition() == Disposition::retired)
                break;
        }
    }

    // --- Phase 3: host extension callbacks ---------------------------------

    void CLAP_ABI ClapInstance::hostLog(const clap_host_t *host,
                                        clap_log_severity severity,
                                        const char *msg)
    {
        ClapInstance *self = from(host);
        if (self == nullptr)
            return;
        if (self->isAudioThread())
        {
            // No I/O, no lock and no allocation in the audio role.
            self->mDroppedAudioLogs.fetch_add(1, std::memory_order_acq_rel);
            return;
        }
        emitLog(self->mPluginId.c_str(), severity,
                msg != nullptr ? msg : "(null)");
    }

    void CLAP_ABI ClapInstance::hostParamsRescan(const clap_host_t *host,
                                                 clap_param_rescan_flags flags)
    {
        ClapInstance *self = from(host);
        if (self == nullptr ||
            !self->mainCallbackAllowed("params.rescan off the main thread"))
            return;
        if ((flags & CLAP_PARAM_RESCAN_ALL) != 0)
        {
            self->handleRescanAll();
            return;
        }
        const uint32_t immediate =
            flags & (CLAP_PARAM_RESCAN_VALUES | CLAP_PARAM_RESCAN_TEXT);
        if (immediate != 0)
            self->notifyParamsRescan(immediate);
        if ((flags & CLAP_PARAM_RESCAN_INFO) != 0)
            self->mLatchedRescan.fetch_or(CLAP_PARAM_RESCAN_INFO,
                                          std::memory_order_acq_rel);
    }

    void CLAP_ABI ClapInstance::hostParamsClear(const clap_host_t *host,
                                                clap_id, clap_param_clear_flags)
    {
        ClapInstance *self = from(host);
        if (self == nullptr ||
            !self->mainCallbackAllowed("params.clear off the main thread"))
            return;
        // v1 holds no automation references: drop every queued edit.
        GuardedLock producer(self->mEditProducerMutex);
        self->mEditEpoch.fetch_add(1, std::memory_order_acq_rel);
    }

    void CLAP_ABI ClapInstance::hostParamsRequestFlush(const clap_host_t *host)
    {
        ClapInstance *self = from(host);
        if (self == nullptr)
            return;
        if (self->isAudioThread())
            self->mMisbehavingCount.fetch_add(1, std::memory_order_acq_rel);
        self->mFlushRequested.store(true, std::memory_order_release);
    }

    void CLAP_ABI ClapInstance::hostStateMarkDirty(const clap_host_t *host)
    {
        ClapInstance *self = from(host);
        if (self == nullptr ||
            !self->mainCallbackAllowed("state.mark_dirty off the main thread"))
            return;
        self->notify(kNotifyStateDirty);
    }

    void CLAP_ABI ClapInstance::hostLatencyChanged(const clap_host_t *host)
    {
        ClapInstance *self = from(host);
        if (self == nullptr ||
            !self->mainCallbackAllowed("latency.changed off the main thread"))
            return;
        // Legal only inside activate; the value is read right after it.
        if (self->mPhase != CallPhase::activating)
            self->logMisbehaving(
                "latency.changed outside activate; request a restart instead");
    }

    bool CLAP_ABI ClapInstance::hostRegisterTimer(const clap_host_t *host,
                                                  uint32_t periodMs,
                                                  clap_id *timerId)
    {
        ClapInstance *self = from(host);
        if (self == nullptr || timerId == nullptr)
            return false;
        *timerId = CLAP_INVALID_ID;
        if (!self->mainCallbackAllowed(
                "timer_support.register_timer off the main thread"))
            return false;
        self->resolveTimerExtension();
        if (self->mPluginTimer == nullptr ||
            self->mTimers.size() >= kMaxTimers)
            return false;
        const uint32_t period = std::max(periodMs, kMinTimerPeriodMs);
        if (self->mNextTimerId == CLAP_INVALID_ID)
            self->mNextTimerId = 1;
        const clap_id id = self->mNextTimerId++;
        try
        {
            self->mTimers.push_back(
                {id, period,
                 nowNs() + static_cast<int64_t>(period) * 1000 * 1000});
        }
        catch (const std::bad_alloc &)
        {
            return false;
        }
        *timerId = id;
        return true;
    }

    bool CLAP_ABI ClapInstance::hostUnregisterTimer(const clap_host_t *host,
                                                    clap_id timerId)
    {
        ClapInstance *self = from(host);
        if (self == nullptr ||
            !self->mainCallbackAllowed(
                "timer_support.unregister_timer off the main thread"))
            return false;
        auto it = std::find_if(self->mTimers.begin(), self->mTimers.end(),
                               [&](const Timer &t) { return t.id == timerId; });
        if (it == self->mTimers.end())
            return false;
        self->mTimers.erase(it);
        return true;
    }

    bool CLAP_ABI ClapInstance::hostAudioPortsIsRescanFlagSupported(
        const clap_host_t *host, uint32_t flag)
    {
        ClapInstance *self = from(host);
        if (self == nullptr ||
            !self->mainCallbackAllowed(
                "audio_ports.is_rescan_flag_supported off the main thread"))
            return false;
        // Every port aspect is re-read and re-validated before activate.
        return flag != 0 && (flag & ~kSupportedPortRescans) == 0;
    }

    void CLAP_ABI ClapInstance::hostAudioPortsRescan(const clap_host_t *host,
                                                     uint32_t flags)
    {
        ClapInstance *self = from(host);
        if (self == nullptr ||
            !self->mainCallbackAllowed("audio_ports.rescan off the main thread"))
            return;
        if ((flags & ~CLAP_AUDIO_PORTS_RESCAN_NAMES) == 0)
            return; // Names are not used.
        if (self->inactiveForRescan())
            return; // Re-read before the next activate.
        self->logMisbehaving("audio_ports.rescan while the plugin is active");
        self->mRestartRequested.store(true, std::memory_order_release);
    }
} // namespace soloud_clap

#endif // SOLOUD_ENABLE_CLAP
