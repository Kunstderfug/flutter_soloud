#ifdef SOLOUD_ENABLE_CLAP

#include "clap_instance.h"

#include <pthread.h>

#include <algorithm>
#include <cstring>

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
#endif

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

    ClapInstance::ClapInstance(ModuleRef module, std::string pluginId,
                               double sampleRate, uint32_t maxBlockSize)
        : mModule(std::move(module)), mPluginId(std::move(pluginId)),
          mSampleRate(sampleRate), mMaxBlockSize(maxBlockSize)
    {
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
        if (!mPlugin->init(mPlugin))
            return false;
        setLifecycle(Lifecycle::inited);
        return true;
    }

    bool ClapInstance::activate()
    {
        if (!mPlugin->activate(mPlugin, mSampleRate, 1, mMaxBlockSize))
            return false;
        setLifecycle(Lifecycle::activated);
        return true;
    }

    void ClapInstance::deactivate()
    {
        mPlugin->deactivate(mPlugin);
        setLifecycle(Lifecycle::inited);
    }

    void ClapInstance::destroy()
    {
        const clap_plugin_t *plugin = mPlugin;
        mPlugin->destroy(plugin);
        setLifecycle(Lifecycle::destroyed);
    }

    void ClapInstance::onMainThread() { mPlugin->on_main_thread(mPlugin); }

    bool ClapInstance::readPorts(PortLayout &outLayout, std::string &outError)
    {
        outLayout = PortLayout();
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
        AudioRole role(*this);
        mPlugin->stop_processing(mPlugin);
        mStarted = false;
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
                                    unsigned bufferSize,
                                    unsigned channels) noexcept
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
            return; // Exclusive servicing in progress: a dry block.
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
            processChunk(buffer, offset, frames, bufferSize, busChannels);
        }
    }

    void ClapInstance::processChunk(float *buffer, unsigned offset,
                                    unsigned frames, unsigned bufferSize,
                                    unsigned busChannels) noexcept
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
        mOutputEvents.clear();

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
        process.out_events = mOutputEvents.vtable();

        // A process call answers any pending request_process (plan F7).
        mProcessRequested.store(false, std::memory_order_release);
        const clap_process_status status = mPlugin->process(mPlugin, &process);
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
        if (std::strcmp(id, CLAP_EXT_THREAD_CHECK) == 0)
            return &kThreadCheck;
        // Phase 3 adds log, params, state, latency, gui, timer-support and
        // audio-ports.
        return nullptr;
    }

    void CLAP_ABI ClapInstance::hostRequestRestart(const clap_host_t *host)
    {
        if (ClapInstance *self = from(host))
            self->mRestartRequested.store(true, std::memory_order_release);
    }

    // v1 calls process() on every eligible block, so a request only stays
    // pending (and counted) until the next process() call. While DSP is not
    // running it stays pending; Phase 3's flush servicing consumes it.
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

    bool CLAP_ABI ClapInstance::hostIsMainThread(const clap_host_t *)
    {
        return pthread_main_np() != 0;
    }

    bool CLAP_ABI ClapInstance::hostIsAudioThread(const clap_host_t *host)
    {
        const ClapInstance *self = from(host);
        return self != nullptr && self->isAudioThread();
    }
} // namespace soloud_clap

#endif // SOLOUD_ENABLE_CLAP
