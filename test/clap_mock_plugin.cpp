// A mock CLAP effect bundle for test/clap_host_test.cpp. Built by
// test/run_clap_host_test.sh as MockClap.clap (and, with
// -DMOCK_CLAP_BAD_VERSION, as MockClapBadVersion.clap).
//
// It checks the host: every callback asserts its thread through the host's
// thread-check extension and its lifecycle state, and the entry asserts that
// init/deinit never overlap another call into the DSO.
#include "clap_mock_api.h"

#include <pthread.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace
{
    MockClapState gState;
    std::atomic<MockClapHook> gHook{nullptr};
    std::atomic<int> gInCall{0};
    std::atomic<bool> gEntryInited{false};
    std::mutex gLogMutex;
    std::string gLog;

    /// Counts a call into the DSO for the init/deinit overlap check.
    struct InCall
    {
        InCall() { gInCall.fetch_add(1, std::memory_order_acq_rel); }
        ~InCall() { gInCall.fetch_sub(1, std::memory_order_acq_rel); }
    };

    const char *threadName(const clap_host_t *host,
                           const clap_host_thread_check_t *check)
    {
        if (check == nullptr)
            return pthread_main_np() ? "main" : "other";
        if (check->is_audio_thread(host))
            return "audio";
        return check->is_main_thread(host) ? "main" : "other";
    }

    void logLine(const std::string &line)
    {
        std::lock_guard<std::mutex> lock(gLogMutex);
        gLog += line;
        gLog += '\n';
    }

    void callHook(const char *event, const char *id, const clap_host_t *host)
    {
        if (MockClapHook hook = gHook.load(std::memory_order_acquire))
            hook(event, id, host);
    }

    enum class Kind
    {
        stereo,
        mono,
        aux,
        noports,
        noinput,
        monoInStereoOut,
        stereoInMonoOut,
        failInit,
        failActivate,
        failStart,
        badlayout,
    };

    struct Descriptor
    {
        Kind kind;
        const char *id;
        const char *name;
        uint32_t in;  // main input channels (0: no input port)
        uint32_t out; // main output channels
        bool aux;
        bool ports;
    };

    const char *const kFeatures[] = {CLAP_PLUGIN_FEATURE_AUDIO_EFFECT,
                                     CLAP_PLUGIN_FEATURE_STEREO, nullptr};

    const Descriptor kDescriptors[] = {
        {Kind::stereo, "mock.stereo", "Mock Stereo", 2, 2, false, true},
        {Kind::mono, "mock.mono", "Mock Mono", 1, 1, false, true},
        {Kind::aux, "mock.aux", "Mock Aux", 2, 2, true, true},
        {Kind::noports, "mock.noports", "Mock No Ports", 0, 0, false, false},
        {Kind::noinput, "mock.noinput", "Mock No Input", 0, 2, false, true},
        {Kind::monoInStereoOut, "mock.mono_in_stereo_out", "Mock M>S", 1, 2,
         false, true},
        {Kind::stereoInMonoOut, "mock.stereo_in_mono_out", "Mock S>M", 2, 1,
         false, true},
        {Kind::failInit, "mock.fail_init", "Mock Fail Init", 2, 2, false, true},
        {Kind::failActivate, "mock.fail_activate", "Mock Fail Activate", 2, 2,
         false, true},
        {Kind::failStart, "mock.fail_start", "Mock Fail Start", 2, 2, false,
         true},
        {Kind::badlayout, "mock.badlayout", "Mock Bad Layout", 4, 4, false,
         true},
    };
    constexpr uint32_t kDescriptorCount =
        sizeof(kDescriptors) / sizeof(kDescriptors[0]);

    clap_plugin_descriptor_t gClapDescriptors[kDescriptorCount];

    struct MockPlugin
    {
        clap_plugin_t plugin{};
        const Descriptor *desc = nullptr;
        const clap_host_t *host = nullptr;
        const clap_host_thread_check_t *threadCheck = nullptr;
        // Lifecycle state, checked on every call.
        bool inited = false;
        bool activated = false;
        bool processing = false;
        bool inOnMain = false;
        uint32_t maxFrames = 0;
        uint32_t in = 0;
        uint32_t out = 0;
        int64_t nextSteadyTime = -1;
    };

    MockPlugin *self(const clap_plugin_t *plugin)
    {
        return static_cast<MockPlugin *>(plugin->plugin_data);
    }

    void violation(MockPlugin *p, const char *what)
    {
        gState.violations.fetch_add(1, std::memory_order_acq_rel);
        logLine(std::string(p != nullptr ? p->desc->id : "entry") +
                ":VIOLATION:" + what);
    }

    /// Logs the event and checks the calling thread.
    void enter(MockPlugin *p, const char *event, bool audio)
    {
        const char *thread = threadName(p->host, p->threadCheck);
        logLine(std::string(p->desc->id) + ":" + event + ":" + thread);
        if (p->threadCheck == nullptr)
            return; // Before init: only create, checked with pthread_main_np.
        const bool isAudio = p->threadCheck->is_audio_thread(p->host);
        const bool isMain = p->threadCheck->is_main_thread(p->host);
        if (audio ? !isAudio : (!isMain || isAudio))
            violation(p, event);
    }

    void applyPendingLayout(MockPlugin *p)
    {
        const int in = gState.pendingMainInChannels.exchange(-1);
        const int out = gState.pendingMainOutChannels.exchange(-1);
        if (in >= 0)
            p->in = static_cast<uint32_t>(in);
        if (out >= 0)
            p->out = static_cast<uint32_t>(out);
    }

    // --- audio-ports ------------------------------------------------------

    uint32_t CLAP_ABI portsCount(const clap_plugin_t *plugin, bool isInput)
    {
        InCall call;
        MockPlugin *p = self(plugin);
        if (p->threadCheck != nullptr && !p->threadCheck->is_main_thread(p->host))
            violation(p, "ports_count");
        if (!p->activated)
            applyPendingLayout(p);
        const uint32_t main = isInput ? (p->in > 0 ? 1 : 0) : 1;
        return main + (p->desc->aux ? 1 : 0);
    }

    bool CLAP_ABI portsGet(const clap_plugin_t *plugin, uint32_t index,
                           bool isInput, clap_audio_port_info_t *info)
    {
        InCall call;
        MockPlugin *p = self(plugin);
        if (p->threadCheck != nullptr && !p->threadCheck->is_main_thread(p->host))
            violation(p, "ports_get");
        const bool hasMain = isInput ? p->in > 0 : true;
        const bool isMain = hasMain && index == 0;
        std::memset(info, 0, sizeof(*info));
        info->id = index;
        std::snprintf(info->name, sizeof(info->name), "%s %u",
                      isMain ? "main" : "aux", index);
        const int unflagged = gState.unflaggedMainPorts.load();
        info->flags = isMain && (unflagged & (isInput ? 1 : 2)) == 0
                          ? CLAP_AUDIO_PORT_IS_MAIN
                          : 0;
        info->channel_count = isMain ? (isInput ? p->in : p->out) : 2;
        info->port_type = info->channel_count == 1   ? CLAP_PORT_MONO
                          : info->channel_count == 2 ? CLAP_PORT_STEREO
                                                     : nullptr;
        info->in_place_pair = CLAP_INVALID_ID;
        return true;
    }

    const clap_plugin_audio_ports_t kAudioPorts = {portsCount, portsGet};

    // --- plugin -----------------------------------------------------------

    bool CLAP_ABI pluginInit(const clap_plugin_t *plugin)
    {
        InCall call;
        MockPlugin *p = self(plugin);
        p->threadCheck = static_cast<const clap_host_thread_check_t *>(
            p->host->get_extension(p->host, CLAP_EXT_THREAD_CHECK));
        enter(p, "init", false);
        if (p->threadCheck == nullptr)
            violation(p, "no thread-check");
        if (p->inited)
            violation(p, "init twice");
        callHook("init", p->desc->id, p->host);
        if (p->desc->kind == Kind::failInit)
            return false;
        p->inited = true;
        return true;
    }

    void CLAP_ABI pluginDestroy(const clap_plugin_t *plugin)
    {
        {
            InCall call;
            MockPlugin *p = self(plugin);
            enter(p, "destroy", false);
            if (p->activated)
                violation(p, "destroy while activated");
            callHook("destroy", p->desc->id, p->host);
            delete p;
        }
        gState.liveInstances.fetch_sub(1, std::memory_order_acq_rel);
    }

    bool CLAP_ABI pluginActivate(const clap_plugin_t *plugin, double sampleRate,
                                 uint32_t minFrames, uint32_t maxFrames)
    {
        InCall call;
        MockPlugin *p = self(plugin);
        enter(p, "activate", false);
        if (!p->inited || p->activated)
            violation(p, "activate state");
        if (sampleRate <= 0 || minFrames < 1 || maxFrames < minFrames)
            violation(p, "activate args");
        callHook("activate", p->desc->id, p->host);
        if (p->desc->kind == Kind::failActivate)
            return false;
        int pending = gState.failActivateCount.load();
        while (pending > 0 &&
               !gState.failActivateCount.compare_exchange_weak(pending,
                                                               pending - 1))
        {
        }
        if (pending > 0)
            return false;
        p->activated = true;
        p->maxFrames = maxFrames;
        p->nextSteadyTime = -1;
        return true;
    }

    void CLAP_ABI pluginDeactivate(const clap_plugin_t *plugin)
    {
        InCall call;
        MockPlugin *p = self(plugin);
        enter(p, "deactivate", false);
        if (!p->activated || p->processing)
            violation(p, "deactivate state");
        p->activated = false;
        applyPendingLayout(p);
        callHook("deactivate", p->desc->id, p->host);
    }

    bool CLAP_ABI pluginStartProcessing(const clap_plugin_t *plugin)
    {
        InCall call;
        MockPlugin *p = self(plugin);
        enter(p, "start", true);
        if (!p->activated || p->processing)
            violation(p, "start state");
        callHook("start", p->desc->id, p->host);
        if (p->desc->kind == Kind::failStart)
            return false;
        p->processing = true;
        return true;
    }

    void CLAP_ABI pluginStopProcessing(const clap_plugin_t *plugin)
    {
        InCall call;
        MockPlugin *p = self(plugin);
        enter(p, "stop", true);
        if (!p->processing)
            violation(p, "stop state");
        p->processing = false;
        callHook("stop", p->desc->id, p->host);
    }

    void CLAP_ABI pluginReset(const clap_plugin_t *plugin)
    {
        InCall call;
        (void)plugin;
    }

    bool checkBuffers(MockPlugin *p, const clap_audio_buffer_t *buffers,
                      uint32_t count, bool isInput)
    {
        const uint32_t expected = (isInput ? (p->in > 0 ? 1 : 0) : 1) +
                                  (p->desc->aux ? 1 : 0);
        if (count != expected)
            return false;
        for (uint32_t i = 0; i < count; ++i)
        {
            const bool hasMain = isInput ? p->in > 0 : true;
            const uint32_t ch =
                hasMain && i == 0 ? (isInput ? p->in : p->out) : 2;
            if (buffers[i].channel_count != ch || buffers[i].data32 == nullptr)
                return false;
        }
        return true;
    }

    clap_process_status CLAP_ABI pluginProcess(const clap_plugin_t *plugin,
                                               const clap_process_t *process)
    {
        InCall call;
        MockPlugin *p = self(plugin);
        if (p->threadCheck == nullptr || !p->threadCheck->is_audio_thread(p->host))
            violation(p, "process thread");
        if (!p->processing)
            violation(p, "process state");
        const uint32_t frames = process->frames_count;
        if (frames == 0 || frames > p->maxFrames)
            violation(p, "frames");
        if (p->nextSteadyTime >= 0 && process->steady_time != p->nextSteadyTime)
            violation(p, "steady_time");
        p->nextSteadyTime = process->steady_time + frames;
        if (process->in_events == nullptr || process->out_events == nullptr ||
            process->in_events->size(process->in_events) != 0)
            violation(p, "events");
        if (!checkBuffers(p, process->audio_inputs, process->audio_inputs_count,
                          true) ||
            !checkBuffers(p, process->audio_outputs,
                          process->audio_outputs_count, false))
        {
            violation(p, "buffers");
            return CLAP_PROCESS_ERROR;
        }
        const int callNo = gState.processCalls.fetch_add(1) + 1;
        gState.lastFrames.store(static_cast<int>(frames));

        if (gState.restartFromAudio.load() > 0 &&
            gState.restartFromAudio.fetch_sub(1) > 0)
            p->host->request_restart(p->host);
        if (gState.callbackFromAudio.load() > 0 &&
            gState.callbackFromAudio.fetch_sub(1) > 0)
            p->host->request_callback(p->host);

        if (p->desc->aux)
        {
            const clap_audio_buffer_t &auxIn = process->audio_inputs[1];
            for (uint32_t c = 0; c < auxIn.channel_count; ++c)
                for (uint32_t i = 0; i < frames; ++i)
                    if (auxIn.data32[c][i] != 0.0f)
                    {
                        violation(p, "aux input not zero");
                        c = auxIn.channel_count;
                        break;
                    }
            const clap_audio_buffer_t &auxOut = process->audio_outputs[1];
            for (uint32_t c = 0; c < auxOut.channel_count; ++c)
                for (uint32_t i = 0; i < frames; ++i)
                    auxOut.data32[c][i] = 7.0f; // Must never reach the bus.
        }

        float *const *out = process->audio_outputs[0].data32;
        const float *const *in =
            p->in > 0 ? process->audio_inputs[0].data32 : nullptr;
        const int every = gState.processErrorEvery.load();
        if (every > 0 && callNo % every == 0)
        {
            for (uint32_t c = 0; c < p->out; ++c)
                for (uint32_t i = 0; i < frames; ++i)
                    out[c][i] = 9.0f; // The host must not use it.
            return CLAP_PROCESS_ERROR;
        }
        for (uint32_t i = 0; i < frames; ++i)
        {
            if (in == nullptr)
            {
                for (uint32_t c = 0; c < p->out; ++c)
                    out[c][i] = 0.25f;
                continue;
            }
            if (p->out == 1)
            {
                // stereo_in_mono_out sums; mono passes through.
                float sum = 0.0f;
                for (uint32_t c = 0; c < p->in; ++c)
                    sum += in[c][i];
                out[0][i] = sum * 0.5f;
                continue;
            }
            for (uint32_t c = 0; c < p->out; ++c)
            {
                const float x = in[c < p->in ? c : 0][i];
                // mono_in_stereo_out: the right channel is inverted.
                out[c][i] = (p->in == 1 && c == 1) ? -0.5f * x : 0.5f * x;
            }
        }
        return CLAP_PROCESS_CONTINUE;
    }

    const void *CLAP_ABI pluginGetExtension(const clap_plugin_t *plugin,
                                            const char *id)
    {
        InCall call;
        MockPlugin *p = self(plugin);
        if (std::strcmp(id, CLAP_EXT_AUDIO_PORTS) == 0 && p->desc->ports)
            return &kAudioPorts;
        return nullptr;
    }

    void CLAP_ABI pluginOnMainThread(const clap_plugin_t *plugin)
    {
        InCall call;
        MockPlugin *p = self(plugin);
        enter(p, "on_main", false);
        if (p->inOnMain)
            violation(p, "on_main reentered");
        p->inOnMain = true;
        callHook("on_main", p->desc->id, p->host);
        p->inOnMain = false;
    }

    // --- factory ------------------------------------------------------------

    uint32_t CLAP_ABI factoryCount(const clap_plugin_factory_t *)
    {
        InCall call;
        return kDescriptorCount;
    }

    const clap_plugin_descriptor_t *CLAP_ABI factoryDescriptor(
        const clap_plugin_factory_t *, uint32_t index)
    {
        InCall call;
        return index < kDescriptorCount ? &gClapDescriptors[index] : nullptr;
    }

    const clap_plugin_t *CLAP_ABI factoryCreate(const clap_plugin_factory_t *,
                                                const clap_host_t *host,
                                                const char *id)
    {
        InCall call;
        if (!gEntryInited.load())
            gState.overlaps.fetch_add(1); // create before init / after deinit
        for (uint32_t i = 0; i < kDescriptorCount; ++i)
        {
            if (std::strcmp(kDescriptors[i].id, id) != 0)
                continue;
            auto *p = new MockPlugin();
            p->desc = &kDescriptors[i];
            p->host = host;
            p->in = p->desc->in;
            p->out = p->desc->out;
            p->plugin.desc = &gClapDescriptors[i];
            p->plugin.plugin_data = p;
            p->plugin.init = pluginInit;
            p->plugin.destroy = pluginDestroy;
            p->plugin.activate = pluginActivate;
            p->plugin.deactivate = pluginDeactivate;
            p->plugin.start_processing = pluginStartProcessing;
            p->plugin.stop_processing = pluginStopProcessing;
            p->plugin.reset = pluginReset;
            p->plugin.process = pluginProcess;
            p->plugin.get_extension = pluginGetExtension;
            p->plugin.on_main_thread = pluginOnMainThread;
            if (!pthread_main_np())
                violation(p, "create thread");
            logLine(std::string(id) + ":create:" +
                    (pthread_main_np() ? "main" : "other"));
            gState.liveInstances.fetch_add(1);
            callHook("create", id, host);
            return &p->plugin;
        }
        return nullptr;
    }

    const clap_plugin_factory_t kFactory = {factoryCount, factoryDescriptor,
                                            factoryCreate};

    // --- entry --------------------------------------------------------------

    bool CLAP_ABI entryInit(const char *path)
    {
        if (gInCall.load() != 0 || gEntryInited.load() || path == nullptr)
            gState.overlaps.fetch_add(1);
        InCall call;
        for (uint32_t i = 0; i < kDescriptorCount; ++i)
        {
            clap_plugin_descriptor_t &d = gClapDescriptors[i];
            d.clap_version = CLAP_VERSION;
            d.id = kDescriptors[i].id;
            d.name = kDescriptors[i].name;
            d.vendor = "Mock Vendor";
            d.url = "";
            d.manual_url = "";
            d.support_url = "";
            d.version = "1.2.3";
            d.description = "mock";
            d.features = kFeatures;
        }
        gState.entryInits.fetch_add(1);
        logLine("entry:entry_init:" +
                std::string(pthread_main_np() ? "main" : "other"));
        callHook("entry_init", nullptr, nullptr);
        gEntryInited.store(true);
        return true;
    }

    void CLAP_ABI entryDeinit()
    {
        if (gInCall.load() != 0 || !gEntryInited.load() ||
            gState.liveInstances.load() != 0)
            gState.overlaps.fetch_add(1);
        InCall call;
        logLine("entry:entry_deinit:" +
                std::string(pthread_main_np() ? "main" : "other"));
        callHook("entry_deinit", nullptr, nullptr);
        gEntryInited.store(false);
        gState.entryDeinits.fetch_add(1);
    }

    const void *CLAP_ABI entryGetFactory(const char *id)
    {
        InCall call;
        if (!gEntryInited.load())
            gState.overlaps.fetch_add(1);
        return std::strcmp(id, CLAP_PLUGIN_FACTORY_ID) == 0 ? &kFactory : nullptr;
    }
} // namespace

