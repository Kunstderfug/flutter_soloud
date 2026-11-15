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
#include <cstdlib>
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
    std::mutex gReceivedMutex;
    std::vector<MockReceived> gReceived;

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
        ext,
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
        {Kind::ext, "mock.ext", "Mock Extensions", 2, 2, false, true},
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
        // mock.ext
        int variant = 0;
        std::atomic<double> values[5];
        std::atomic<int> inDsp{0};
        bool destroying = false;
        uint32_t latency = 0;
    };

    // --- mock.ext parameters --------------------------------------------------

    struct ParamRec
    {
        clap_id id;
        const char *name;
        const char *module;
        uint32_t flags;
        double min, max, def;
    };

    constexpr uint32_t kAutomatable = CLAP_PARAM_IS_AUTOMATABLE;
    constexpr uint32_t kStepped = CLAP_PARAM_IS_STEPPED;
    // One record set per variant: the cookies are the record addresses.
    ParamRec gParams[2][5] = {
        {
            {10, "Gain", "Main", kAutomatable, 0, 2, 1},
            {20, "Mode", "Main/Settings", kStepped | CLAP_PARAM_IS_ENUM, 0, 3, 0},
            {30, "Meter", "Meters", CLAP_PARAM_IS_READONLY, 0, 1, 0},
            {40, "Oversample", "Main/Settings",
             kStepped | CLAP_PARAM_REQUIRES_PROCESS, 0, 1, 0},
            {50, "Extra", "Main", kAutomatable, 0, 1, 0.5},
        },
        {
            {10, "Gain", "Main", kAutomatable, 0, 2, 1},
            {20, "Mode", "Main/Settings", kStepped | CLAP_PARAM_IS_ENUM, 0, 3, 0},
            {30, "Meter", "Meters", CLAP_PARAM_IS_READONLY, 0, 1, 0},
            {40, "Oversample", "Main/Settings",
             kStepped | CLAP_PARAM_REQUIRES_PROCESS, 0, 1, 0},
            {50, "Extra", "Main", kAutomatable, 0, 1, 0.5},
        },
    };

    uint32_t paramCount(const struct MockPlugin *p);
    /// The slot (0..4) of [id] in the plugin's current variant, or -1.
    int paramSlot(const struct MockPlugin *p, clap_id id);

    MockPlugin *self(const clap_plugin_t *plugin)
    {
        return static_cast<MockPlugin *>(plugin->plugin_data);
    }

    uint32_t paramCount(const MockPlugin *p) { return p->variant == 1 ? 5 : 4; }

    int paramSlot(const MockPlugin *p, clap_id id)
    {
        for (uint32_t i = 0; i < paramCount(p); ++i)
            if (gParams[p->variant][i].id == id)
                return static_cast<int>(i);
        return -1;
    }

    template <typename T>
    const T *hostExt(const MockPlugin *p, const char *id)
    {
        return static_cast<const T *>(p->host->get_extension(p->host, id));
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
        if (!p->activated && isInput && p->desc->kind == Kind::ext)
        {
            int n = gState.rescanAllInPorts.load();
            bool rescan = n < 0;
            while (n > 0 && !gState.rescanAllInPorts.compare_exchange_weak(n, n - 1))
            {
            }
            rescan = rescan || n > 0;
            if (rescan)
            {
                const int variant = gState.portsParamVariant.exchange(-1);
                if (variant >= 0)
                    p->variant = variant;
                if (auto *params = hostExt<clap_host_params_t>(p, CLAP_EXT_PARAMS))
                    params->rescan(p->host, CLAP_PARAM_RESCAN_ALL);
            }
        }
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


    // --- mock.ext: events --------------------------------------------------

    void record(const MockReceived &r)
    {
        std::lock_guard<std::mutex> lock(gReceivedMutex);
        gReceived.push_back(r);
    }

    /// Checks one incoming list: sorted, in the block, only carried param
    /// values. Applies them for mock.ext.
    void applyInputEvents(MockPlugin *p, const clap_input_events_t *in,
                          uint32_t frames, int path)
    {
        if (in == nullptr)
        {
            violation(p, "events null");
            return;
        }
        const uint32_t n = in->size(in);
        uint32_t lastTime = 0;
        for (uint32_t i = 0; i < n; ++i)
        {
            const clap_event_header_t *h = in->get(in, i);
            if (h == nullptr || h->space_id != CLAP_CORE_EVENT_SPACE_ID ||
                h->type != CLAP_EVENT_PARAM_VALUE ||
                h->size != sizeof(clap_event_param_value_t))
            {
                violation(p, "event kind");
                continue;
            }
            if (h->time < lastTime || (frames > 0 && h->time >= frames))
                violation(p, "event time");
            lastTime = h->time;
            const auto *e = reinterpret_cast<const clap_event_param_value_t *>(h);
            if (e->note_id != -1 || e->port_index != -1 || e->channel != -1 ||
                e->key != -1)
                violation(p, "event wildcard");
            if (p->desc->kind != Kind::ext)
            {
                violation(p, "event without params");
                continue;
            }
            const int slot = paramSlot(p, e->param_id);
            if (slot < 0)
            {
                violation(p, "event unknown id");
                continue;
            }
            const ParamRec &rec = gParams[p->variant][slot];
            if (e->cookie != nullptr && e->cookie != &rec)
                violation(p, "stale cookie");
            if (rec.flags & CLAP_PARAM_IS_READONLY)
                violation(p, "readonly set");
            if ((rec.flags & CLAP_PARAM_IS_STEPPED) &&
                e->value != std::floor(e->value))
                violation(p, "stepped value");
            if (e->value < rec.min || e->value > rec.max)
                violation(p, "value range");
            if ((rec.flags & CLAP_PARAM_REQUIRES_PROCESS) && path == 2)
                violation(p, "requires_process via active flush");
            p->values[slot].store(e->value);
            record({e->param_id, e->value, path, h->time});
        }
    }

    bool pushOut(MockPlugin *p, const clap_output_events_t *out, uint16_t type,
                 double value)
    {
        gState.outPushes.fetch_add(1);
        bool ok = false;
        if (type == CLAP_EVENT_PARAM_VALUE)
        {
            clap_event_param_value_t e{};
            e.header.size = sizeof(e);
            e.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
            e.header.type = type;
            e.header.flags = gState.gestureDontRecord.load() != 0
                                 ? CLAP_EVENT_DONT_RECORD
                                 : 0;
            e.param_id = 10;
            e.cookie = &gParams[p->variant][0];
            e.note_id = e.port_index = e.channel = e.key = -1;
            e.value = value;
            ok = out->try_push(out, &e.header);
        }
        else
        {
            clap_event_param_gesture_t e{};
            e.header.size = sizeof(e);
            e.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
            e.header.type = type;
            e.param_id = 10;
            ok = out->try_push(out, &e.header);
        }
        if (!ok)
            gState.outPushFailures.fetch_add(1);
        return ok;
    }

    void emitGesture(MockPlugin *p, const clap_output_events_t *out)
    {
        const int k = std::max(1, gState.gestureValues.load());
        pushOut(p, out, CLAP_EVENT_PARAM_GESTURE_BEGIN, 0);
        for (int i = 0; i < k; ++i)
            pushOut(p, out, CLAP_EVENT_PARAM_VALUE, 2.0 * (i + 1) / k);
        pushOut(p, out, CLAP_EVENT_PARAM_GESTURE_END, 0);
    }

    bool takeOne(std::atomic<int> &counter)
    {
        int n = counter.load();
        while (n > 0 && !counter.compare_exchange_weak(n, n - 1))
        {
        }
        return n > 0;
    }

    /// Process/flush are never concurrent (the host serialises them).
    struct DspScope
    {
        MockPlugin *p;
        explicit DspScope(MockPlugin *plugin) : p(plugin)
        {
            if (p->inDsp.fetch_add(1) != 0)
                violation(p, "process/flush overlap");
        }
        ~DspScope() { p->inDsp.fetch_sub(1); }
    };

    // --- mock.ext: params ----------------------------------------------------

    bool mainOnly(MockPlugin *p, const char *what)
    {
        if (p->threadCheck == nullptr || !p->threadCheck->is_main_thread(p->host) ||
            p->threadCheck->is_audio_thread(p->host))
        {
            violation(p, what);
            return false;
        }
        return true;
    }

    uint32_t CLAP_ABI paramsCount(const clap_plugin_t *plugin)
    {
        InCall call;
        MockPlugin *p = self(plugin);
        mainOnly(p, "params_count");
        return paramCount(p);
    }

    bool CLAP_ABI paramsGetInfo(const clap_plugin_t *plugin, uint32_t index,
                                clap_param_info_t *info)
    {
        InCall call;
        MockPlugin *p = self(plugin);
        mainOnly(p, "params_get_info");
        callHook("get_info", p->desc->id, p->host);
        if (index >= paramCount(p))
            return false;
        const ParamRec &rec = gParams[p->variant][index];
        std::memset(info, 0, sizeof(*info));
        info->id = rec.id;
        info->flags = rec.flags;
        info->cookie = const_cast<ParamRec *>(&rec);
        if (rec.id == 10 && gState.paramNameVariant.load() != 0)
            std::snprintf(info->name, sizeof(info->name), "%s %d", rec.name,
                          gState.paramNameVariant.load());
        else
            std::snprintf(info->name, sizeof(info->name), "%s", rec.name);
        std::snprintf(info->module, sizeof(info->module), "%s", rec.module);
        info->min_value = rec.min;
        info->max_value = rec.max;
        info->default_value = rec.def;
        return true;
    }

    bool CLAP_ABI paramsGetValue(const clap_plugin_t *plugin, clap_id id,
                                 double *out)
    {
        InCall call;
        MockPlugin *p = self(plugin);
        mainOnly(p, "params_get_value");
        callHook("get_value", p->desc->id, p->host);
        const int slot = paramSlot(p, id);
        if (slot < 0)
            return false;
        *out = id == 30 ? gState.meterValue.load() : p->values[slot].load();
        return true;
    }

    bool CLAP_ABI paramsValueToText(const clap_plugin_t *plugin, clap_id id,
                                    double value, char *out, uint32_t size)
    {
        InCall call;
        MockPlugin *p = self(plugin);
        mainOnly(p, "params_value_to_text");
        if (paramSlot(p, id) < 0 || size == 0)
            return false;
        std::snprintf(out, size, "%.2f", value);
        return true;
    }

    bool CLAP_ABI paramsTextToValue(const clap_plugin_t *plugin, clap_id id,
                                    const char *text, double *out)
    {
        InCall call;
        MockPlugin *p = self(plugin);
        mainOnly(p, "params_text_to_value");
        if (paramSlot(p, id) < 0 || text == nullptr || *text == '\0')
            return false;
        char *end = nullptr;
        const double v = std::strtod(text, &end);
        if (end == text || *end != '\0')
            return false;
        *out = v;
        return true;
    }

    void CLAP_ABI paramsFlush(const clap_plugin_t *plugin,
                              const clap_input_events_t *in,
                              const clap_output_events_t *out)
    {
        InCall call;
        MockPlugin *p = self(plugin);
        DspScope dsp(p);
        gState.flushCalls.fetch_add(1);
        const bool audio = p->threadCheck->is_audio_thread(p->host);
        const bool main = p->threadCheck->is_main_thread(p->host);
        logLine(std::string(p->desc->id) + ":flush:" +
                (audio ? "audio" : (main ? "main" : "other")));
        if (p->activated ? !audio : (!main || audio))
            violation(p, "flush thread");
        if (p->processing && !audio)
            violation(p, "flush while processing");
        applyInputEvents(p, in, 0, p->activated ? 2 : 3);
        if (out == nullptr)
            violation(p, "flush out null");
        else if (takeOne(gState.gestureFromFlush))
            emitGesture(p, out);
        callHook("flush", p->desc->id, p->host);
    }

    const clap_plugin_params_t kParamsExt = {paramsCount, paramsGetInfo,
                                             paramsGetValue, paramsValueToText,
                                             paramsTextToValue, paramsFlush};

    // --- mock.ext: state -----------------------------------------------------

    constexpr uint32_t kStateMagic = 0x4D434B53; // 'MCKS'

    bool CLAP_ABI stateSave(const clap_plugin_t *plugin,
                            const clap_ostream_t *stream)
    {
        InCall call;
        MockPlugin *p = self(plugin);
        mainOnly(p, "state_save");
        gState.saves.fetch_add(1);
        callHook("save", p->desc->id, p->host);
        if (gState.failSave.load() != 0)
            return false;
        if (gState.stateEmpty.load() != 0)
            return true;
        std::vector<unsigned char> bytes;
        auto put = [&](const void *data, size_t n) {
            const auto *b = static_cast<const unsigned char *>(data);
            bytes.insert(bytes.end(), b, b + n);
        };
        const uint32_t header[4] = {kStateMagic, 1,
                                    static_cast<uint32_t>(p->variant),
                                    paramCount(p)};
        put(header, sizeof(header));
        for (uint32_t i = 0; i < paramCount(p); ++i)
        {
            const uint32_t id = gParams[p->variant][i].id;
            const double v = p->values[i].load();
            put(&id, sizeof(id));
            put(&v, sizeof(v));
        }
        bytes.resize(bytes.size() +
                         static_cast<size_t>(std::max(0, gState.stateExtraBytes.load())),
                     0xAB);
        const size_t chunk = gState.stateChunk.load() > 0
                                 ? static_cast<size_t>(gState.stateChunk.load())
                                 : bytes.size();
        size_t offset = 0;
        while (offset < bytes.size())
        {
            const size_t n = std::min(chunk, bytes.size() - offset);
            const int64_t written = stream->write(stream, bytes.data() + offset, n);
            if (written <= 0)
                return false;
            offset += static_cast<size_t>(written);
        }
        return true;
    }

    bool CLAP_ABI stateLoad(const clap_plugin_t *plugin,
                            const clap_istream_t *stream)
    {
        InCall call;
        MockPlugin *p = self(plugin);
        mainOnly(p, "state_load");
        gState.loads.fetch_add(1);
        callHook("load", p->desc->id, p->host);
        std::vector<unsigned char> bytes;
        const size_t chunk = gState.stateChunk.load() > 0
                                 ? static_cast<size_t>(gState.stateChunk.load())
                                 : 4096;
        std::vector<unsigned char> buf(chunk);
        for (;;)
        {
            const int64_t n = stream->read(stream, buf.data(), chunk);
            if (n < 0)
                return false;
            if (n == 0)
                break;
            bytes.insert(bytes.end(), buf.data(), buf.data() + n);
        }
        if (gState.failLoad.load() != 0)
            return false;
        if (!bytes.empty())
        {
            uint32_t header[4];
            if (bytes.size() < sizeof(header))
                return false;
            std::memcpy(header, bytes.data(), sizeof(header));
            if (header[0] != kStateMagic || header[1] != 1 ||
                bytes.size() < sizeof(header) + header[3] * 12u)
                return false;
            for (uint32_t i = 0; i < header[3]; ++i)
            {
                uint32_t id;
                double v;
                std::memcpy(&id, bytes.data() + 16 + i * 12, 4);
                std::memcpy(&v, bytes.data() + 20 + i * 12, 8);
                const int slot = paramSlot(p, id);
                if (slot >= 0)
                    p->values[slot].store(v);
            }
        }
        const int variant = gState.loadSwitchesVariant.exchange(-1);
        if (variant >= 0)
        {
            p->variant = variant;
            if (auto *params = hostExt<clap_host_params_t>(p, CLAP_EXT_PARAMS))
                params->rescan(p->host, CLAP_PARAM_RESCAN_ALL);
        }
        return true;
    }

    const clap_plugin_state_t kStateExt = {stateSave, stateLoad};

    // --- mock.ext: latency, timers --------------------------------------------

    uint32_t CLAP_ABI latencyGet(const clap_plugin_t *plugin)
    {
        InCall call;
        MockPlugin *p = self(plugin);
        mainOnly(p, "latency_get");
        if (!p->activated)
            violation(p, "latency_get inactive");
        return p->latency;
    }

    const clap_plugin_latency_t kLatencyExt = {latencyGet};

    void CLAP_ABI timerOnTimer(const clap_plugin_t *plugin, clap_id id)
    {
        InCall call;
        MockPlugin *p = self(plugin);
        mainOnly(p, "on_timer");
        if (p->destroying)
            violation(p, "on_timer after destroy");
        logLine(std::string(p->desc->id) + ":on_timer:" + std::to_string(id));
        gState.onTimerCalls.fetch_add(1);
        callHook("on_timer", p->desc->id, p->host);
    }

    const clap_plugin_timer_support_t kTimerExt = {timerOnTimer};

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
        if (p->desc->kind == Kind::ext)
        {
            p->latency = static_cast<uint32_t>(gState.latency.load());
            if (auto *log = hostExt<clap_host_log_t>(p, CLAP_EXT_LOG))
                log->log(p->host, CLAP_LOG_INFO, "mock.ext init");
            const int ms = gState.registerTimerMs.load();
            auto *timer = hostExt<clap_host_timer_support_t>(p, CLAP_EXT_TIMER_SUPPORT);
            if (ms > 0 && timer != nullptr)
            {
                clap_id id = CLAP_INVALID_ID;
                gState.initTimerRegistered.store(
                    timer->register_timer(p->host, static_cast<uint32_t>(ms), &id)
                        ? 1
                        : 0);
                gState.initTimerId.store(id);
            }
        }
        callHook("init", p->desc->id, p->host);
        if (p->desc->kind == Kind::failInit)
            return false;
        if (p->desc->kind == Kind::ext && gState.failInitExt.load() != 0)
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
            p->destroying = true;
            if (p->desc->kind == Kind::ext && gState.unregisterInDestroy.load())
            {
                auto *timer = hostExt<clap_host_timer_support_t>(
                    p, CLAP_EXT_TIMER_SUPPORT);
                gState.unregisterInDestroyResult.store(
                    timer != nullptr &&
                            timer->unregister_timer(p->host,
                                                    gState.initTimerId.load())
                        ? 1
                        : 0);
            }
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
        const int pendingLatency = gState.pendingLatency.exchange(-1);
        if (p->desc->kind == Kind::ext && pendingLatency >= 0)
        {
            p->latency = static_cast<uint32_t>(pendingLatency);
            if (auto *latency = hostExt<clap_host_latency_t>(p, CLAP_EXT_LATENCY))
                latency->changed(p->host);
        }
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
        if (p->desc->kind == Kind::ext)
        {
            const int variant = gState.pendingParamVariant.exchange(-1);
            bool rescan = gState.rescanAllInDeactivate.exchange(0) != 0;
            if (variant >= 0)
            {
                p->variant = variant;
                rescan = true;
            }
            auto *params = hostExt<clap_host_params_t>(p, CLAP_EXT_PARAMS);
            if (rescan && params != nullptr)
                params->rescan(p->host, CLAP_PARAM_RESCAN_ALL);
        }
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
        int failStart = gState.failStartCount.load();
        while (failStart > 0 &&
               !gState.failStartCount.compare_exchange_weak(failStart,
                                                            failStart - 1))
        {
        }
        if (failStart > 0)
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
        DspScope dsp(p);
        if (process->out_events == nullptr)
            violation(p, "events");
        applyInputEvents(p, process->in_events, frames, 1);
        if (p->desc->kind == Kind::ext && process->out_events != nullptr)
        {
            if (takeOne(gState.gestureFromProcess))
                emitGesture(p, process->out_events);
            const int burst = gState.burstFromProcess.exchange(0);
            for (int i = 0; i < burst; ++i)
                pushOut(p, process->out_events, CLAP_EVENT_PARAM_VALUE,
                        (i % 200) / 100.0);
            if (takeOne(gState.logFromAudio))
                if (auto *log = hostExt<clap_host_log_t>(p, CLAP_EXT_LOG))
                    log->log(p->host, CLAP_LOG_WARNING, "mock.ext audio log");
            callHook("process", p->desc->id, p->host);
        }
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
        if (p->desc->kind != Kind::ext)
            return nullptr;
        if (std::strcmp(id, CLAP_EXT_PARAMS) == 0)
            return &kParamsExt;
        if (std::strcmp(id, CLAP_EXT_STATE) == 0)
            return &kStateExt;
        if (std::strcmp(id, CLAP_EXT_LATENCY) == 0)
            return &kLatencyExt;
        if (std::strcmp(id, CLAP_EXT_TIMER_SUPPORT) == 0)
            return &kTimerExt;
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
            for (uint32_t s = 0; s < 5; ++s)
                p->values[s].store(gParams[0][s].def);
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

    __attribute__((visibility("default"))) size_t mock_clap_received(
        MockReceived *out, size_t capacity)
    {
        std::lock_guard<std::mutex> lock(gReceivedMutex);
        if (out != nullptr)
            std::copy_n(gReceived.begin(), std::min(capacity, gReceived.size()),
                        out);
        return gReceived.size();
    }

    __attribute__((visibility("default"))) void mock_clap_reset()
    {
        {
            std::lock_guard<std::mutex> lock(gLogMutex);
            gLog.clear();
        }
        {
            std::lock_guard<std::mutex> lock(gReceivedMutex);
            gReceived.clear();
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
        gState.pendingParamVariant = -1;
        gState.rescanAllInDeactivate = 0;
        gState.rescanAllInPorts = 0;
        gState.portsParamVariant = -1;
        gState.loadSwitchesVariant = -1;
        gState.paramNameVariant = 0;
        gState.meterValue = 0.0;
        gState.gestureFromProcess = 0;
        gState.gestureFromFlush = 0;
        gState.gestureValues = 1;
        gState.gestureDontRecord = 0;
        gState.burstFromProcess = 0;
        gState.outPushFailures = 0;
        gState.outPushes = 0;
        gState.stateChunk = 0;
        gState.failSave = 0;
        gState.failLoad = 0;
        gState.stateExtraBytes = 0;
        gState.stateEmpty = 0;
        gState.saves = 0;
        gState.loads = 0;
        gState.latency = 0;
        gState.pendingLatency = -1;
        gState.registerTimerMs = 0;
        gState.initTimerRegistered = 0;
        gState.initTimerId = CLAP_INVALID_ID;
        gState.onTimerCalls = 0;
        gState.unregisterInDestroy = 0;
        gState.unregisterInDestroyResult = -1;
        gState.logFromAudio = 0;
        gState.flushCalls = 0;
        gState.failInitExt = 0;
        gState.failStartCount = 0;
    }
}