extern "C"
{
    __attribute__((visibility("default"))) extern const clap_plugin_entry_t
        clap_entry;
    const clap_plugin_entry_t clap_entry = {
#ifdef MOCK_CLAP_BAD_VERSION
        {0, 9, 0},
#else
        CLAP_VERSION_INIT,
#endif
        entryInit,
        entryDeinit,
        entryGetFactory,
    };

    __attribute__((visibility("default"))) MockClapState *mock_clap_state()
    {
        return &gState;
    }

    __attribute__((visibility("default"))) void mock_clap_set_hook(
        MockClapHook hook)
    {
        gHook.store(hook, std::memory_order_release);
    }

    __attribute__((visibility("default"))) size_t mock_clap_log(char *buffer,
                                                                size_t capacity)
    {
        std::lock_guard<std::mutex> lock(gLogMutex);
        if (buffer != nullptr && capacity > 0)
        {
            const size_t n = std::min(capacity - 1, gLog.size());
            std::memcpy(buffer, gLog.data(), n);
            buffer[n] = '\0';
        }
        return gLog.size();
    }

    __attribute__((visibility("default"))) void mock_clap_reset()
    {
        {
            std::lock_guard<std::mutex> lock(gLogMutex);
            gLog.clear();
        }
        gHook.store(nullptr);
        gState.entryInits = 0;
        gState.entryDeinits = 0;
        gState.overlaps = 0;
        gState.violations = 0;
        gState.processCalls = 0;
        gState.lastFrames = 0;
        gState.restartFromAudio = 0;
        gState.callbackFromAudio = 0;
        gState.failActivateCount = 0;
        gState.processErrorEvery = 0;
        gState.pendingMainInChannels = -1;
        gState.pendingMainOutChannels = -1;
        gState.unflaggedMainPorts = 0;
    }
}
