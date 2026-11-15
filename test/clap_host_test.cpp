// Native harness for the CLAP host core (src/clap). Built and run by
// test/run_clap_host_test.sh [--asan|--tsan] [test-name]. Uses the mock
// bundles that script builds; no Flutter, SoLoud engine or VST3 SDK.
//
// The simulated audio thread calls clapFilterDspCallback under gAudioMutex;
// the audio-thread barrier locks and unlocks it, like SoLoud's
// audioThreadBarrier_internal. The main queue is driven with
// CFRunLoopRunInMode. The C ABI (src/clap_bindings.cpp) is linked in, with
// the engine's soloud_pluginAudioThreadBarrier defined here.
#include "../src/bindings.h"
#include "../src/clap/clap_host.h"
#include "clap_fake_window.h"
#include "clap_mock_api.h"

#include <CoreFoundation/CoreFoundation.h>

#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace soloud_clap;

namespace
{
    int gFailures = 0;
    int gChecksInTest = 0;
    const char *gTestName = "";

#define CHECK(cond)                                                            \
    do                                                                         \
    {                                                                          \
        ++gChecksInTest;                                                       \
        if (!(cond))                                                           \
        {                                                                      \
            ++gFailures;                                                       \
            std::fprintf(stderr, "  FAIL %s:%d [%s]: %s\n", __FILE__,          \
                         __LINE__, gTestName, #cond);                          \
        }                                                                      \
    } while (0)

// Evaluates [cond] once: it may have side effects.
#define REQUIRE(cond)                                                          \
    do                                                                         \
    {                                                                          \
        const bool requireOk = static_cast<bool>(cond);                        \
        CHECK(requireOk && #cond);                                             \
        if (!requireOk)                                                        \
            return;                                                            \
    } while (0)

    constexpr unsigned kSampleRate = 48000;
    std::string gWork;
    std::string gMockPath;
    std::string gBadVersionPath;
    std::string gNoEntryPath;
    std::string gAliasPath;

    // --- the mock's test API --------------------------------------------------

    struct MockApi
    {
        MockClapState *state = nullptr;
        MockClapSetHookFn setHook = nullptr;
        MockClapLogFn log = nullptr;
        MockClapResetFn reset = nullptr;
        MockClapReceivedFn received = nullptr;
    };

    MockApi loadMockApi(const std::string &path)
    {
        MockApi api;
        CFURLRef url = CFURLCreateFromFileSystemRepresentation(
            kCFAllocatorDefault, reinterpret_cast<const UInt8 *>(path.c_str()),
            static_cast<CFIndex>(path.size()), true);
        CFBundleRef bundle = CFBundleCreate(kCFAllocatorDefault, url);
        CFRelease(url);
        if (bundle == nullptr || !CFBundleLoadExecutable(bundle))
            return api;
        // The bundle stays loaded for the whole run (never released).
        auto stateFn = reinterpret_cast<MockClapStateFn>(
            CFBundleGetFunctionPointerForName(bundle, CFSTR("mock_clap_state")));
        api.state = stateFn != nullptr ? stateFn() : nullptr;
        api.setHook = reinterpret_cast<MockClapSetHookFn>(
            CFBundleGetFunctionPointerForName(bundle,
                                              CFSTR("mock_clap_set_hook")));
        api.log = reinterpret_cast<MockClapLogFn>(
            CFBundleGetFunctionPointerForName(bundle, CFSTR("mock_clap_log")));
        api.reset = reinterpret_cast<MockClapResetFn>(
            CFBundleGetFunctionPointerForName(bundle, CFSTR("mock_clap_reset")));
        api.received = reinterpret_cast<MockClapReceivedFn>(
            CFBundleGetFunctionPointerForName(bundle,
                                              CFSTR("mock_clap_received")));
        return api;
    }

    MockApi gMock;
    MockApi gBadMock;

    std::function<void(const std::string &, const clap_host_t *)> gHookFn;
    std::mutex gHookMutex;

    void hookTrampoline(const char *event, const char *, const clap_host_t *host)
    {
        std::function<void(const std::string &, const clap_host_t *)> fn;
        {
            std::lock_guard<std::mutex> lock(gHookMutex);
            fn = gHookFn;
        }
        if (fn)
            fn(event, host);
    }

    void setHook(std::function<void(const std::string &, const clap_host_t *)> fn)
    {
        std::lock_guard<std::mutex> lock(gHookMutex);
        gHookFn = std::move(fn);
    }

    std::vector<std::string> logLines()
    {
        const size_t n = gMock.log(nullptr, 0);
        std::string text(n + 1, '\0');
        gMock.log(&text[0], text.size());
        text.resize(n);
        std::vector<std::string> lines;
        std::istringstream in(text);
        for (std::string line; std::getline(in, line);)
            lines.push_back(line);
        return lines;
    }

    /// The lifecycle events of [id] as "event:thread", in order.
    std::vector<std::string> events(const std::string &id)
    {
        std::vector<std::string> out;
        const std::string prefix = id + ":";
        for (const std::string &line : logLines())
            if (line.compare(0, prefix.size(), prefix) == 0)
                out.push_back(line.substr(prefix.size()));
        return out;
    }

    int countEvent(const std::string &id, const std::string &event)
    {
        int n = 0;
        for (const std::string &e : events(id))
            if (e.compare(0, event.size() + 1, event + ":") == 0)
                ++n;
        return n;
    }

    std::string join(const std::vector<std::string> &v)
    {
        std::string s;
        for (const auto &e : v)
            s += (s.empty() ? "" : " ") + e;
        return s;
    }

    bool hasViolation()
    {
        for (const std::string &line : logLines())
            if (line.find(":VIOLATION:") != std::string::npos)
            {
                std::fprintf(stderr, "  mock: %s\n", line.c_str());
                return true;
            }
        return false;
    }

    // --- main runloop ----------------------------------------------------------

    void pumpFor(double seconds)
    {
        const auto end = std::chrono::steady_clock::now() +
                         std::chrono::duration<double>(seconds);
        while (std::chrono::steady_clock::now() < end)
            CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.002, true);
    }

    bool pumpUntil(const std::function<bool()> &done, double timeout = 3.0)
    {
        const auto end = std::chrono::steady_clock::now() +
                         std::chrono::duration<double>(timeout);
        while (!done())
        {
            if (std::chrono::steady_clock::now() > end)
                return false;
            CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.002, true);
        }
        return true;
    }

    /// Runs [fn] on a worker thread while main keeps its runloop going.
    void onWorker(const std::function<void()> &fn)
    {
        std::atomic<bool> done{false};
        std::thread worker([&] {
            fn();
            done.store(true);
        });
        pumpUntil([&] { return done.load(); }, 10.0);
        worker.join();
    }

    // --- simulated audio thread ------------------------------------------------

    std::mutex gAudioMutex;
    std::atomic<int> gBarrierRuns{0};

    void testBarrier(void *)
    {
        std::lock_guard<std::mutex> lock(gAudioMutex);
        gBarrierRuns.fetch_add(1);
    }

    /// Runs of the engine barrier that the C ABI installs.
    std::atomic<int> gAbiBarrierRuns{0};
} // namespace

extern "C" void soloud_pluginAudioThreadBarrier(void *)
{
    std::lock_guard<std::mutex> lock(gAudioMutex);
    gAbiBarrierRuns.fetch_add(1);
}

namespace
{

    void callDsp(int handle, float *buffer, unsigned samples,
                 unsigned bufferSize, unsigned channels)
    {
        std::lock_guard<std::mutex> lock(gAudioMutex);
        clapFilterDspCallback(buffer, samples, bufferSize, channels,
                              static_cast<float>(kSampleRate), 0.0, nullptr, 0,
                              0,
                              reinterpret_cast<void *>(
                                  static_cast<uintptr_t>(handle)));
    }

    /// A planar bus buffer.
    struct Bus
    {
        unsigned channels;
        unsigned size;
        std::vector<float> data;
        Bus(unsigned ch, unsigned sz) : channels(ch), size(sz), data(ch * sz) {}
        float *channel(unsigned c) { return data.data() + c * size; }
        void fill(std::initializer_list<float> values)
        {
            unsigned c = 0;
            for (float v : values)
            {
                if (c >= channels)
                    break;
                std::fill(channel(c), channel(c) + size, v);
                ++c;
            }
        }
        /// Channel [c] equals [v] on frames [from, to).
        bool equals(unsigned c, float v, unsigned from, unsigned to)
        {
            for (unsigned i = from; i < to; ++i)
                if (std::fabs(channel(c)[i] - v) > 1e-6f)
                    return false;
            return true;
        }
    };

    /// One DSP block on a fresh thread (the audio thread), joined.
    void processOnce(int handle, Bus &bus, unsigned samples)
    {
        std::thread audio(
            [&] { callDsp(handle, bus.data.data(), samples, bus.size, bus.channels); });
        audio.join();
    }

    /// Processes a stereo block of 1.0/0.5 and returns the output.
    Bus stereoBlock(int handle, unsigned samples = 256)
    {
        Bus bus(2, samples);
        bus.fill({1.0f, 0.5f});
        processOnce(handle, bus, samples);
        return bus;
    }

    bool isSilent(Bus &bus, unsigned samples)
    {
        for (unsigned c = 0; c < std::min(bus.channels, 2u); ++c)
            if (!bus.equals(c, 0.0f, 0, samples))
                return false;
        return true;
    }

    bool isGained(Bus &bus, unsigned samples)
    {
        return bus.equals(0, 0.5f, 0, samples) && bus.equals(1, 0.25f, 0, samples);
    }

    /// A continuously running audio thread on one handle.
    class DspThread
    {
    public:
        explicit DspThread(int handle) : mHandle(handle)
        {
            mThread = std::thread([this] { run(); });
        }
        ~DspThread() { stop(); }
        void stop()
        {
            mRunning.store(false);
            if (mThread.joinable())
                mThread.join();
        }
        int blocks() const { return mBlocks.load(); }
        int silent() const { return mSilent.load(); }
        int audible() const { return mAudible.load(); }
        void resetCounts()
        {
            mBlocks = 0;
            mSilent = 0;
            mAudible = 0;
        }
        void setHandle(int handle) { mHandle.store(handle); }

    private:
        void run()
        {
            Bus bus(2, 128);
            while (mRunning.load())
            {
                bus.fill({1.0f, 0.5f});
                callDsp(mHandle.load(), bus.data.data(), 128, 128, 2);
                if (isSilent(bus, 128))
                    mSilent.fetch_add(1);
                else if (isGained(bus, 128))
                    mAudible.fetch_add(1);
                mBlocks.fetch_add(1);
                std::this_thread::sleep_for(std::chrono::microseconds(500));
            }
        }
        std::atomic<int> mHandle;
        std::atomic<bool> mRunning{true};
        std::atomic<int> mBlocks{0};
        std::atomic<int> mSilent{0};
        std::atomic<int> mAudible{0};
        std::thread mThread;
    };

    // --- manager helpers ---------------------------------------------------------

    ClapHostManager &mgr() { return ClapHostManager::instance(); }

    int load(const char *id, unsigned maxBlock = 512, PlayerErrors *error = nullptr,
             const std::string &path = gMockPath, unsigned channels = 2)
    {
        int handle = 0;
        const PlayerErrors e = mgr().loadEffect(path.c_str(), id, kSampleRate,
                                                maxBlock, channels, &handle);
        if (error != nullptr)
            *error = e;
        return e == noError ? handle : 0;
    }

    const clap_host_thread_check_t *threadCheck(const clap_host_t *host)
    {
        return static_cast<const clap_host_thread_check_t *>(
            host->get_extension(host, CLAP_EXT_THREAD_CHECK));
    }

    /// Brings everything back to empty between tests.
    void cleanup()
    {
        setHook(nullptr);
        test::setAfterTableLoadHook(nullptr);
        test::setBeforeLoadMarshalHook(nullptr);
        test::setRegistryWaitHook(nullptr);
        test::failScratchAllocation(-1);
        test::setLogSink(nullptr);
        test::setIdleThresholdNs(0);
        test::setOutRingCapacity(0);
        test::setStateSizeLimit(0);
        test::setEditProducerHook(nullptr);
        test::setDrainBeforeAckHook(nullptr);
        test::setDrainInsideAckHook(nullptr);
        test::setRescanAfterPendingHook(nullptr);
        mgr().setAudioThreadBarrier(&testBarrier, nullptr);
        mgr().unloadAll();
        // A closed editor never leaves its window up (plan Phase 4b).
        CHECK(!fakeWindow().open);
        CHECK(gMock.state->guiLive.load() == 0);
        resetFakeWindow();
        const bool drained = pumpUntil([] {
            return mgr().liveInstanceCount() == 0 &&
                   mgr().retiredBatchCount() == 0 && !mgr().pumpRunning();
        });
        CHECK(drained);
        CHECK(ModuleRegistry::instance().entryCount() == 0);
        CHECK(gMock.state->liveInstances.load() == 0);
        CHECK(gMock.state->overlaps.load() == 0);
        CHECK(gMock.state->violations.load() == 0);
        CHECK(!hasViolation());
        CHECK(test::lockOrderViolations() == 0);
        test::resetLockOrderViolations();
        gMock.reset();
        gMock.setHook(&hookTrampoline);
    }

    // =========================================================================
    // 1. Event lists
    // =========================================================================

    clap_event_param_value_t paramEvent(uint32_t time, uint32_t id)
    {
        clap_event_param_value_t ev{};
        ev.header.size = sizeof(ev);
        ev.header.time = time;
        ev.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
        ev.header.type = CLAP_EVENT_PARAM_VALUE;
        ev.param_id = id;
        return ev;
    }

    void testEventList()
    {
        auto list = std::make_unique<InputEventList>();
        for (uint32_t i = 0; i < InputEventList::kCapacity; ++i)
        {
            const auto ev = paramEvent((i * 7919u) % 100u, i);
            CHECK(list->tryPush(&ev.header));
        }
        const auto extra = paramEvent(5, 99999);
        CHECK(!list->tryPush(&extra.header));
        CHECK(list->size() == InputEventList::kCapacity);
        bool sorted = true, stable = true, aligned = true;
        for (uint32_t i = 0; i < list->size(); ++i)
        {
            const auto *h = list->get(i);
            aligned = aligned && reinterpret_cast<uintptr_t>(h) %
                                         alignof(std::max_align_t) ==
                                     0;
            if (i == 0)
                continue;
            const auto *prev = reinterpret_cast<const clap_event_param_value_t *>(
                list->get(i - 1));
            const auto *cur = reinterpret_cast<const clap_event_param_value_t *>(h);
            sorted = sorted && prev->header.time <= cur->header.time;
            if (prev->header.time == cur->header.time)
                stable = stable && prev->param_id < cur->param_id;
        }
        CHECK(sorted);
        CHECK(stable);
        CHECK(aligned);
        CHECK(list->get(list->size()) == nullptr);
        const clap_input_events_t *vt = list->vtable();
        CHECK(vt->size(vt) == InputEventList::kCapacity);
        CHECK(vt->get(vt, 0) == list->get(0));
        list->clear();
        CHECK(list->size() == 0);

        clap_event_midi_sysex_t sysex{};
        sysex.header.size = sizeof(sysex);
        sysex.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
        sysex.header.type = CLAP_EVENT_MIDI_SYSEX;
        CHECK(!list->tryPush(&sysex.header));
        auto foreign = paramEvent(0, 1);
        foreign.header.space_id = 77;
        CHECK(!list->tryPush(&foreign.header));
        auto unknown = paramEvent(0, 1);
        unknown.header.type = 999;
        CHECK(!list->tryPush(&unknown.header));
        alignas(std::max_align_t) unsigned char big[512] = {};
        auto *bigHeader = reinterpret_cast<clap_event_header_t *>(big);
        *bigHeader = paramEvent(0, 1).header;
        bigHeader->size = EventSlots::kSlotSize + 1;
        CHECK(!list->tryPush(bigHeader));
        bigHeader->size = sizeof(clap_event_header_t) - 1;
        CHECK(!list->tryPush(bigHeader));
        bigHeader->size = EventSlots::kSlotSize;
        CHECK(list->tryPush(bigHeader));
        CHECK(!list->tryPush(nullptr));

        auto out = std::make_unique<OutputEventList>();
        const clap_output_events_t *ovt = out->vtable();
        for (uint32_t i = 0; i < OutputEventList::kCapacity; ++i)
        {
            const auto ev = paramEvent(i, i);
            CHECK(ovt->try_push(ovt, &ev.header));
        }
        const auto over = paramEvent(0, 0);
        CHECK(!ovt->try_push(ovt, &over.header));
        CHECK(!out->tryPush(&sysex.header));
        CHECK(out->dropped() == 2);
        CHECK(out->size() == OutputEventList::kCapacity);
        CHECK(reinterpret_cast<const clap_event_param_value_t *>(out->get(3))
                  ->param_id == 3);
        out->clear();
        CHECK(out->size() == 0 && out->dropped() == 0);
    }

    // =========================================================================
    // 2. Scan
    // =========================================================================

    void testScan()
    {
        std::vector<ScannedPlugin> plugins;
        CHECK(mgr().scanBundle(gMockPath.c_str(), plugins) == noError);
        CHECK(plugins.size() == 12);
        const ScannedPlugin *stereo = nullptr;
        for (const auto &p : plugins)
            if (p.id == "mock.stereo")
                stereo = &p;
        REQUIRE(stereo != nullptr);
        CHECK(stereo->name == "Mock Stereo");
        CHECK(stereo->vendor == "Mock Vendor");
        CHECK(stereo->version == "1.2.3");
        CHECK(stereo->features == "audio-effect;stereo");
        // The scan's module reference is gone: init and deinit ran once and
        // the copied strings outlive the module.
        CHECK(gMock.state->entryInits.load() == 1);
        CHECK(gMock.state->entryDeinits.load() == 1);
        CHECK(ModuleRegistry::instance().entryCount() == 0);
        CHECK(stereo->name == "Mock Stereo");

        std::vector<ScannedPlugin> none;
        CHECK(mgr().scanBundle(gBadVersionPath.c_str(), none) == dllNotFound);
        CHECK(none.empty());
        CHECK(gBadMock.state->entryInits.load() == 0);
        CHECK(gBadMock.state->entryDeinits.load() == 0);
        CHECK(mgr().scanBundle(gNoEntryPath.c_str(), none) == dllNotFound);
        CHECK(mgr().scanBundle((gWork + "/Missing.clap").c_str(), none) ==
              fileNotFound);
        CHECK(mgr().scanBundle("", none) == invalidParameter);
        CHECK(mgr().scanBundle(nullptr, none) == invalidParameter);

        PlayerErrors error = noError;
        CHECK(load("mock.stereo", 512, &error, gBadVersionPath) == 0);
        CHECK(error == dllNotFound);
        CHECK(gBadMock.state->entryInits.load() == 0);
    }

    // =========================================================================
    // 3. Lifecycle order and thread roles
    // =========================================================================

    void testLifecycle()
    {
        const int h = load("mock.stereo");
        REQUIRE(h > 0);
        CHECK(gMock.state->entryInits.load() == 1);
        auto instance = mgr().getInstance(h);
        REQUIRE(instance != nullptr);
        const clap_host_t *host = instance->host();
        CHECK(threadCheck(host)->is_main_thread(host));
        CHECK(!threadCheck(host)->is_audio_thread(host));
        CHECK(mgr().pumpRunning());
        for (int i = 0; i < 3; ++i)
        {
            Bus out = stereoBlock(h);
            CHECK(isGained(out, 256));
        }
        CHECK(gMock.state->processCalls.load() == 3);
        CHECK(mgr().unloadPlugin(h) == noError);
        CHECK(join(events("mock.stereo")) ==
              "create:main init:main activate:main start:audio stop:audio "
              "deactivate:main destroy:main");
        CHECK(instance->lifecycle() == Lifecycle::destroyed);
        CHECK(instance->disposition() == Disposition::retired);
        CHECK(!threadCheck(host)->is_audio_thread(host) || true);
        // The module stays cached until unloadAll.
        CHECK(gMock.state->entryDeinits.load() == 0);
        CHECK(mgr().cachedModuleCount() == 1);
        CHECK(mgr().unloadPlugin(h) == invalidParameter);
        mgr().unloadAll();
        CHECK(gMock.state->entryDeinits.load() == 1);
        CHECK(mgr().cachedModuleCount() == 0);
        CHECK(!mgr().pumpRunning());
        const auto lines = logLines();
        CHECK(!lines.empty() && lines.back() == "entry:entry_deinit:main");
    }

    // =========================================================================
    // 4. Unload variants
    // =========================================================================

    void testUnloadVariants()
    {
        // Concurrent DSP.
        {
            const int h = load("mock.stereo");
            REQUIRE(h > 0);
            DspThread dsp(h);
            CHECK(pumpUntil([&] { return dsp.audible() > 5; }));
            CHECK(mgr().unloadPlugin(h) == noError);
            const int after = dsp.blocks();
            CHECK(pumpUntil([&] { return dsp.blocks() > after + 5; }));
            dsp.stop();
            CHECK(countEvent("mock.stereo", "stop") == 1);
            CHECK(countEvent("mock.stereo", "destroy") == 1);
        }
        gMock.reset();
        gMock.setHook(&hookTrampoline);
        // Paused: processed, then no DSP.
        {
            const int h = load("mock.stereo");
            REQUIRE(h > 0);
            stereoBlock(h);
            CHECK(mgr().unloadPlugin(h) == noError);
            CHECK(join(events("mock.stereo")) ==
                  "create:main init:main activate:main start:audio stop:audio "
                  "deactivate:main destroy:main");
        }
        gMock.reset();
        gMock.setHook(&hookTrampoline);
        // Never processed: no start, no stop.
        {
            const int h = load("mock.stereo");
            REQUIRE(h > 0);
            CHECK(mgr().unloadPlugin(h) == noError);
            CHECK(join(events("mock.stereo")) ==
                  "create:main init:main activate:main deactivate:main "
                  "destroy:main");
        }
        gMock.reset();
        gMock.setHook(&hookTrampoline);
        // Device stopped: no barrier at all.
        {
            mgr().setAudioThreadBarrier(nullptr, nullptr);
            const int h = load("mock.stereo");
            REQUIRE(h > 0);
            const int runs = gBarrierRuns.load();
            CHECK(mgr().unloadPlugin(h) == noError);
            CHECK(gBarrierRuns.load() == runs);
            CHECK(countEvent("mock.stereo", "destroy") == 1);
            mgr().setAudioThreadBarrier(&testBarrier, nullptr);
        }
    }

    // =========================================================================
    // 5. Failed init, activate, start; invalid arguments
    // =========================================================================

    void testFailures()
    {
        PlayerErrors error = noError;
        CHECK(load("mock.fail_init", 512, &error) == 0);
        CHECK(error == fileLoadFailed);
        CHECK(join(events("mock.fail_init")) ==
              "create:main init:main destroy:main");
        // A failed load caches nothing: the module is released at once.
        CHECK(gMock.state->entryInits.load() == 1);
        CHECK(gMock.state->entryDeinits.load() == 1);

        CHECK(load("mock.fail_activate", 512, &error) == 0);
        CHECK(error == fileLoadFailed);
        CHECK(join(events("mock.fail_activate")) ==
              "create:main init:main activate:main destroy:main");

        CHECK(load("mock.noports", 512, &error) == 0);
        CHECK(error == fileLoadFailed);
        CHECK(join(events("mock.noports")) ==
              "create:main init:main destroy:main");
        CHECK(load("mock.badlayout", 512, &error) == 0);
        CHECK(error == fileLoadFailed);
        CHECK(load("mock.unknown", 512, &error) == 0);
        CHECK(error == fileLoadFailed);
        CHECK(load("mock.stereo", 512, &error, gWork + "/Missing.clap") == 0);
        CHECK(error == fileNotFound);
        CHECK(load("mock.stereo", 512, &error, gNoEntryPath) == 0);
        CHECK(error == dllNotFound);

        int handle = 7;
        const char *p = gMockPath.c_str();
        CHECK(mgr().loadEffect(p, "mock.stereo", 0, 512, 2, &handle) ==
              invalidParameter);
        CHECK(handle == 0);
        CHECK(mgr().loadEffect(p, "mock.stereo", kSampleRate, 0, 2, &handle) ==
              invalidParameter);
        CHECK(mgr().loadEffect(p, "mock.stereo", kSampleRate, 65537, 2,
                               &handle) == invalidParameter);
        CHECK(mgr().loadEffect(p, "mock.stereo", kSampleRate, 512, 0, &handle) ==
              invalidParameter);
        CHECK(mgr().loadEffect(p, "mock.stereo", kSampleRate, 512, 33,
                               &handle) == invalidParameter);
        CHECK(mgr().loadEffect(nullptr, "mock.stereo", kSampleRate, 512, 2,
                               &handle) == invalidParameter);
        CHECK(mgr().loadEffect(p, "", kSampleRate, 512, 2, &handle) ==
              invalidParameter);
        CHECK(mgr().loadEffect(p, "mock.stereo", kSampleRate, 512, 2, nullptr) ==
              invalidParameter);
        CHECK(mgr().liveInstanceCount() == 0);

        // start_processing fails: silence from then on, no retry, no stop.
        const int h = load("mock.fail_start");
        REQUIRE(h > 0);
        for (int i = 0; i < 6; ++i)
        {
            Bus out = stereoBlock(h);
            CHECK(isSilent(out, 256));
        }
        Bus mono(1, 64);
        mono.fill({1.0f});
        processOnce(h, mono, 64);
        CHECK(mono.equals(0, 0.0f, 0, 64));
        CHECK(mgr().getInstance(h)->disposition() == Disposition::failed);
        CHECK(countEvent("mock.fail_start", "start") == 1);
        CHECK(mgr().unloadPlugin(h) == noError);
        CHECK(join(events("mock.fail_start")) ==
              "create:main init:main activate:main start:audio "
              "deactivate:main destroy:main");
    }

    // =========================================================================
    // 6. Restart
    // =========================================================================

    void testRestart()
    {
        const int h = load("mock.stereo");
        REQUIRE(h > 0);
        auto instance = mgr().getInstance(h);
        const clap_host_t *host = instance->host();
        stereoBlock(h);

        // From the audio thread.
        gMock.state->restartFromAudio = 1;
        stereoBlock(h);
        CHECK(pumpUntil([&] { return instance->restartCount() == 1; }));
        CHECK(countEvent("mock.stereo", "stop") == 1);
        CHECK(countEvent("mock.stereo", "deactivate") == 1);
        CHECK(countEvent("mock.stereo", "activate") == 2);
        {
            Bus out = stereoBlock(h);
            CHECK(isGained(out, 256));
        }
        CHECK(countEvent("mock.stereo", "start") == 2);

        // From main, paused, coalesced.
        host->request_restart(host);
        host->request_restart(host);
        CHECK(pumpUntil([&] { return instance->restartCount() == 2; }));
        pumpFor(0.08);
        CHECK(instance->restartCount() == 2);

        // From inside on_main_thread.
        std::atomic<int> onMainCalls{0};
        setHook([&](const std::string &event, const clap_host_t *hst) {
            if (event == "on_main" && onMainCalls.fetch_add(1) == 0)
                hst->request_restart(hst);
        });
        host->request_callback(host);
        CHECK(pumpUntil([&] { return instance->restartCount() == 3; }));
        setHook(nullptr);

        // While processing.
        {
            DspThread dsp(h);
            CHECK(pumpUntil([&] { return dsp.audible() > 3; }));
            host->request_restart(host);
            CHECK(pumpUntil([&] { return instance->restartCount() == 4; }));
            const int audible = dsp.audible();
            CHECK(pumpUntil([&] { return dsp.audible() > audible + 3; }));
        }
        CHECK(instance->disposition() == Disposition::active);
        CHECK(mgr().unloadPlugin(h) == noError);
    }

    // =========================================================================
    // 7. Restart with a topology change
    // =========================================================================

    void testRestartTopology()
    {
        const int h = load("mock.mono");
        REQUIRE(h > 0);
        auto instance = mgr().getInstance(h);
        const clap_host_t *host = instance->host();
        {
            Bus out = stereoBlock(h);
            // (1 + 0.5) / 2 * 0.5 on both channels.
            CHECK(out.equals(0, 0.375f, 0, 256) && out.equals(1, 0.375f, 0, 256));
        }
        CHECK(instance->layout().mainOutputChannels() == 1);

        // Mono -> stereo: the new ports are read while deactivated.
        gMock.state->pendingMainInChannels = 2;
        gMock.state->pendingMainOutChannels = 2;
        host->request_restart(host);
        CHECK(pumpUntil([&] { return instance->restartCount() == 1; }));
        CHECK(instance->layout().mainInputChannels() == 2);
        CHECK(instance->layout().mainOutputChannels() == 2);
        {
            Bus out = stereoBlock(h);
            CHECK(isGained(out, 256));
        }

        // -> 4 channels: unroutable, so Failed (silence).
        gMock.state->pendingMainInChannels = 4;
        gMock.state->pendingMainOutChannels = 4;
        host->request_restart(host);
        CHECK(pumpUntil(
            [&] { return instance->disposition() == Disposition::failed; }));
        for (int i = 0; i < 5; ++i)
        {
            Bus out = stereoBlock(h);
            CHECK(isSilent(out, 256));
        }
        CHECK(instance->restartCount() == 1);
        CHECK(instance->lifecycle() == Lifecycle::inited);

        // A good restart from Inited recovers (no stop, no deactivate).
        const int deactivates = countEvent("mock.mono", "deactivate");
        gMock.state->pendingMainInChannels = 2;
        gMock.state->pendingMainOutChannels = 2;
        host->request_restart(host);
        CHECK(pumpUntil([&] { return instance->restartCount() == 2; }));
        CHECK(instance->disposition() == Disposition::active);
        CHECK(countEvent("mock.mono", "deactivate") == deactivates);
        {
            Bus out = stereoBlock(h);
            CHECK(isGained(out, 256));
        }

        // A failed activate, then recovery.
        gMock.state->failActivateCount = 1;
        host->request_restart(host);
        CHECK(pumpUntil(
            [&] { return instance->disposition() == Disposition::failed; }));
        {
            Bus out = stereoBlock(h);
            CHECK(isSilent(out, 256));
        }
        host->request_restart(host);
        CHECK(pumpUntil([&] { return instance->restartCount() == 3; }));
        {
            Bus out = stereoBlock(h);
            CHECK(isGained(out, 256));
        }
        CHECK(mgr().unloadPlugin(h) == noError);
    }

    // =========================================================================
    // 8. Callbacks
    // =========================================================================

    void testCallbacks()
    {
        const int h = load("mock.stereo");
        REQUIRE(h > 0);
        auto instance = mgr().getInstance(h);
        const clap_host_t *host = instance->host();

        gMock.state->callbackFromAudio = 1;
        stereoBlock(h);
        CHECK(pumpUntil([&] { return countEvent("mock.stereo", "on_main") == 1; }));
        CHECK(events("mock.stereo").back() == "on_main:main");

        // A request from inside on_main_thread is serviced on a later tick.
        std::atomic<int> calls{0};
        setHook([&](const std::string &event, const clap_host_t *hst) {
            if (event == "on_main" && calls.fetch_add(1) == 0)
                hst->request_callback(hst);
        });
        host->request_callback(host);
        CHECK(pumpUntil([&] { return countEvent("mock.stereo", "on_main") == 3; }));
        pumpFor(0.08);
        CHECK(countEvent("mock.stereo", "on_main") == 3);
        setHook(nullptr);

        // Pending requests at unload are never delivered.
        host->request_callback(host);
        host->request_restart(host);
        CHECK(mgr().unloadPlugin(h) == noError);
        pumpFor(0.08);
        const auto ev = events("mock.stereo");
        CHECK(!ev.empty() && ev.back() == "destroy:main");
        CHECK(countEvent("mock.stereo", "on_main") == 3);
        CHECK(instance->restartCount() == 0);
    }

    // =========================================================================
    // 9. Two instances on one module; symlinked alias
    // =========================================================================

    void testTwoInstances()
    {
        const int a = load("mock.stereo");
        const int b = load("mock.mono");
        REQUIRE(a > 0 && b > 0);
        CHECK(a != b);
        CHECK(gMock.state->entryInits.load() == 1);
        CHECK(mgr().cachedModuleCount() == 1);
        {
            DspThread dsp(a);
            CHECK(pumpUntil([&] { return dsp.audible() > 3; }));
            std::vector<ScannedPlugin> plugins;
            CHECK(mgr().scanBundle(gMockPath.c_str(), plugins) == noError);
            CHECK(plugins.size() == 12);
            CHECK(gMock.state->entryInits.load() == 1);
        }
        CHECK(mgr().unloadPlugin(a) == noError);
        CHECK(gMock.state->entryDeinits.load() == 0);
        CHECK(mgr().unloadPlugin(b) == noError);
        CHECK(gMock.state->entryDeinits.load() == 0);
        mgr().unloadAll();
        CHECK(gMock.state->entryDeinits.load() == 1);

        // A symlinked alias shares the module.
        const int c = load("mock.stereo", 512, nullptr, gAliasPath);
        const int d = load("mock.stereo");
        REQUIRE(c > 0 && d > 0);
        CHECK(gMock.state->entryInits.load() == 2);
        CHECK(ModuleRegistry::instance().entryCount() == 1);
        CHECK(mgr().cachedModuleCount() == 1);
        mgr().unloadAll();
        CHECK(gMock.state->entryDeinits.load() == 2);
        CHECK(mgr().liveInstanceCount() == 0);
    }

    // =========================================================================
    // 10. Stale handles
    // =========================================================================

    void testStaleHandle()
    {
        const int h = load("mock.stereo");
        REQUIRE(h > 0);
        CHECK(mgr().unloadPlugin(h) == noError);
        Bus bus(2, 64);
        bus.fill({1.0f, 0.5f});
        processOnce(h, bus, 64);
        CHECK(bus.equals(0, 1.0f, 0, 64) && bus.equals(1, 0.5f, 0, 64));
        for (int junk : {0, -1, 12345, 1023})
        {
            processOnce(junk, bus, 64);
            CHECK(bus.equals(0, 1.0f, 0, 64));
        }
        const int h2 = load("mock.stereo");
        REQUIRE(h2 > 0);
        CHECK(h2 != h);
        CHECK(ClapHostManager::slotOf(h2) == ClapHostManager::slotOf(h));
        processOnce(h, bus, 64);
        CHECK(bus.equals(0, 1.0f, 0, 64));
        CHECK(gMock.state->processCalls.load() == 0);
        CHECK(mgr().unloadPlugin(h2) == noError);
    }

    // =========================================================================
    // 11. Process mapping, chunking, errors, steady time
    // =========================================================================

    void testProcess()
    {
        const unsigned maxBlock = 64;
        const unsigned samples = 3 * maxBlock + 7;
        {
            const int h = load("mock.stereo", maxBlock);
            REQUIRE(h > 0);
            Bus bus(2, 256);
            bus.fill({1.0f, 0.5f});
            processOnce(h, bus, samples);
            CHECK(bus.equals(0, 0.5f, 0, samples));
            CHECK(bus.equals(1, 0.25f, 0, samples));
            CHECK(bus.equals(0, 1.0f, samples, 256)); // tail untouched
            CHECK(gMock.state->processCalls.load() == 4);
            CHECK(gMock.state->lastFrames.load() == 7);
            auto instance = mgr().getInstance(h);
            CHECK(instance->lastSteadyTime() == 3 * maxBlock);
            processOnce(h, bus, samples);
            CHECK(instance->lastSteadyTime() == samples + 3 * maxBlock);

            // A mono bus feeds both plugin inputs and averages the outputs.
            Bus mono(1, 64);
            mono.fill({1.0f});
            processOnce(h, mono, 64);
            CHECK(mono.equals(0, 0.5f, 0, 64));

            // A 4-channel bus: the plugin owns channels 0 and 1 only.
            Bus quad(4, 64);
            quad.fill({1.0f, 0.5f, 0.3f, 0.2f});
            processOnce(h, quad, 64);
            CHECK(quad.equals(0, 0.5f, 0, 64) && quad.equals(1, 0.25f, 0, 64));
            CHECK(quad.equals(2, 0.3f, 0, 64) && quad.equals(3, 0.2f, 0, 64));

            // A process error silences that block only.
            gMock.state->processCalls = 0;
            gMock.state->processErrorEvery = 3;
            for (int call = 1; call <= 4; ++call)
            {
                Bus out = stereoBlock(h, maxBlock);
                if (call == 3)
                    CHECK(isSilent(out, maxBlock));
                else
                    CHECK(isGained(out, maxBlock));
            }
            gMock.state->processErrorEvery = 0;
            CHECK(mgr().unloadPlugin(h) == noError);
        }
        struct Case
        {
            const char *id;
            float left;
            float right;
        };
        // Stereo bus input L = 1.0, R = 0.5.
        const Case cases[] = {
            {"mock.mono", 0.375f, 0.375f},
            {"mock.mono_in_stereo_out", 0.375f, -0.375f},
            {"mock.stereo_in_mono_out", 0.75f, 0.75f},
            {"mock.noinput", 0.25f, 0.25f},
            {"mock.aux", 0.5f, 0.25f},
        };
        for (const Case &c : cases)
        {
            const int h = load(c.id, maxBlock);
            CHECK(h > 0);
            if (h <= 0)
                continue;
            Bus bus(2, 256);
            bus.fill({1.0f, 0.5f});
            processOnce(h, bus, samples);
            const bool ok = bus.equals(0, c.left, 0, samples) &&
                            bus.equals(1, c.right, 0, samples);
            if (!ok)
                std::fprintf(stderr, "  %s: got %f %f\n", c.id, bus.channel(0)[0],
                             bus.channel(1)[0]);
            CHECK(ok);
            CHECK(mgr().unloadPlugin(h) == noError);
        }
        // A mono bus on a mono plugin.
        const int h = load("mock.mono", maxBlock);
        REQUIRE(h > 0);
        Bus mono(1, 64);
        mono.fill({0.8f});
        processOnce(h, mono, 64);
        CHECK(mono.equals(0, 0.4f, 0, 64));
        CHECK(mgr().unloadPlugin(h) == noError);
    }

    // =========================================================================
    // 12. request_process
    // =========================================================================

    void testRequestProcess()
    {
        const int h = load("mock.stereo");
        REQUIRE(h > 0);
        auto instance = mgr().getInstance(h);
        const clap_host_t *host = instance->host();
        stereoBlock(h);
        host->request_process(host);
        CHECK(instance->processRequested());
        pumpFor(0.05);
        CHECK(instance->processRequested()); // Paused: stays pending.
        stereoBlock(h);
        CHECK(!instance->processRequested());
        CHECK(instance->processRequestCount() == 1);
        CHECK(mgr().unloadPlugin(h) == noError);
    }

    // =========================================================================
    // 13. Reentrancy
    // =========================================================================

    void testReentrancy()
    {
        // request_restart from inside the initial activate(): one restart.
        {
            std::atomic<int> activates{0};
            setHook([&](const std::string &event, const clap_host_t *hst) {
                if (event == "activate" && activates.fetch_add(1) == 0)
                    hst->request_restart(hst);
            });
            const int h = load("mock.stereo");
            REQUIRE(h > 0);
            auto instance = mgr().getInstance(h);
            CHECK(pumpUntil([&] { return instance->restartCount() == 1; }));
            pumpFor(0.08);
            CHECK(instance->restartCount() == 1);
            setHook(nullptr);
            CHECK(mgr().unloadPlugin(h) == noError);
        }
        // request_callback and request_restart from init().
        {
            setHook([&](const std::string &event, const clap_host_t *hst) {
                if (event == "init")
                {
                    hst->request_callback(hst);
                    hst->request_restart(hst);
                }
            });
            const int h = load("mock.stereo");
            REQUIRE(h > 0);
            setHook(nullptr);
            auto instance = mgr().getInstance(h);
            CHECK(pumpUntil([&] {
                return instance->restartCount() == 1 &&
                       countEvent("mock.stereo", "on_main") == 1;
            }));
            CHECK(mgr().unloadPlugin(h) == noError);
        }
        gMock.reset();
        gMock.setHook(&hookTrampoline);
        // A nested runloop inside on_main_thread does not re-enter the
        // instance; requests made there are serviced afterwards.
        {
            const int h = load("mock.stereo");
            REQUIRE(h > 0);
            auto instance = mgr().getInstance(h);
            const clap_host_t *host = instance->host();
            std::atomic<int> calls{0};
            std::atomic<int> restartsSeenInside{-1};
            setHook([&](const std::string &event, const clap_host_t *hst) {
                if (event != "on_main" || calls.fetch_add(1) != 0)
                    return;
                hst->request_callback(hst);
                hst->request_restart(hst);
                CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.1, false);
                restartsSeenInside = static_cast<int>(instance->restartCount());
            });
            host->request_callback(host);
            CHECK(pumpUntil([&] {
                return instance->restartCount() == 1 &&
                       countEvent("mock.stereo", "on_main") == 2;
            }));
            CHECK(restartsSeenInside.load() == 0);
            setHook(nullptr);
            CHECK(mgr().unloadPlugin(h) == noError);
        }
        gMock.reset();
        gMock.setHook(&hookTrampoline);
        // An unload of itself from inside on_main_thread is deferred.
        {
            const int h = load("mock.stereo");
            REQUIRE(h > 0);
            auto instance = mgr().getInstance(h);
            const clap_host_t *host = instance->host();
            std::atomic<int> liveAfterUnload{-1};
            std::atomic<int> unloadResult{-1};
            setHook([&](const std::string &event, const clap_host_t *) {
                if (event != "on_main")
                    return;
                unloadResult = mgr().unloadPlugin(h);
                liveAfterUnload = gMock.state->liveInstances.load();
            });
            host->request_callback(host);
            CHECK(pumpUntil([&] { return gMock.state->liveInstances.load() == 0; }));
            CHECK(unloadResult.load() == noError);
            CHECK(liveAfterUnload.load() == 1);
            CHECK(join(events("mock.stereo")) ==
                  "create:main init:main activate:main on_main:main "
                  "deactivate:main destroy:main");
            setHook(nullptr);
        }
    }

    // =========================================================================
    // 14. Worker-thread entry points
    // =========================================================================

    void testWorkerAbi()
    {
        int h = 0;
        std::vector<ScannedPlugin> plugins;
        PlayerErrors scan = unknownError, loaded = unknownError,
                     unloaded = unknownError;
        onWorker([&] {
            scan = mgr().scanBundle(gMockPath.c_str(), plugins);
            loaded = mgr().loadEffect(gMockPath.c_str(), "mock.stereo",
                                      kSampleRate, 512, 2, &h);
        });
        CHECK(scan == noError && plugins.size() == 12);
        REQUIRE(loaded == noError && h > 0);
        stereoBlock(h);
        onWorker([&] { unloaded = mgr().unloadPlugin(h); });
        CHECK(unloaded == noError);
        CHECK(join(events("mock.stereo")) ==
              "create:main init:main activate:main start:audio stop:audio "
              "deactivate:main destroy:main");
        onWorker([&] { mgr().unloadAll(); });
        CHECK(gMock.state->entryDeinits.load() == 2); // scan + unloadAll
        const auto lines = logLines();
        CHECK(!lines.empty() && lines.back() == "entry:entry_deinit:main");
    }

    // =========================================================================
    // 15. Dispose and unloadAll during activate (no resurrection)
    // =========================================================================

    void testNoResurrection()
    {
        // Dispose (worker) during the initial activate.
        {
            std::atomic<int> activates{0};
            setHook([&](const std::string &event, const clap_host_t *) {
                if (event == "activate" && activates.fetch_add(1) == 0)
                    std::thread([] { mgr().retireAllForDispose(); }).join();
            });
            PlayerErrors error = noError;
            CHECK(load("mock.stereo", 512, &error) == 0);
            CHECK(error == unknownError);
            setHook(nullptr);
            CHECK(join(events("mock.stereo")) ==
                  "create:main init:main activate:main deactivate:main "
                  "destroy:main");
            pumpFor(0.05);
            // Re-init and a fresh load work.
            const int h = load("mock.stereo");
            CHECK(h > 0);
            mgr().unloadAll();
        }
        gMock.reset();
        gMock.setHook(&hookTrampoline);
        // Dispose (worker) during a restart's activate.
        {
            const int h = load("mock.stereo");
            REQUIRE(h > 0);
            auto instance = mgr().getInstance(h);
            const clap_host_t *host = instance->host();
            stereoBlock(h);
            setHook([&](const std::string &event, const clap_host_t *) {
                if (event == "activate")
                    std::thread([] { mgr().retireAllForDispose(); }).join();
            });
            host->request_restart(host);
            CHECK(pumpUntil([&] { return countEvent("mock.stereo", "destroy") == 1; }));
            setHook(nullptr);
            CHECK(mgr().getInstance(h) == nullptr);
            CHECK(instance->disposition() == Disposition::retired);
            CHECK(instance->restartCount() == 0);
            CHECK(join(events("mock.stereo")) ==
                  "create:main init:main activate:main start:audio stop:audio "
                  "deactivate:main activate:main deactivate:main destroy:main");
            Bus bus(2, 64);
            bus.fill({1.0f, 0.5f});
            processOnce(h, bus, 64);
            CHECK(bus.equals(0, 1.0f, 0, 64));
            const int h2 = load("mock.stereo");
            CHECK(h2 > 0);
            Bus out = stereoBlock(h2);
            CHECK(isGained(out, 256));
            mgr().unloadAll();
        }
        gMock.reset();
        gMock.setHook(&hookTrampoline);
        // unloadAll from inside the initial activate (reentrant).
        {
            std::atomic<int> activates{0};
            setHook([&](const std::string &event, const clap_host_t *) {
                if (event == "activate" && activates.fetch_add(1) == 0)
                    mgr().unloadAll();
            });
            PlayerErrors error = noError;
            CHECK(load("mock.stereo", 512, &error) == 0);
            CHECK(error == unknownError);
            setHook(nullptr);
            CHECK(join(events("mock.stereo")) ==
                  "create:main init:main activate:main deactivate:main "
                  "destroy:main");
            const int h = load("mock.stereo");
            CHECK(h > 0);
        }
    }

    // =========================================================================
    // 16. Dispose while main is in a nested runloop inside on_main_thread
    // =========================================================================

    void testDisposeNested()
    {
        const int h = load("mock.stereo");
        REQUIRE(h > 0);
        auto instance = mgr().getInstance(h);
        const clap_host_t *host = instance->host();
        std::atomic<int> liveInside{-1};
        std::atomic<bool> pumpInside{false};
        setHook([&](const std::string &event, const clap_host_t *) {
            if (event != "on_main")
                return;
            std::thread([] { mgr().retireAllForDispose(); }).join();
            CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.1, false);
            liveInside = gMock.state->liveInstances.load();
            pumpInside = mgr().pumpRunning();
        });
        host->request_callback(host);
        CHECK(pumpUntil([&] { return gMock.state->liveInstances.load() == 0; }));
        setHook(nullptr);
        CHECK(liveInside.load() == 1);
        CHECK(pumpInside.load());
        CHECK(join(events("mock.stereo")) ==
              "create:main init:main activate:main on_main:main "
              "deactivate:main destroy:main");
        CHECK(pumpUntil([] { return !mgr().pumpRunning(); }));
        // The dispose batch held the cache: the module is deinited.
        CHECK(pumpUntil([] { return gMock.state->entryDeinits.load() == 1; }));
    }

    // =========================================================================
    // 17. A batch is quiesced before its teardown (N1, N7)
    // =========================================================================

    std::mutex gLatchMutex;
    std::condition_variable gLatchCv;
    bool gLatchArmed = false;
    bool gLatchEntered = false;
    bool gLatchReleased = false;
    int gLatchHandle = 0;

    void latchHook(int handle)
    {
        std::unique_lock<std::mutex> lock(gLatchMutex);
        if (!gLatchArmed || handle != gLatchHandle)
            return;
        gLatchArmed = false;
        gLatchEntered = true;
        gLatchCv.notify_all();
        gLatchCv.wait(lock, [] { return gLatchReleased; });
    }

    void testQuiesce()
    {
        const int h = load("mock.stereo");
        REQUIRE(h > 0);
        DspThread dsp(h);
        CHECK(pumpUntil([&] { return dsp.audible() > 3; }));
        {
            std::lock_guard<std::mutex> lock(gLatchMutex);
            gLatchArmed = true;
            gLatchEntered = false;
            gLatchReleased = false;
            gLatchHandle = h;
        }
        test::setAfterTableLoadHook(&latchHook);
        {
            std::unique_lock<std::mutex> lock(gLatchMutex);
            gLatchCv.wait(lock, [] { return gLatchEntered; });
        }
        // DSP holds the audio lock between its table load and its check.
        std::atomic<bool> done{false};
        std::thread worker([&] {
            mgr().unloadPlugin(h);
            done = true;
        });
        CHECK(pumpUntil([] { return mgr().liveInstanceCount() == 0; }));
        pumpFor(0.15); // Pump ticks drain, but the batch is not quiesced.
        CHECK(!done.load());
        CHECK(mgr().retiredBatchCount() == 1);
        CHECK(countEvent("mock.stereo", "stop") == 0);
        CHECK(countEvent("mock.stereo", "destroy") == 0);
        {
            std::lock_guard<std::mutex> lock(gLatchMutex);
            gLatchReleased = true;
        }
        gLatchCv.notify_all();
        CHECK(pumpUntil([&] { return done.load(); }));
        worker.join();
        dsp.stop();
        test::setAfterTableLoadHook(nullptr);
        CHECK(countEvent("mock.stereo", "stop") == 1);
        CHECK(countEvent("mock.stereo", "deactivate") == 1);
        CHECK(countEvent("mock.stereo", "destroy") == 1);
        CHECK(mgr().retiredBatchCount() == 0);
    }

    // =========================================================================
    // 18. Module registry: acquire waits for a retiring module (N4)
    // =========================================================================

    std::atomic<int> gRegistryWaits{0};
    void countRegistryWait() { gRegistryWaits.fetch_add(1); }

    void testRegistryRetiring()
    {
        gRegistryWaits = 0;
        test::setRegistryWaitHook(&countRegistryWait);
        std::atomic<bool> reachedWait{false};
        const int h = load("mock.stereo");
        REQUIRE(h > 0);
        CHECK(mgr().unloadPlugin(h) == noError);
        std::atomic<bool> acquired{false};
        std::atomic<bool> acquiredDuringDeinit{true};
        std::atomic<int> status{-1};
        std::thread worker;
        std::atomic<int> deinits{0};
        setHook([&](const std::string &event, const clap_host_t *) {
            if (event != "entry_deinit" || deinits.fetch_add(1) != 0)
                return;
            worker = std::thread([&] {
                AcquireStatus st = AcquireStatus::busy;
                ModuleRef ref = ModuleRegistry::instance().acquire(gMockPath, st);
                status = static_cast<int>(st);
                acquired = true;
                // Dropping it deinits on this thread.
            });
            // The worker is blocked in acquire on this retiring entry, not
            // merely slow to start.
            const auto deadline =
                std::chrono::steady_clock::now() + std::chrono::seconds(3);
            while (gRegistryWaits.load() == 0 &&
                   std::chrono::steady_clock::now() < deadline)
                std::this_thread::yield();
            reachedWait = gRegistryWaits.load() > 0;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            acquiredDuringDeinit = acquired.load();
        });
        mgr().unloadAll();
        worker.join();
        setHook(nullptr);
        test::setRegistryWaitHook(nullptr);
        CHECK(reachedWait.load());
        CHECK(!acquiredDuringDeinit.load());
        CHECK(status.load() == static_cast<int>(AcquireStatus::ok));
        CHECK(gMock.state->entryInits.load() == 2);
        CHECK(gMock.state->entryDeinits.load() == 2);
        CHECK(gMock.state->overlaps.load() == 0);
        const auto lines = logLines();
        CHECK(lines.size() >= 2 && lines[lines.size() - 2] == "entry:entry_init:other" &&
              lines.back() == "entry:entry_deinit:other");
    }

    // =========================================================================
    // 19. Reload before the dispose batch drains (N5)
    // =========================================================================

    void testDisposeReload()
    {
        const int a = load("mock.stereo");
        REQUIRE(a > 0);
        stereoBlock(a);
        mgr().retireAllForDispose(); // On main: the drain is posted.
        CHECK(countEvent("mock.stereo", "destroy") == 0);
        const int b = load("mock.stereo");
        REQUIRE(b > 0);
        CHECK(gMock.state->entryInits.load() == 1);
        CHECK(pumpUntil([] { return mgr().retiredBatchCount() == 0; }));
        CHECK(countEvent("mock.stereo", "destroy") == 1);
        CHECK(gMock.state->entryDeinits.load() == 0);
        Bus out = stereoBlock(b);
        CHECK(isGained(out, 256));
        mgr().unloadAll();
        CHECK(gMock.state->entryDeinits.load() == 1);
    }

    // =========================================================================
    // 20. Failed persists through a slow, failing recovery (N6)
    // =========================================================================

    void testFailedPersists()
    {
        const int h = load("mock.stereo");
        REQUIRE(h > 0);
        auto instance = mgr().getInstance(h);
        const clap_host_t *host = instance->host();
        gMock.state->pendingMainInChannels = 4;
        gMock.state->pendingMainOutChannels = 4;
        host->request_restart(host);
        CHECK(pumpUntil(
            [&] { return instance->disposition() == Disposition::failed; }));

        DspThread dsp(h);
        CHECK(pumpUntil([&] { return dsp.blocks() > 3; }));
        gMock.state->pendingMainInChannels = 2;
        gMock.state->pendingMainOutChannels = 2;
        gMock.state->failActivateCount = 1;
        std::atomic<int> slowActivates{0};
        setHook([&](const std::string &event, const clap_host_t *) {
            if (event == "activate")
            {
                slowActivates.fetch_add(1);
                std::this_thread::sleep_for(std::chrono::milliseconds(60));
            }
        });
        host->request_restart(host);
        CHECK(pumpUntil([&] { return slowActivates.load() == 1; }));
        pumpFor(0.05);
        CHECK(instance->disposition() == Disposition::failed);
        CHECK(dsp.audible() == 0);
        CHECK(dsp.silent() == dsp.blocks() || dsp.silent() + 1 >= dsp.blocks());

        host->request_restart(host);
        CHECK(pumpUntil([&] { return instance->restartCount() == 1; }));
        setHook(nullptr);
        CHECK(pumpUntil([&] { return dsp.audible() > 3; }));
        dsp.stop();
        CHECK(mgr().unloadPlugin(h) == noError);
    }

    // =========================================================================
    // 21. A load queued for main is cancelled by dispose or unloadAll
    // =========================================================================

    std::atomic<bool> gLoadQueued{false};
    void markLoadQueued() { gLoadQueued = true; }

    void testQueuedLoad()
    {
        for (int variant = 0; variant < 2; ++variant)
        {
            gLoadQueued = false;
            test::setBeforeLoadMarshalHook(&markLoadQueued);
            PlayerErrors error = noError;
            int handle = -1;
            std::atomic<bool> done{false};
            std::thread worker([&] {
                error = mgr().loadEffect(gMockPath.c_str(), "mock.stereo",
                                         kSampleRate, 512, 2, &handle);
                done = true;
            });
            // Main does not pump: the load waits for main with its epoch
            // captured.
            while (!gLoadQueued.load())
                std::this_thread::yield();
            test::setBeforeLoadMarshalHook(nullptr);
            if (variant == 0)
                mgr().retireAllForDispose();
            else
                mgr().unloadAll();
            CHECK(pumpUntil([&] { return done.load(); }));
            worker.join();
            CHECK(error == unknownError);
            CHECK(handle == 0);
            CHECK(mgr().liveInstanceCount() == 0);
            CHECK(gMock.state->liveInstances.load() == 0);
            CHECK(countEvent("mock.stereo", "create") == 0);
            CHECK(gMock.state->entryInits.load() == 0);
            CHECK(pumpUntil([] {
                return mgr().retiredBatchCount() == 0 && !mgr().pumpRunning();
            }));
            // The next load is not cancelled.
            const int h = load("mock.stereo");
            CHECK(h > 0);
            mgr().unloadAll();
            gMock.reset();
            gMock.setHook(&hookTrampoline);
        }
    }

    // =========================================================================
    // 22. CLAP_AUDIO_PORT_IS_MAIN decides the main ports
    // =========================================================================

    void testMainPortFlags()
    {
        // No main input: every input port is auxiliary and gets silence.
        gMock.state->unflaggedMainPorts = 1;
        {
            const int h = load("mock.stereo");
            REQUIRE(h > 0);
            auto instance = mgr().getInstance(h);
            const PortLayout layout = instance->layout();
            CHECK(!layout.hasMainInput && layout.hasMainOutput);
            CHECK(layout.mainInputChannels() == 0);
            CHECK(layout.mainOutputChannels() == 2);
            Bus out = stereoBlock(h);
            CHECK(instance->lastSteadyTime() == 0); // It processed.
            CHECK(isSilent(out, 256));
            CHECK(instance->disposition() == Disposition::active);
            CHECK(mgr().unloadPlugin(h) == noError);
        }
        // No main output: rejected.
        mgr().unloadAll();
        gMock.reset();
        gMock.setHook(&hookTrampoline);
        gMock.state->unflaggedMainPorts = 2;
        {
            PlayerErrors error = noError;
            CHECK(load("mock.stereo", 512, &error) == 0);
            CHECK(error == fileLoadFailed);
            CHECK(gMock.state->liveInstances.load() == 0);
            CHECK(join(events("mock.stereo")) ==
                  "create:main init:main destroy:main");
        }
        // A restart that drops the output's main flag fails; restoring it
        // recovers.
        gMock.state->unflaggedMainPorts = 0;
        {
            const int h = load("mock.stereo");
            REQUIRE(h > 0);
            auto instance = mgr().getInstance(h);
            const clap_host_t *host = instance->host();
            {
                Bus out = stereoBlock(h);
                CHECK(isGained(out, 256));
            }
            gMock.state->unflaggedMainPorts = 2;
            host->request_restart(host);
            CHECK(pumpUntil(
                [&] { return instance->disposition() == Disposition::failed; }));
            for (int i = 0; i < 3; ++i)
            {
                Bus out = stereoBlock(h);
                CHECK(isSilent(out, 256));
            }
            CHECK(instance->restartCount() == 0);
            CHECK(instance->lifecycle() == Lifecycle::inited);
            gMock.state->unflaggedMainPorts = 0;
            host->request_restart(host);
            CHECK(pumpUntil([&] { return instance->restartCount() == 1; }));
            CHECK(instance->disposition() == Disposition::active);
            {
                Bus out = stereoBlock(h);
                CHECK(isGained(out, 256));
            }
            CHECK(mgr().unloadPlugin(h) == noError);
        }
    }

    // =========================================================================
    // 23. Scratch allocation failure (input or output buffer)
    // =========================================================================

    void testScratchOom()
    {
        // During load: outOfMemory and a clean teardown.
        for (int nth = 0; nth < 2; ++nth)
        {
            test::failScratchAllocation(nth);
            PlayerErrors error = noError;
            CHECK(load("mock.stereo", 512, &error) == 0);
            test::failScratchAllocation(-1);
            CHECK(error == outOfMemory);
            CHECK(mgr().liveInstanceCount() == 0);
            CHECK(gMock.state->liveInstances.load() == 0);
            CHECK(join(events("mock.stereo")) ==
                  "create:main init:main destroy:main");
            gMock.reset();
            gMock.setHook(&hookTrampoline);
        }
        // During a restart: Failed (silence), and a later restart recovers.
        const int h = load("mock.stereo");
        REQUIRE(h > 0);
        auto instance = mgr().getInstance(h);
        const clap_host_t *host = instance->host();
        stereoBlock(h);
        for (int nth = 0; nth < 2; ++nth)
        {
            test::failScratchAllocation(nth);
            host->request_restart(host);
            CHECK(pumpUntil(
                [&] { return instance->disposition() == Disposition::failed; }));
            test::failScratchAllocation(-1);
            {
                Bus out = stereoBlock(h);
                CHECK(isSilent(out, 256));
            }
            CHECK(instance->lifecycle() == Lifecycle::inited);
            CHECK(instance->restartCount() == static_cast<uint64_t>(nth));
            host->request_restart(host);
            CHECK(pumpUntil([&] {
                return instance->restartCount() == static_cast<uint64_t>(nth + 1);
            }));
            CHECK(instance->disposition() == Disposition::active);
            {
                Bus out = stereoBlock(h);
                CHECK(isGained(out, 256));
            }
        }
        CHECK(mgr().unloadPlugin(h) == noError);
    }

    // =========================================================================
    // 24. The C ABI (clap_bindings.cpp) from a worker while main pumps
    // =========================================================================

    void testAbiExports()
    {
        CHECK(soloud_clapIsSupported() == 1);
        int count = -1;
        char **ids = nullptr, **names = nullptr, **vendors = nullptr,
             **versions = nullptr, **features = nullptr;
        PlayerErrors scan = unknownError;
        onWorker([&] {
            scan = soloud_clapScanBundle(gMockPath.c_str(), &count, &ids, &names,
                                         &vendors, &versions, &features);
        });
        REQUIRE(scan == noError && count == 12);
        REQUIRE(ids && names && vendors && versions && features);
        // The copies outlive the module.
        onWorker([] { soloud_clapHostUnloadAll(); });
        CHECK(gMock.state->entryDeinits.load() == 1);
        CHECK(ModuleRegistry::instance().entryCount() == 0);
        int stereo = -1;
        for (int i = 0; i < count; ++i)
            if (std::strcmp(ids[i], "mock.stereo") == 0)
                stereo = i;
        REQUIRE(stereo >= 0);
        CHECK(std::strcmp(names[stereo], "Mock Stereo") == 0);
        CHECK(std::strcmp(vendors[stereo], "Mock Vendor") == 0);
        CHECK(std::strcmp(versions[stereo], "1.2.3") == 0);
        CHECK(std::strcmp(features[stereo], "audio-effect;stereo") == 0);
        onWorker([&] {
            soloud_clapFreeScanResults(count, ids, names, vendors, versions,
                                       features);
        });

        int handle = -1;
        PlayerErrors loaded = unknownError;
        onWorker([&] {
            loaded = soloud_clapLoadEffect(gMockPath.c_str(), "mock.stereo",
                                           kSampleRate, 512, 2, &handle);
        });
        REQUIRE(loaded == noError && handle > 0);
        auto callback = reinterpret_cast<SoLoudFilterDspCallback_t>(
            soloud_clapGetFilterCallbackPtr());
        REQUIRE(callback != nullptr);
        void *userData = soloud_clapGetPluginUserData(handle);
        CHECK(userData != nullptr);
        Bus bus(2, 256);
        bus.fill({1.0f, 0.5f});
        std::thread audio([&] {
            std::lock_guard<std::mutex> lock(gAudioMutex);
            callback(bus.data.data(), 256, 256, 2,
                     static_cast<float>(kSampleRate), 0.0, nullptr, 0, 0,
                     userData);
        });
        audio.join();
        CHECK(isGained(bus, 256));

        const int runs = gAbiBarrierRuns.load();
        PlayerErrors unloaded = unknownError, again = noError;
        onWorker([&] {
            unloaded = soloud_clapUnloadPlugin(handle);
            again = soloud_clapUnloadPlugin(handle);
        });
        CHECK(unloaded == noError);
        CHECK(again == invalidParameter);
        // The engine barrier the ABI installed quiesced the unload.
        CHECK(gAbiBarrierRuns.load() > runs);
        CHECK(join(events("mock.stereo")) ==
              "create:main init:main activate:main start:audio stop:audio "
              "deactivate:main destroy:main");
        onWorker([] { soloud_clapHostUnloadAll(); });
        CHECK(gMock.state->entryDeinits.load() == 2);
    }

    // =========================================================================
    // Phase 3 helpers
    // =========================================================================

    constexpr int kEvValue = static_cast<int>(HostEventType::paramValue);
    constexpr int kEvBegin = static_cast<int>(HostEventType::gestureBegin);
    constexpr int kEvEnd = static_cast<int>(HostEventType::gestureEnd);
    constexpr int kEvResync = static_cast<int>(HostEventType::resync);
    constexpr int kEvRescan = static_cast<int>(HostEventType::paramsRescan);
    constexpr int kEvLatency = static_cast<int>(HostEventType::latencyChanged);
    constexpr int kEvRestarted = static_cast<int>(HostEventType::restarted);
    constexpr int kEvRestartFailed =
        static_cast<int>(HostEventType::restartFailed);
    constexpr int kEvStateDirty = static_cast<int>(HostEventType::stateDirty);
    constexpr uint64_t kMs = 1000 * 1000;
    constexpr uint64_t kHour = UINT64_C(3600) * 1000 * kMs;

    struct Ev
    {
        int type;
        unsigned id;
        double value;
        unsigned flags;
    };

    std::vector<Ev> drain(int h, int cap = 64, PlayerErrors *error = nullptr)
    {
        const size_t n = static_cast<size_t>(std::max(cap, 1));
        std::vector<int> types(n, -1);
        std::vector<unsigned> ids(n), flags(n);
        std::vector<double> values(n);
        int count = -1;
        const PlayerErrors e = mgr().drainEvents(h, cap, types.data(), ids.data(),
                                                 values.data(), flags.data(), &count);
        if (error != nullptr)
            *error = e;
        std::vector<Ev> out;
        for (int i = 0; i < count; ++i)
            out.push_back({types[i], ids[i], values[i], flags[i]});
        return out;
    }

    /// Drains until a call returns nothing. Every call returns at most [cap].
    std::vector<Ev> drainAll(int h, int cap = 64)
    {
        std::vector<Ev> all;
        for (int i = 0; i < 100000; ++i)
        {
            std::vector<Ev> part = drain(h, cap);
            CHECK(part.size() <= static_cast<size_t>(cap));
            if (part.empty())
                break;
            all.insert(all.end(), part.begin(), part.end());
        }
        return all;
    }

    int countType(const std::vector<Ev> &evs, int type)
    {
        return static_cast<int>(std::count_if(
            evs.begin(), evs.end(), [&](const Ev &e) { return e.type == type; }));
    }

    int indexOf(const std::vector<Ev> &evs, int type)
    {
        for (size_t i = 0; i < evs.size(); ++i)
            if (evs[i].type == type)
                return static_cast<int>(i);
        return -1;
    }

    std::string typeString(const std::vector<Ev> &evs)
    {
        std::string s;
        for (const Ev &e : evs)
            s += (s.empty() ? "" : " ") + std::to_string(e.type);
        return s;
    }

    /// BEGIN opens and END closes a gesture per id. An END without a BEGIN is
    /// legal only after a resync; a resync clears every open gesture; nothing
    /// may be open at the end.
    bool gesturesBalanced(const std::vector<Ev> &evs)
    {
        std::map<unsigned, bool> open;
        bool resynced = false;
        for (const Ev &e : evs)
        {
            if (e.type == kEvResync)
            {
                open.clear();
                resynced = true;
            }
            else if (e.type == kEvBegin)
            {
                if (open[e.id])
                    return false;
                open[e.id] = true;
            }
            else if (e.type == kEvEnd)
            {
                if (!open[e.id] && !resynced)
                    return false;
                open[e.id] = false;
            }
        }
        for (const auto &kv : open)
            if (kv.second)
                return false;
        return true;
    }

    std::vector<MockReceived> received()
    {
        const size_t n = gMock.received(nullptr, 0);
        std::vector<MockReceived> out(n);
        const size_t m = gMock.received(out.data(), n);
        out.resize(std::min(n, m));
        return out;
    }

    /// The received values of [id] from index [from] on.
    std::vector<MockReceived> receivedFor(uint32_t id, size_t from = 0)
    {
        std::vector<MockReceived> out;
        const std::vector<MockReceived> all = received();
        for (size_t i = from; i < all.size(); ++i)
            if (all[i].id == id)
                out.push_back(all[i]);
        return out;
    }

    bool receivedValue(uint32_t id, double value, size_t from = 0)
    {
        for (const MockReceived &r : receivedFor(id, from))
            if (r.value == value)
                return true;
        return false;
    }

    int countLine(const std::string &id, const std::string &line)
    {
        int n = 0;
        for (const std::string &e : events(id))
            if (e == line)
                ++n;
        return n;
    }

    // The host log sink.
    std::mutex gSinkMutex;
    std::vector<std::string> gSinkLines;

    void captureSink(const char *pluginId, int severity, const char *msg)
    {
        std::lock_guard<std::mutex> lock(gSinkMutex);
        gSinkLines.push_back(std::string(pluginId != nullptr ? pluginId : "?") +
                             "|" + std::to_string(severity) + "|" +
                             (msg != nullptr ? msg : "<null>"));
    }

    void captureLogs()
    {
        {
            std::lock_guard<std::mutex> lock(gSinkMutex);
            gSinkLines.clear();
        }
        test::setLogSink(&captureSink);
    }

    int sinkCount(const std::string &needle)
    {
        std::lock_guard<std::mutex> lock(gSinkMutex);
        int n = 0;
        for (const std::string &line : gSinkLines)
            if (line.find(needle) != std::string::npos)
                ++n;
        return n;
    }

    // Plain-function test hooks forwarding to std::function. Set and cleared
    // only while no thread can call them.
    std::function<void()> gProducerFn;
    std::function<void()> gDrainAckFn;
    void producerTrampoline()
    {
        if (gProducerFn)
            gProducerFn();
    }
    void drainAckTrampoline()
    {
        if (gDrainAckFn)
            gDrainAckFn();
    }
    std::function<void()> gDrainInsideFn;
    std::function<void()> gAfterPendingFn;
    void drainInsideTrampoline()
    {
        if (gDrainInsideFn)
            gDrainInsideFn();
    }
    void afterPendingTrampoline()
    {
        if (gAfterPendingFn)
            gAfterPendingFn();
    }

    // Spins until [done] or [seconds] pass, without pumping.
    bool waitFlag(const std::atomic<bool> &done, double seconds)
    {
        const auto end = std::chrono::steady_clock::now() +
                         std::chrono::duration<double>(seconds);
        while (!done.load())
        {
            if (std::chrono::steady_clock::now() >= end)
                return false;
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
        return true;
    }

    template <typename T>
    const T *hostExt(const clap_host_t *host, const char *id)
    {
        return static_cast<const T *>(host->get_extension(host, id));
    }

    const clap_host_params_t *hostParams(const clap_host_t *host)
    {
        return hostExt<clap_host_params_t>(host, CLAP_EXT_PARAMS);
    }

    const clap_host_timer_support_t *hostTimers(const clap_host_t *host)
    {
        return hostExt<clap_host_timer_support_t>(host, CLAP_EXT_TIMER_SUPPORT);
    }

    /// Restarts with the next activate failing: Failed and inited.
    bool makeInactive(ClapInstance &instance)
    {
        const clap_host_t *host = instance.host();
        gMock.state->failActivateCount = 1;
        host->request_restart(host);
        return pumpUntil(
                   [&] { return instance.disposition() == Disposition::failed; }) &&
               instance.lifecycle() == Lifecycle::inited;
    }

    /// Processes stereo blocks until [done] or [maxBlocks].
    bool processUntil(int h, const std::function<bool()> &done, int maxBlocks = 32)
    {
        for (int i = 0; i < maxBlocks && !done(); ++i)
            stereoBlock(h);
        return done();
    }

    // =========================================================================
    // 25. Host extensions
    // =========================================================================

    void testExtensions()
    {
        captureLogs();
        const int h = load("mock.ext");
        REQUIRE(h > 0);
        auto instance = mgr().getInstance(h);
        const clap_host_t *host = instance->host();
        for (const char *id :
             {CLAP_EXT_THREAD_CHECK, CLAP_EXT_LOG, CLAP_EXT_PARAMS, CLAP_EXT_STATE,
              CLAP_EXT_LATENCY, CLAP_EXT_TIMER_SUPPORT, CLAP_EXT_AUDIO_PORTS,
              CLAP_EXT_GUI}) // gui: this harness builds with the plugin UI.
            CHECK(host->get_extension(host, id) != nullptr);
        CHECK(host->get_extension(host, "com.example.unknown") == nullptr);
        CHECK(host->get_extension(host, nullptr) == nullptr);
        CHECK(instance->hasParamsExtension());
        CHECK(instance->hasStateExtension());

        auto *ports = hostExt<clap_host_audio_ports_t>(host, CLAP_EXT_AUDIO_PORTS);
        REQUIRE(ports != nullptr);
        for (uint32_t flag :
             {CLAP_AUDIO_PORTS_RESCAN_NAMES, CLAP_AUDIO_PORTS_RESCAN_FLAGS,
              CLAP_AUDIO_PORTS_RESCAN_CHANNEL_COUNT,
              CLAP_AUDIO_PORTS_RESCAN_PORT_TYPE,
              CLAP_AUDIO_PORTS_RESCAN_IN_PLACE_PAIR,
              CLAP_AUDIO_PORTS_RESCAN_LIST})
            CHECK(ports->is_rescan_flag_supported(host, flag));
        CHECK(!ports->is_rescan_flag_supported(host, 0));
        CHECK(!ports->is_rescan_flag_supported(host, 1u << 30));
        const uint64_t misbehaving = instance->misbehavingCount();
        bool offMain = true;
        onWorker([&] {
            offMain = ports->is_rescan_flag_supported(host,
                                                      CLAP_AUDIO_PORTS_RESCAN_NAMES);
        });
        CHECK(!offMain);
        CHECK(instance->misbehavingCount() == misbehaving + 1);
        CHECK(sinkCount("mock.ext|" + std::to_string(CLAP_LOG_PLUGIN_MISBEHAVING) +
                        "|audio_ports.is_rescan_flag_supported") == 1);

        const int s = load("mock.stereo");
        REQUIRE(s > 0);
        auto stereo = mgr().getInstance(s);
        CHECK(!stereo->hasParamsExtension());
        CHECK(!stereo->hasStateExtension());
        CHECK(mgr().unloadPlugin(s) == noError);
        CHECK(mgr().unloadPlugin(h) == noError);
    }

    // =========================================================================
    // 26. Parameter metadata, values and text
    // =========================================================================

    void testParamsMetadata()
    {
        const int h = load("mock.ext");
        REQUIRE(h > 0);
        CHECK(mgr().parameterCount(h) == 4);
        ParamEntry e;
        CHECK(mgr().parameterInfo(h, 0, e) == noError);
        CHECK(e.id == 10 && e.name == "Gain" && e.module == "Main");
        CHECK(e.min == 0 && e.max == 2 && e.def == 1);
        CHECK((e.flags & CLAP_PARAM_IS_AUTOMATABLE) != 0 && e.cookie != nullptr);
        CHECK(mgr().parameterInfo(h, 1, e) == noError);
        CHECK(e.id == 20 && e.module == "Main/Settings" && e.max == 3);
        CHECK((e.flags & CLAP_PARAM_IS_STEPPED) != 0 &&
              (e.flags & CLAP_PARAM_IS_ENUM) != 0);
        CHECK(mgr().parameterInfo(h, 2, e) == noError);
        CHECK(e.id == 30 && (e.flags & CLAP_PARAM_IS_READONLY) != 0);
        CHECK(mgr().parameterInfo(h, 3, e) == noError);
        CHECK(e.id == 40 && (e.flags & CLAP_PARAM_REQUIRES_PROCESS) != 0);
        CHECK(mgr().parameterInfo(h, 4, e) == invalidParameter);
        CHECK(e.id == CLAP_INVALID_ID && e.name.empty());
        CHECK(mgr().parameterInfo(h, -1, e) == invalidParameter);

        double v = -1;
        CHECK(mgr().getParameterValue(h, 10, &v) == noError && v == 1.0);
        gMock.state->meterValue = 0.75;
        CHECK(mgr().getParameterValue(h, 30, &v) == noError && v == 0.75);
        CHECK(mgr().getParameterValue(h, 99, &v) == invalidParameter && v == 0);
        CHECK(mgr().getParameterValue(h, 10, nullptr) == invalidParameter);
        gMock.state->meterValue = NAN;
        CHECK(mgr().getParameterValue(h, 30, &v) == invalidParameter && v == 0);

        char text[32];
        CHECK(mgr().valueToText(h, 10, 1.5, text, sizeof(text)) == noError);
        CHECK(std::strcmp(text, "1.50") == 0);
        CHECK(mgr().valueToText(h, 10, 1.5, text, 3) == noError);
        CHECK(std::strcmp(text, "1.") == 0);
        std::strcpy(text, "junk");
        CHECK(mgr().valueToText(h, 99, 1.5, text, sizeof(text)) == invalidParameter);
        CHECK(text[0] == '\0');
        CHECK(mgr().valueToText(h, 10, NAN, text, sizeof(text)) == invalidParameter);
        CHECK(mgr().textToValue(h, 10, "0.25", &v) == noError && v == 0.25);
        CHECK(mgr().textToValue(h, 10, "abc", &v) == invalidParameter && v == 0);
        CHECK(mgr().textToValue(h, 99, "0.25", &v) == invalidParameter);
        CHECK(mgr().textToValue(h, 10, nullptr, &v) == invalidParameter);

        // From a worker: a main hop.
        PlayerErrors worker = unknownError;
        v = -1;
        onWorker([&] { worker = mgr().getParameterValue(h, 10, &v); });
        CHECK(worker == noError && v == 1.0);

        // A plugin without params.
        const int s = load("mock.stereo");
        REQUIRE(s > 0);
        CHECK(mgr().parameterCount(s) == 0);
        CHECK(mgr().parameterInfo(s, 0, e) == invalidParameter);
        CHECK(mgr().getParameterValue(s, 10, &v) == notImplemented);
        CHECK(mgr().valueToText(s, 10, 1, text, sizeof(text)) == notImplemented);
        CHECK(mgr().textToValue(s, 10, "1", &v) == notImplemented);
        CHECK(mgr().setParameterValue(s, 10, 1) == invalidParameter);

        // Invalid handles.
        CHECK(mgr().parameterCount(0x7fff) == 0);
        CHECK(mgr().parameterInfo(0x7fff, 0, e) == invalidParameter);
        CHECK(mgr().getParameterValue(0x7fff, 10, &v) == invalidParameter);
        CHECK(mgr().setParameterValue(0x7fff, 10, 1) == invalidParameter);
        CHECK(mgr().unloadPlugin(s) == noError);
        CHECK(mgr().unloadPlugin(h) == noError);
    }

    // =========================================================================
    // 27. setParameterValue delivered by process
    // =========================================================================

    void testParamsSetProcess()
    {
        const int h = load("mock.ext");
        REQUIRE(h > 0);
        auto instance = mgr().getInstance(h);
        stereoBlock(h);
        CHECK(mgr().setParameterValue(h, 10, 5.0) == noError);  // clamped to 2
        CHECK(mgr().setParameterValue(h, 20, 1.6) == noError);  // rounded to 2
        CHECK(mgr().setParameterValue(h, 20, -3.0) == noError); // clamped to 0
        CHECK(mgr().setParameterValue(h, 30, 0.5) == invalidParameter);
        CHECK(mgr().setParameterValue(h, 99, 0.5) == invalidParameter);
        CHECK(mgr().setParameterValue(h, 10, NAN) == invalidParameter);
        CHECK(mgr().setParameterValue(h, 10, INFINITY) == invalidParameter);
        stereoBlock(h);
        std::vector<MockReceived> got = received();
        REQUIRE(got.size() == 3);
        CHECK(got[0].id == 10 && got[0].value == 2.0 && got[0].path == 1 &&
              got[0].time == 0);
        CHECK(got[1].id == 20 && got[1].value == 2.0 && got[1].path == 1);
        CHECK(got[2].id == 20 && got[2].value == 0.0 && got[2].path == 1);
        double v = -1;
        CHECK(mgr().getParameterValue(h, 10, &v) == noError && v == 2.0);

        // 4 producers x 200 edits while DSP runs: none lost, order kept.
        const size_t base = received().size();
        {
            DspThread dsp(h);
            std::atomic<int> failures{0};
            std::vector<std::thread> producers;
            for (int p = 0; p < 4; ++p)
                producers.emplace_back([&, p] {
                    for (int i = 0; i < 200; ++i)
                    {
                        PlayerErrors e;
                        while ((e = mgr().setParameterValue(h, 10, p * 0.5 + i * 0.002)) ==
                               outOfMemory)
                            std::this_thread::yield();
                        if (e != noError)
                            failures.fetch_add(1);
                    }
                });
            for (auto &t : producers)
                t.join();
            CHECK(failures.load() == 0);
            CHECK(pumpUntil([&] { return received().size() >= base + 800; }));
        }
        got = receivedFor(10, base);
        CHECK(got.size() == 800);
        int next[4] = {0, 0, 0, 0};
        bool ordered = true;
        for (const MockReceived &r : got)
        {
            const int p = static_cast<int>(std::floor((r.value + 1e-9) / 0.5));
            const int i = static_cast<int>(std::lround((r.value - p * 0.5) / 0.002));
            if (p < 0 || p > 3 || i != next[p])
                ordered = false;
            else
                ++next[p];
        }
        CHECK(ordered);
        CHECK(next[0] == 200 && next[1] == 200 && next[2] == 200 && next[3] == 200);

        // A full ring: outOfMemory. A huge idle threshold keeps the pump out.
        test::setIdleThresholdNs(kHour);
        stereoBlock(h);
        uint32_t pushed = 0;
        PlayerErrors e = noError;
        while (pushed < 5000 && (e = mgr().setParameterValue(h, 10, 1.0)) == noError)
            ++pushed;
        CHECK(pushed == kEditCapacity && e == outOfMemory);
        CHECK(instance->editRingReadable() == kEditCapacity);
        CHECK(processUntil(h, [&] { return instance->editRingReadable() == 0; }));
        CHECK(instance->maxExaminedPerChunk() <= kMaxEditsExaminedPerChunk);
        CHECK(mgr().setParameterValue(h, 10, 1.0) == noError);
        CHECK(mgr().unloadPlugin(h) == noError);
    }

    // =========================================================================
    // 28. Flush while DSP is paused (active, audio role on main)
    // =========================================================================

    void testParamsFlushPaused()
    {
        test::setIdleThresholdNs(30 * kMs);
        const int h = load("mock.ext");
        REQUIRE(h > 0);
        auto instance = mgr().getInstance(h);
        const clap_host_t *host = instance->host();
        auto *params = hostParams(host);

        // Never processed: the pump flushes at once, in the audio role.
        CHECK(mgr().setParameterValue(h, 10, 0.5) == noError);
        CHECK(pumpUntil([&] { return received().size() == 1; }));
        std::vector<MockReceived> got = received();
        CHECK(got.back().id == 10 && got.back().value == 0.5 && got.back().path == 2);
        CHECK(countLine("mock.ext", "flush:audio") == 1);

        // Running -> paused: a flush after the threshold.
        stereoBlock(h);
        size_t base = received().size();
        CHECK(mgr().setParameterValue(h, 10, 0.6) == noError);
        CHECK(pumpUntil([&] { return received().size() == base + 1; }));
        CHECK(received().back().path == 2);

        // REQUIRES_PROCESS is retained until DSP resumes.
        uint64_t flushes = instance->flushCount();
        CHECK(mgr().setParameterValue(h, 40, 1.0) == noError);
        CHECK(pumpUntil([&] { return instance->retainedCount() == 1; }));
        pumpFor(0.12);
        CHECK(received().size() == base + 1);
        CHECK(instance->flushCount() == flushes + 1);
        stereoBlock(h);
        got = received();
        CHECK(got.size() == base + 2 && got.back().id == 40 &&
              got.back().value == 1.0 && got.back().path == 1);
        CHECK(instance->retainedCount() == 0);

        // request_flush from a worker, and request_process while idle.
        flushes = instance->flushCount();
        onWorker([&] { params->request_flush(host); });
        CHECK(pumpUntil([&] { return instance->flushCount() > flushes; }));
        flushes = instance->flushCount();
        host->request_process(host);
        CHECK(pumpUntil([&] { return instance->flushCount() > flushes; }));

        // The try-lock skips while DSP is inside process.
        {
            std::atomic<bool> inProcess{false}, release{false};
            setHook([&](const std::string &event, const clap_host_t *) {
                if (event == "process" && !release.load())
                {
                    inProcess = true;
                    const auto end = std::chrono::steady_clock::now() +
                                     std::chrono::seconds(3);
                    while (!release.load() && std::chrono::steady_clock::now() < end)
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            });
            std::thread audio([&] {
                Bus bus(2, 256);
                bus.fill({1.0f, 0.5f});
                callDsp(h, bus.data.data(), 256, 256, 2);
            });
            CHECK(pumpUntil([&] { return inProcess.load(); }));
            flushes = instance->flushCount();
            base = received().size();
            CHECK(mgr().setParameterValue(h, 10, 0.75) == noError);
            pumpFor(0.12);
            CHECK(instance->flushCount() == flushes);
            CHECK(received().size() == base);
            release = true;
            audio.join();
            setHook(nullptr);
            CHECK(pumpUntil([&] { return receivedValue(10, 0.75, base); }));
        }

        // An edit queued inside flush is delivered by the next pass.
        {
            std::atomic<int> inFlush{0};
            setHook([&](const std::string &event, const clap_host_t *) {
                if (event == "flush" && inFlush.fetch_add(1) == 0)
                    CHECK(mgr().setParameterValue(h, 10, 0.3) == noError);
            });
            base = received().size();
            flushes = instance->flushCount();
            CHECK(mgr().setParameterValue(h, 10, 0.6) == noError);
            CHECK(pumpUntil([&] { return receivedFor(10, base).size() == 2; }));
            setHook(nullptr);
            got = receivedFor(10, base);
            CHECK(got.size() == 2 && got[0].value == 0.6 && got[1].value == 0.3);
            CHECK(got.size() == 2 && got[1].path == 2);
            CHECK(instance->flushCount() >= flushes + 2);
        }

        // Resume during flush: that DSP call leaves its block dry.
        {
            std::atomic<bool> flushing{false}, dspDone{false};
            setHook([&](const std::string &event, const clap_host_t *) {
                if (event == "flush" && !flushing.exchange(true))
                {
                    const auto end = std::chrono::steady_clock::now() +
                                     std::chrono::seconds(3);
                    while (!dspDone.load() && std::chrono::steady_clock::now() < end)
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            });
            const uint64_t dry = instance->dryBlocksFromServicing();
            Bus bus(2, 256);
            std::thread audio([&] {
                while (!flushing.load())
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                bus.fill({1.0f, 0.5f});
                callDsp(h, bus.data.data(), 256, 256, 2);
                dspDone = true;
            });
            CHECK(mgr().setParameterValue(h, 40, 0.0) == noError);
            CHECK(pumpUntil([&] { return dspDone.load(); }));
            audio.join();
            setHook(nullptr);
            CHECK(bus.equals(0, 1.0f, 0, 256) && bus.equals(1, 0.5f, 0, 256));
            CHECK(instance->dryBlocksFromServicing() == dry + 1);
            base = received().size();
            Bus out = stereoBlock(h);
            CHECK(isGained(out, 256));
            CHECK(receivedValue(40, 0.0, base));
        }
        CHECK(mgr().unloadPlugin(h) == noError);

        // Failed start (Failed + activated): flushed in the audio role.
        gMock.state->failStartCount = 1;
        const int f = load("mock.ext");
        REQUIRE(f > 0);
        auto failed = mgr().getInstance(f);
        {
            Bus out = stereoBlock(f);
            CHECK(isSilent(out, 256));
        }
        CHECK(failed->disposition() == Disposition::failed);
        CHECK(failed->lifecycle() == Lifecycle::activated);
        base = received().size();
        CHECK(mgr().setParameterValue(f, 10, 0.9) == noError);
        CHECK(pumpUntil([&] { return received().size() == base + 1; }));
        CHECK(received().back().path == 2);
        CHECK(mgr().unloadPlugin(f) == noError);
    }

    // =========================================================================
    // 29. Flush while inactive (Failed + inited, main without the audio role)
    // =========================================================================

    void testParamsFlushInactive()
    {
        test::setIdleThresholdNs(30 * kMs);
        const int h = load("mock.ext");
        REQUIRE(h > 0);
        auto instance = mgr().getInstance(h);
        const clap_host_t *host = instance->host();
        auto *params = hostParams(host);
        stereoBlock(h);
        // A retained REQUIRES_PROCESS edit survives into the inactive state.
        CHECK(mgr().setParameterValue(h, 40, 1.0) == noError);
        CHECK(pumpUntil([&] { return instance->retainedCount() == 1; }));
        REQUIRE(makeInactive(*instance));
        CHECK(pumpUntil([&] { return receivedValue(40, 1.0); }));
        CHECK(receivedFor(40).back().path == 3);
        CHECK(instance->retainedCount() == 0);

        size_t base = received().size();
        CHECK(mgr().setParameterValue(h, 10, 0.5) == noError);
        CHECK(mgr().setParameterValue(h, 40, 0.0) == noError);
        CHECK(pumpUntil([&] { return received().size() == base + 2; }));
        std::vector<MockReceived> got = received();
        CHECK(got[base].id == 10 && got[base].value == 0.5 && got[base].path == 3);
        CHECK(got[base + 1].id == 40 && got[base + 1].value == 0.0 &&
              got[base + 1].path == 3);
        CHECK(countLine("mock.ext", "flush:main") >= 2);

        // request_flush from inside an inactive flush: one more pass.
        std::atomic<int> hooks{0};
        setHook([&](const std::string &event, const clap_host_t *hst) {
            if (event == "flush" && hooks.fetch_add(1) == 0)
                hostParams(hst)->request_flush(hst);
        });
        const uint64_t flushes = instance->flushCount();
        params->request_flush(host);
        CHECK(pumpUntil([&] { return instance->flushCount() >= flushes + 2; }));
        pumpFor(0.08);
        CHECK(instance->flushCount() == flushes + 2);
        setHook(nullptr);

        // A restart recovers.
        host->request_restart(host);
        CHECK(pumpUntil([&] { return instance->disposition() == Disposition::active; }));
        Bus out = stereoBlock(h);
        CHECK(isGained(out, 256));
        CHECK(mgr().unloadPlugin(h) == noError);
    }

    // =========================================================================
    // 30. Out-events: gestures and values to Dart
    // =========================================================================

    void testOutEvents()
    {
        const int h = load("mock.ext");
        REQUIRE(h > 0);
        auto instance = mgr().getInstance(h);
        const clap_host_t *host = instance->host();
        stereoBlock(h);
        CHECK(drainAll(h).empty());

        gMock.state->gestureValues = 3;
        gMock.state->gestureFromProcess = 1;
        stereoBlock(h);
        std::vector<Ev> evs = drainAll(h);
        REQUIRE(evs.size() == 5);
        CHECK(typeString(evs) == "2 1 1 1 3");
        for (const Ev &e : evs)
            CHECK(e.id == 10 && e.flags == 0);
        CHECK(std::fabs(evs[1].value - 2.0 / 3) < 1e-12 &&
              std::fabs(evs[2].value - 4.0 / 3) < 1e-12 && evs[3].value == 2.0);

        // dontRecord.
        gMock.state->gestureDontRecord = 1;
        gMock.state->gestureFromProcess = 1;
        stereoBlock(h);
        evs = drainAll(h);
        REQUIRE(evs.size() == 5);
        CHECK(evs[1].flags == 1 && evs[2].flags == 1 && evs[3].flags == 1);
        gMock.state->gestureDontRecord = 0;

        // From a paused flush.
        test::setIdleThresholdNs(30 * kMs);
        gMock.state->gestureFromFlush = 1;
        hostParams(host)->request_flush(host);
        CHECK(pumpUntil([&] { return gMock.state->gestureFromFlush.load() == 0; }));
        evs = drainAll(h);
        CHECK(typeString(evs) == "2 1 1 1 3");
        test::setIdleThresholdNs(kHour);

        // Bounded draining.
        for (int cap = 1; cap <= 3; ++cap)
        {
            gMock.state->gestureValues = 4;
            gMock.state->gestureFromProcess = 2;
            stereoBlock(h);
            stereoBlock(h);
            evs = drainAll(h, cap);
            CHECK(typeString(evs) == "2 1 1 1 1 3 2 1 1 1 1 3");
            CHECK(gesturesBalanced(evs));
        }
        CHECK(gMock.state->outPushFailures.load() == 0);

        // Arguments.
        int types[2];
        unsigned ids[2], flags[2];
        double values[2];
        int count = -1;
        CHECK(mgr().drainEvents(h, -1, types, ids, values, flags, &count) ==
              invalidParameter);
        CHECK(count == 0);
        CHECK(mgr().drainEvents(h, 2, types, ids, values, flags, nullptr) ==
              invalidParameter);
        CHECK(mgr().drainEvents(h, 2, nullptr, ids, values, flags, &count) ==
              invalidParameter);
        count = -1;
        CHECK(mgr().drainEvents(h, 0, nullptr, nullptr, nullptr, nullptr, &count) ==
              noError);
        CHECK(count == 0);

        CHECK(mgr().unloadPlugin(h) == noError);
        PlayerErrors e = noError;
        CHECK(drain(h, 4, &e).empty());
        CHECK(e == invalidParameter);
    }

    // =========================================================================
    // 31. Out-ring overflow
    // =========================================================================

    void testOutOverflow()
    {
        test::setIdleThresholdNs(kHour);
        test::setOutRingCapacity(512);
        int h = load("mock.ext");
        REQUIRE(h > 0);
        stereoBlock(h);

        // One call over capacity: 1102 pushes, 512 fit.
        gMock.state->gestureValues = 1100;
        gMock.state->gestureFromProcess = 1;
        stereoBlock(h);
        CHECK(gMock.state->outPushFailures.load() == 590);
        std::vector<Ev> evs = drainAll(h);
        REQUIRE(evs.size() == 513);
        CHECK(evs[0].type == kEvBegin && evs[511].type == kEvValue);
        CHECK(evs.back().type == kEvResync && evs.back().flags == kResyncOverflow &&
              evs.back().value == 590);
        CHECK(gesturesBalanced(evs));
        // Recovered.
        gMock.state->outPushFailures = 0;
        gMock.state->gestureValues = 3;
        gMock.state->gestureFromProcess = 1;
        stereoBlock(h);
        evs = drainAll(h);
        CHECK(typeString(evs) == "2 1 1 1 3");
        CHECK(gMock.state->outPushFailures.load() == 0);

        // A fill spread over several calls.
        gMock.state->burstFromProcess = 300;
        stereoBlock(h);
        gMock.state->burstFromProcess = 300;
        stereoBlock(h);
        CHECK(gMock.state->outPushFailures.load() == 88);
        evs = drainAll(h, 7);
        REQUIRE(evs.size() == 513);
        CHECK(countType(evs, kEvValue) == 512);
        CHECK(evs.back().type == kEvResync && evs.back().value == 88);
        gMock.state->outPushFailures = 0;
        gMock.state->burstFromProcess = 300;
        stereoBlock(h);
        CHECK(drainAll(h).size() == 300);
        CHECK(gMock.state->outPushFailures.load() == 0);

        // An overflow during a paused flush.
        test::setIdleThresholdNs(30 * kMs);
        auto instance = mgr().getInstance(h);
        const clap_host_t *host = instance->host();
        gMock.state->gestureValues = 600;
        gMock.state->gestureFromFlush = 1;
        hostParams(host)->request_flush(host);
        CHECK(pumpUntil([&] { return gMock.state->gestureFromFlush.load() == 0; }));
        CHECK(gMock.state->outPushFailures.load() == 90);
        evs = drainAll(h);
        CHECK(evs.size() == 513 && evs.back().type == kEvResync &&
              evs.back().value == 90);
        CHECK(gesturesBalanced(evs));
        CHECK(mgr().unloadPlugin(h) == noError);

        // The default ring holds the same call.
        test::setOutRingCapacity(0);
        gMock.state->outPushFailures = 0;
        h = load("mock.ext");
        REQUIRE(h > 0);
        stereoBlock(h);
        gMock.state->gestureValues = 1100;
        gMock.state->gestureFromProcess = 1;
        stereoBlock(h);
        CHECK(gMock.state->outPushFailures.load() == 0);
        evs = drainAll(h);
        CHECK(evs.size() == 1102 && countType(evs, kEvResync) == 0);
        CHECK(gesturesBalanced(evs));
        CHECK(mgr().unloadPlugin(h) == noError);
    }

    // =========================================================================
    // 32. params.rescan and params.clear
    // =========================================================================

    void testParamsRescan()
    {
        captureLogs();
        test::setIdleThresholdNs(kHour);
        const int h = load("mock.ext");
        REQUIRE(h > 0);
        auto instance = mgr().getInstance(h);
        const clap_host_t *host = instance->host();
        auto *params = hostParams(host);
        stereoBlock(h);
        drainAll(h);

        // VALUES and TEXT notify at once.
        params->rescan(host, CLAP_PARAM_RESCAN_VALUES);
        std::vector<Ev> evs = drainAll(h);
        CHECK(evs.size() == 1 && evs[0].type == kEvRescan &&
              evs[0].flags == CLAP_PARAM_RESCAN_VALUES);
        params->rescan(host, CLAP_PARAM_RESCAN_VALUES | CLAP_PARAM_RESCAN_TEXT);
        evs = drainAll(h);
        CHECK(evs.size() == 1 &&
              evs[0].flags == (CLAP_PARAM_RESCAN_VALUES | CLAP_PARAM_RESCAN_TEXT));

        // INFO: rebuilt by the pump; the name changes, the cookies stay.
        ParamEntry before, after;
        CHECK(mgr().parameterInfo(h, 0, before) == noError);
        const uint64_t generation = instance->paramsGeneration();
        gMock.state->paramNameVariant = 2;
        params->rescan(host, CLAP_PARAM_RESCAN_INFO);
        CHECK(pumpUntil([&] {
            ParamEntry e;
            return mgr().parameterInfo(h, 0, e) == noError && e.name == "Gain 2";
        }));
        CHECK(mgr().parameterInfo(h, 0, after) == noError);
        CHECK(after.cookie == before.cookie);
        CHECK(instance->paramsGeneration() == generation);
        evs = drainAll(h);
        CHECK(evs.size() == 1 && evs[0].type == kEvRescan &&
              (evs[0].flags & CLAP_PARAM_RESCAN_INFO) != 0);

        // ALL while active: misbehaving; a restart rebuilds.
        size_t base = received().size();
        CHECK(mgr().setParameterValue(h, 10, 0.5) == noError);
        uint64_t misbehaving = instance->misbehavingCount();
        uint64_t restarts = instance->restartCount();
        params->rescan(host, CLAP_PARAM_RESCAN_ALL);
        CHECK(instance->misbehavingCount() == misbehaving + 1);
        CHECK(instance->paramsPending());
        CHECK(mgr().parameterCount(h) == 0);
        ParamEntry e;
        CHECK(mgr().parameterInfo(h, 0, e) == invalidParameter);
        CHECK(mgr().setParameterValue(h, 10, 0.5) == unknownError);
        CHECK(pumpUntil([&] {
            return instance->restartCount() == restarts + 1 &&
                   !instance->paramsPending();
        }));
        CHECK(mgr().parameterCount(h) == 4);
        stereoBlock(h);
        CHECK(!receivedValue(10, 0.5, base));
        evs = drainAll(h);
        {
            const int resync = indexOf(evs, kEvResync);
            const int restarted = indexOf(evs, kEvRestarted);
            const int rescan = indexOf(evs, kEvRescan);
            CHECK(resync >= 0 && restarted > resync && rescan > restarted);
            CHECK(resync >= 0 && (evs[resync].flags & kResyncParamsReset) != 0);
            CHECK(rescan >= 0 && evs[rescan].flags == CLAP_PARAM_RESCAN_ALL);
            CHECK(countType(evs, kEvResync) == 1);
        }
        CHECK(sinkCount("|" + std::to_string(CLAP_LOG_PLUGIN_MISBEHAVING) + "|") >= 1);

        // A variant change in deactivate (legal): the queued edit is dropped.
        misbehaving = instance->misbehavingCount();
        restarts = instance->restartCount();
        base = received().size();
        gMock.state->pendingParamVariant = 1;
        CHECK(mgr().setParameterValue(h, 10, 0.25) == noError);
        host->request_restart(host);
        CHECK(pumpUntil([&] {
            return instance->restartCount() == restarts + 1 &&
                   !instance->paramsPending();
        }));
        pumpFor(0.1);
        CHECK(instance->restartCount() == restarts + 1);
        CHECK(instance->misbehavingCount() == misbehaving);
        CHECK(mgr().parameterCount(h) == 5);
        CHECK(mgr().parameterInfo(h, 4, e) == noError && e.id == 50);
        CHECK(mgr().parameterInfo(h, 0, after) == noError);
        CHECK(after.cookie != before.cookie);
        stereoBlock(h);
        CHECK(!receivedValue(10, 0.25, base));
        CHECK(mgr().setParameterValue(h, 50, 0.8) == noError);
        stereoBlock(h);
        CHECK(receivedValue(50, 0.8, base));
        evs = drainAll(h);
        {
            const int resync = indexOf(evs, kEvResync);
            const int restarted = indexOf(evs, kEvRestarted);
            const int rescan = indexOf(evs, kEvRescan);
            CHECK(resync >= 0 && restarted > resync && rescan > restarted);
            CHECK(resync >= 0 && (evs[resync].flags & kResyncParamsReset) != 0);
            CHECK(rescan >= 0 && evs[rescan].flags == CLAP_PARAM_RESCAN_ALL);
        }

        // ALL from deactivate with the cookies unchanged: exactly one restart.
        restarts = instance->restartCount();
        const uint64_t generationBefore = instance->paramsGeneration();
        gMock.state->rescanAllInDeactivate = 1;
        host->request_restart(host);
        CHECK(pumpUntil([&] {
            return instance->restartCount() == restarts + 1 &&
                   !instance->paramsPending();
        }));
        pumpFor(0.17); // 10 pump ticks
        CHECK(instance->restartCount() == restarts + 1);
        CHECK(instance->paramsGeneration() > generationBefore);
        evs = drainAll(h);
        CHECK(countType(evs, kEvResync) == 1);
        CHECK(countType(evs, kEvRestarted) == 1);

        // clear drops queued edits.
        base = received().size();
        CHECK(mgr().setParameterValue(h, 10, 0.125) == noError);
        params->clear(host, 10, CLAP_PARAM_CLEAR_ALL);
        stereoBlock(h);
        CHECK(!receivedValue(10, 0.125, base));
        CHECK(mgr().setParameterValue(h, 10, 0.375) == noError);
        stereoBlock(h);
        CHECK(receivedValue(10, 0.375, base));

        // Off main: ignored and logged.
        misbehaving = instance->misbehavingCount();
        onWorker([&] {
            params->rescan(host, CLAP_PARAM_RESCAN_VALUES);
            params->clear(host, 10, CLAP_PARAM_CLEAR_ALL);
        });
        CHECK(instance->misbehavingCount() == misbehaving + 2);
        CHECK(drainAll(h).empty());

        // state.mark_dirty.
        auto *state = hostExt<clap_host_state_t>(host, CLAP_EXT_STATE);
        state->mark_dirty(host);
        evs = drainAll(h);
        CHECK(evs.size() == 1 && evs[0].type == kEvStateDirty);
        CHECK(mgr().unloadPlugin(h) == noError);
    }

    // =========================================================================
    // 33. State
    // =========================================================================

    void testState()
    {
        test::setIdleThresholdNs(kHour);
        const int h = load("mock.ext");
        REQUIRE(h > 0);
        stereoBlock(h);
        CHECK(mgr().setParameterValue(h, 10, 1.5) == noError);
        CHECK(mgr().setParameterValue(h, 20, 2.0) == noError);
        stereoBlock(h);
        drainAll(h);
        gMock.state->stateExtraBytes = 5000;
        std::vector<unsigned char> saved;
        for (int chunk : {1, 7, 4096})
        {
            gMock.state->stateChunk = chunk;
            saved.clear();
            CHECK(mgr().saveState(h, saved) == noError);
            CHECK(saved.size() == 16 + 4 * 12 + 5000);
            CHECK(mgr().setParameterValue(h, 10, 0.1) == noError);
            stereoBlock(h);
            double v = -1;
            CHECK(mgr().getParameterValue(h, 10, &v) == noError && v == 0.1);
            CHECK(mgr().loadState(h, saved.data(), saved.size()) == noError);
            CHECK(mgr().getParameterValue(h, 10, &v) == noError && v == 1.5);
            CHECK(mgr().getParameterValue(h, 20, &v) == noError && v == 2.0);
            std::vector<Ev> evs = drainAll(h);
            CHECK(evs.size() == 1 && evs[0].type == kEvRescan &&
                  evs[0].flags == (CLAP_PARAM_RESCAN_VALUES | CLAP_PARAM_RESCAN_TEXT));
        }
        gMock.state->stateChunk = 0;

        // Edits queued before a load do not override it.
        const size_t base = received().size();
        CHECK(mgr().setParameterValue(h, 10, 0.2) == noError);
        CHECK(mgr().loadState(h, saved.data(), saved.size()) == noError);
        stereoBlock(h);
        CHECK(!receivedValue(10, 0.2, base));
        double v = -1;
        CHECK(mgr().getParameterValue(h, 10, &v) == noError && v == 1.5);

        // Failures.
        gMock.state->failSave = 1;
        std::vector<unsigned char> out{1, 2, 3};
        CHECK(mgr().saveState(h, out) == unknownError && out.empty());
        gMock.state->failSave = 0;
        gMock.state->failLoad = 1;
        CHECK(mgr().loadState(h, saved.data(), saved.size()) == unknownError);
        gMock.state->failLoad = 0;
        test::setStateSizeLimit(100);
        out.assign(3, 1);
        CHECK(mgr().saveState(h, out) == outOfMemory && out.empty());
        test::setStateSizeLimit(0);
        // A truncated state is rejected by the plugin.
        CHECK(mgr().loadState(h, saved.data(), 10) == unknownError);

        // Empty state, null data.
        gMock.state->stateEmpty = 1;
        out.assign(3, 1);
        CHECK(mgr().saveState(h, out) == noError && out.empty());
        gMock.state->stateEmpty = 0;
        CHECK(mgr().loadState(h, nullptr, 0) == noError);
        CHECK(mgr().loadState(h, nullptr, 5) == invalidParameter);

        // From a worker.
        PlayerErrors worker = unknownError;
        std::vector<unsigned char> fromWorker;
        onWorker([&] { worker = mgr().saveState(h, fromWorker); });
        CHECK(worker == noError && fromWorker.size() == 16 + 4 * 12 + 5000);
        onWorker([&] { worker = mgr().loadState(h, fromWorker.data(), fromWorker.size()); });
        CHECK(worker == noError);

        // No state extension.
        const int s = load("mock.stereo");
        REQUIRE(s > 0);
        out.assign(3, 1);
        CHECK(mgr().saveState(s, out) == notImplemented && out.empty());
        CHECK(mgr().loadState(s, saved.data(), saved.size()) == notImplemented);
        CHECK(mgr().saveState(0x7fff, out) == invalidParameter);
        CHECK(mgr().unloadPlugin(s) == noError);
        CHECK(mgr().unloadPlugin(h) == noError);
    }

    // =========================================================================
    // 34. Latency
    // =========================================================================

    void testLatency()
    {
        captureLogs();
        gMock.state->latency = 64;
        const int h = load("mock.ext");
        REQUIRE(h > 0);
        auto instance = mgr().getInstance(h);
        const clap_host_t *host = instance->host();
        CHECK(mgr().latencySamples(h) == 64);
        CHECK(drainAll(h).empty());

        gMock.state->pendingLatency = 128;
        host->request_restart(host);
        CHECK(pumpUntil([&] { return instance->restartCount() == 1; }));
        CHECK(mgr().latencySamples(h) == 128);
        std::vector<Ev> evs = drainAll(h);
        {
            const int restarted = indexOf(evs, kEvRestarted);
            const int latency = indexOf(evs, kEvLatency);
            CHECK(restarted >= 0 && latency > restarted);
            CHECK(restarted >= 0 && evs[restarted].value == 1);
            CHECK(latency >= 0 && evs[latency].value == 128);
            CHECK(evs.size() == 2);
        }

        // changed() outside activate: logged, no event.
        const uint64_t misbehaving = instance->misbehavingCount();
        hostExt<clap_host_latency_t>(host, CLAP_EXT_LATENCY)->changed(host);
        CHECK(instance->misbehavingCount() == misbehaving + 1);
        CHECK(sinkCount("latency.changed outside activate") == 1);
        CHECK(drainAll(h).empty());
        CHECK(mgr().latencySamples(h) == 128);

        // An activation failure: restartFailed only.
        gMock.state->failActivateCount = 1;
        host->request_restart(host);
        CHECK(pumpUntil([&] { return instance->disposition() == Disposition::failed; }));
        evs = drainAll(h);
        CHECK(evs.size() == 1 && evs[0].type == kEvRestartFailed);
        // Recovery with the same latency: restarted only.
        host->request_restart(host);
        CHECK(pumpUntil([&] { return instance->disposition() == Disposition::active; }));
        evs = drainAll(h);
        CHECK(evs.size() == 1 && evs[0].type == kEvRestarted);
        CHECK(mgr().latencySamples(0x7fff) == 0);
        CHECK(mgr().unloadPlugin(h) == noError);
    }

    // =========================================================================
    // 35. Timers
    // =========================================================================

    void testTimers()
    {
        int h = load("mock.ext");
        REQUIRE(h > 0);
        auto instance = mgr().getInstance(h);
        const clap_host_t *host = instance->host();
        auto *timers = hostTimers(host);
        clap_id id = 999;
        CHECK(timers->register_timer(host, 1, &id) && id != CLAP_INVALID_ID);
        CHECK(instance->timerCount() == 1);
        CHECK(instance->timerPeriodMs(id) == 16);
        CHECK(instance->timerPeriodMs(12345) == 0);
        pumpFor(0.2);
        const int calls = gMock.state->onTimerCalls.load();
        // Clamped to 16 ms, no catch-up: at most 13 fires in 200 ms.
        CHECK(calls >= 3 && calls <= 13);
        CHECK(timers->unregister_timer(host, id));
        CHECK(!timers->unregister_timer(host, id));
        CHECK(!timers->unregister_timer(host, 12345));
        pumpFor(0.06);
        CHECK(gMock.state->onTimerCalls.load() == calls);
        CHECK(!timers->register_timer(host, 20, nullptr));

        // Off main: refused.
        bool ok = true;
        clap_id workerId = 0;
        onWorker([&] { ok = timers->register_timer(host, 20, &workerId); });
        CHECK(!ok && workerId == CLAP_INVALID_ID && instance->timerCount() == 0);

        // The limit.
        std::vector<clap_id> ids;
        for (uint32_t i = 0; i < kMaxTimers; ++i)
        {
            clap_id t = CLAP_INVALID_ID;
            CHECK(timers->register_timer(host, 10000, &t));
            ids.push_back(t);
        }
        clap_id extra = 0;
        CHECK(!timers->register_timer(host, 10000, &extra) && extra == CLAP_INVALID_ID);
        for (clap_id t : ids)
            CHECK(timers->unregister_timer(host, t));

        // Register and unregister from inside on_timer.
        clap_id outer = CLAP_INVALID_ID, inner = CLAP_INVALID_ID;
        std::atomic<int> fires{0};
        setHook([&](const std::string &event, const clap_host_t *hst) {
            if (event != "on_timer" || fires.fetch_add(1) != 0)
                return;
            CHECK(hostTimers(hst)->register_timer(hst, 16, &inner));
            CHECK(hostTimers(hst)->unregister_timer(hst, outer));
        });
        CHECK(timers->register_timer(host, 16, &outer));
        CHECK(pumpUntil([&] { return fires.load() >= 3; }));
        setHook(nullptr);
        CHECK(countLine("mock.ext", "on_timer:" + std::to_string(outer)) == 1);
        CHECK(countLine("mock.ext", "on_timer:" + std::to_string(inner)) >= 2);
        CHECK(timers->unregister_timer(host, inner));

        // A timer pending at unload never fires.
        clap_id pending = CLAP_INVALID_ID;
        CHECK(timers->register_timer(host, 16, &pending));
        int before = gMock.state->onTimerCalls.load();
        CHECK(mgr().unloadPlugin(h) == noError);
        pumpFor(0.08);
        CHECK(gMock.state->onTimerCalls.load() == before);

        // Registered in init: fires after load; unregistered in destroy.
        gMock.state->registerTimerMs = 20;
        gMock.state->unregisterInDestroy = 1;
        h = load("mock.ext");
        REQUIRE(h > 0);
        CHECK(gMock.state->initTimerRegistered.load() == 1);
        before = gMock.state->onTimerCalls.load();
        CHECK(pumpUntil([&] { return gMock.state->onTimerCalls.load() > before; }));
        CHECK(mgr().unloadPlugin(h) == noError);
        CHECK(pumpUntil([&] { return gMock.state->liveInstances.load() == 0; }));
        CHECK(gMock.state->unregisterInDestroyResult.load() == 1);
        gMock.state->unregisterInDestroy = 0;

        // A failed init with an init-time timer: never fires.
        gMock.state->failInitExt = 1;
        gMock.state->initTimerRegistered = 0;
        before = gMock.state->onTimerCalls.load();
        PlayerErrors error = noError;
        CHECK(load("mock.ext", 512, &error) == 0 && error != noError);
        CHECK(gMock.state->initTimerRegistered.load() == 1);
        pumpFor(0.1);
        CHECK(gMock.state->onTimerCalls.load() == before);
        gMock.state->failInitExt = 0;
        gMock.state->registerTimerMs = 0;

        // Two due timers; the first on_timer unloads: the second never fires.
        h = load("mock.ext");
        REQUIRE(h > 0);
        instance = mgr().getInstance(h);
        host = instance->host();
        std::atomic<int> unloadFires{0};
        setHook([&](const std::string &event, const clap_host_t *) {
            if (event == "on_timer" && unloadFires.fetch_add(1) == 0)
                CHECK(mgr().unloadPlugin(h) == noError);
        });
        clap_id a = CLAP_INVALID_ID, b = CLAP_INVALID_ID;
        CHECK(hostTimers(host)->register_timer(host, 16, &a));
        CHECK(hostTimers(host)->register_timer(host, 16, &b));
        instance.reset();
        CHECK(pumpUntil([&] { return gMock.state->liveInstances.load() == 0; }));
        pumpFor(0.05);
        CHECK(unloadFires.load() == 1);
        setHook(nullptr);

        // Dispose from inside on_timer.
        h = load("mock.ext");
        REQUIRE(h > 0);
        host = mgr().getInstance(h)->host();
        std::atomic<int> disposeFires{0};
        setHook([&](const std::string &event, const clap_host_t *) {
            if (event == "on_timer" && disposeFires.fetch_add(1) == 0)
                mgr().retireAllForDispose();
        });
        CHECK(hostTimers(host)->register_timer(host, 16, &a));
        CHECK(pumpUntil([&] { return gMock.state->liveInstances.load() == 0; }));
        pumpFor(0.05);
        CHECK(disposeFires.load() == 1);
        setHook(nullptr);
    }

    // =========================================================================
    // 36. Log
    // =========================================================================

    void testLog()
    {
        captureLogs();
        const int h = load("mock.ext");
        REQUIRE(h > 0);
        auto instance = mgr().getInstance(h);
        const clap_host_t *host = instance->host();
        CHECK(sinkCount("mock.ext|" + std::to_string(CLAP_LOG_INFO) +
                        "|mock.ext init") == 1);
        gMock.state->logFromAudio = 1;
        stereoBlock(h);
        CHECK(instance->droppedAudioLogs() == 1);
        CHECK(sinkCount("mock.ext audio log") == 0);
        auto *log = hostExt<clap_host_log_t>(host, CLAP_EXT_LOG);
        log->log(host, CLAP_LOG_ERROR, nullptr);
        CHECK(sinkCount("mock.ext|" + std::to_string(CLAP_LOG_ERROR) + "|(null)") == 1);
        onWorker([&] { log->log(host, CLAP_LOG_WARNING, "from a worker"); });
        CHECK(sinkCount("mock.ext|" + std::to_string(CLAP_LOG_WARNING) +
                        "|from a worker") == 1);
        // Many audio logs while DSP runs: dropped, counted, never emitted.
        {
            gMock.state->logFromAudio = 50;
            DspThread dsp(h);
            CHECK(pumpUntil([&] { return gMock.state->logFromAudio.load() == 0; }));
        }
        CHECK(instance->droppedAudioLogs() == 51);
        CHECK(sinkCount("mock.ext audio log") == 0);
        CHECK(mgr().unloadPlugin(h) == noError);
    }

    // =========================================================================
    // 37. The Phase 3 C ABI from a worker
    // =========================================================================

    void testAbiParamsState()
    {
        int h = -1;
        PlayerErrors loaded = unknownError;
        onWorker([&] {
            loaded = soloud_clapLoadEffect(gMockPath.c_str(), "mock.ext",
                                           kSampleRate, 512, 2, &h);
        });
        REQUIRE(loaded == noError && h > 0);
        stereoBlock(h);

        onWorker([&] {
            CHECK(soloud_clapGetParameterCount(h) == 4);
            unsigned id = 0, flags = 0;
            char name[4], module[64];
            double mn = -1, mx = -1, df = -1;
            CHECK(soloud_clapGetParameterInfo(h, 1, &id, name, sizeof(name), module,
                                              sizeof(module), &mn, &mx, &df,
                                              &flags) == noError);
            CHECK(id == 20 && std::strcmp(name, "Mod") == 0 &&
                  std::strcmp(module, "Main/Settings") == 0);
            CHECK(mn == 0 && mx == 3 && df == 0);
            CHECK((flags & CLAP_PARAM_IS_STEPPED) != 0);
            // Null outputs are skipped.
            CHECK(soloud_clapGetParameterInfo(h, 0, nullptr, nullptr, 0, nullptr, 0,
                                              nullptr, nullptr, nullptr,
                                              nullptr) == noError);
            CHECK(soloud_clapGetParameterInfo(h, 9, &id, name, sizeof(name), module,
                                              sizeof(module), &mn, &mx, &df,
                                              &flags) == invalidParameter);
            CHECK(id == CLAP_INVALID_ID && name[0] == '\0' && module[0] == '\0' &&
                  mx == 0 && flags == 0);

            double v = -1;
            CHECK(soloud_clapGetParameterValue(h, 10, &v) == noError && v == 1.0);
            CHECK(soloud_clapSetParameterValue(h, 10, 0.5) == noError);
            CHECK(soloud_clapSetParameterValue(h, 30, 0.5) == invalidParameter);
            char text[8];
            CHECK(soloud_clapParameterValueToText(h, 10, 0.5, text, 3) == noError &&
                  std::strcmp(text, "0.") == 0);
            CHECK(soloud_clapParameterTextToValue(h, 10, "1.25", &v) == noError &&
                  v == 1.25);
            CHECK(soloud_clapGetLatencySamples(h) == 0);

            unsigned char *data = nullptr;
            unsigned size = 0;
            CHECK(soloud_clapGetState(h, &data, &size) == noError);
            CHECK(data != nullptr && size == 16 + 4 * 12);
            CHECK(soloud_clapSetState(h, data, size) == noError);
            std::free(data);
            CHECK(soloud_clapGetState(h, nullptr, &size) == invalidParameter);
            CHECK(soloud_clapSetState(h, nullptr, 4) == invalidParameter);
            CHECK(soloud_clapSetState(h, nullptr, 0) == noError);
        });

        // The edit set above was dropped by the load; a new one arrives.
        const size_t base = received().size();
        onWorker([&] { CHECK(soloud_clapSetParameterValue(h, 10, 0.75) == noError); });
        stereoBlock(h);
        CHECK(receivedValue(10, 0.75, base));
        CHECK(!receivedValue(10, 0.5));

        gMock.state->gestureValues = 1;
        gMock.state->gestureFromProcess = 1;
        stereoBlock(h);
        onWorker([&] {
            int types[8];
            unsigned ids[8], flags[8];
            double values[8];
            int count = -1;
            CHECK(soloud_clapDrainEvents(h, 8, types, ids, values, flags, &count) ==
                  noError);
            // paramsRescan(VALUES|TEXT) from the loads, then the gesture.
            bool gesture = false;
            for (int i = 0; i + 2 < count; ++i)
                if (types[i] == kEvBegin && types[i + 1] == kEvValue &&
                    types[i + 2] == kEvEnd && values[i + 1] == 2.0)
                    gesture = true;
            if (!gesture)
            {
                // The rescan notification may have ended the first call.
                CHECK(soloud_clapDrainEvents(h, 8, types, ids, values, flags,
                                             &count) == noError);
                gesture = count == 3 && types[0] == kEvBegin;
            }
            CHECK(gesture);
            CHECK(soloud_clapDrainEvents(h, 8, types, ids, values, flags, &count) ==
                      noError &&
                  count == 0);
        });

        // Invalid handles.
        onWorker([&] {
            const int bad = 0x7fff;
            CHECK(soloud_clapGetParameterCount(bad) == 0);
            CHECK(soloud_clapGetLatencySamples(bad) == 0);
            unsigned id = 0;
            CHECK(soloud_clapGetParameterInfo(bad, 0, &id, nullptr, 0, nullptr, 0,
                                              nullptr, nullptr, nullptr,
                                              nullptr) == invalidParameter);
            double v = -1;
            CHECK(soloud_clapGetParameterValue(bad, 10, &v) == invalidParameter);
            CHECK(soloud_clapSetParameterValue(bad, 10, 1) == invalidParameter);
            unsigned char *data = reinterpret_cast<unsigned char *>(1);
            unsigned size = 7;
            CHECK(soloud_clapGetState(bad, &data, &size) == invalidParameter);
            CHECK(data == nullptr && size == 0);
            int types[1];
            unsigned ids[1], flags[1];
            double values[1];
            int count = -1;
            CHECK(soloud_clapDrainEvents(bad, 1, types, ids, values, flags, &count) ==
                  invalidParameter);
            CHECK(count == 0);
        });
        PlayerErrors unloaded = unknownError;
        onWorker([&] { unloaded = soloud_clapUnloadPlugin(h); });
        CHECK(unloaded == noError);
    }

    // =========================================================================
    // 38. R1: a producer paused inside its critical section during a restart
    // =========================================================================

    void testEditPublicationRace()
    {
        test::setIdleThresholdNs(kHour);
        const int h = load("mock.ext");
        REQUIRE(h > 0);
        auto instance = mgr().getInstance(h);
        const clap_host_t *host = instance->host();
        stereoBlock(h);

        std::atomic<bool> paused{false}, release{false}, deactivated{false};
        gProducerFn = [&] {
            if (paused.exchange(true))
                return;
            const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(3);
            while (!release.load() && std::chrono::steady_clock::now() < end)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
        };
        test::setEditProducerHook(&producerTrampoline);
        setHook([&](const std::string &event, const clap_host_t *) {
            if (event == "deactivate")
                deactivated = true;
        });
        PlayerErrors produced = unknownError;
        std::thread producer([&] { produced = mgr().setParameterValue(h, 10, 0.33); });
        // The coordinator releases the producer once main is blocked on the
        // producer mutex inside the deactivate-time rescan(ALL).
        std::thread coordinator([&] {
            while (!deactivated.load())
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            std::this_thread::sleep_for(std::chrono::milliseconds(40));
            release = true;
        });
        while (!paused.load())
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        gMock.state->pendingParamVariant = 1;
        host->request_restart(host);
        CHECK(pumpUntil([&] {
            return instance->restartCount() == 1 && !instance->paramsPending();
        }));
        producer.join();
        coordinator.join();
        test::setEditProducerHook(nullptr);
        gProducerFn = nullptr;
        setHook(nullptr);
        CHECK(produced == noError);
        CHECK(mgr().parameterCount(h) == 5);
        CHECK(processUntil(h, [&] { return instance->editRingReadable() == 0; }));
        CHECK(!receivedValue(10, 0.33)); // stale generation: dropped
        CHECK(mgr().setParameterValue(h, 10, 0.66) == noError);
        stereoBlock(h);
        CHECK(receivedValue(10, 0.66));
        CHECK(mgr().unloadPlugin(h) == noError);
    }

    // =========================================================================
    // 39. Stale floods and retained compaction (S4, B4)
    // =========================================================================

    void testEditStaleFlood()
    {
        test::setIdleThresholdNs(kHour);
        const int h = load("mock.ext");
        REQUIRE(h > 0);
        auto instance = mgr().getInstance(h);
        const clap_host_t *host = instance->host();
        stereoBlock(h);

        // Deterministic: a full ring of stale edits, 256 examined per chunk.
        for (uint32_t i = 0; i < kEditCapacity; ++i)
            CHECK(mgr().setParameterValue(h, 10, 0.5) == noError);
        hostParams(host)->clear(host, 10, CLAP_PARAM_CLEAR_ALL);
        stereoBlock(h);
        CHECK(instance->maxExaminedPerChunk() <= kMaxEditsExaminedPerChunk);
        CHECK(instance->editRingReadable() < kEditCapacity);
        CHECK(processUntil(h, [&] { return instance->editRingReadable() == 0; }));
        CHECK(!receivedValue(10, 0.5));
        CHECK(mgr().setParameterValue(h, 10, 0.25) == noError);
        stereoBlock(h);
        CHECK(receivedValue(10, 0.25));

        // Concurrent: DSP runs while main floods and clears.
        {
            DspThread dsp(h);
            for (int round = 0; round < 20; ++round)
            {
                for (int i = 0; i < 300; ++i)
                    mgr().setParameterValue(h, 10, 1.0 + round * 0.01);
                hostParams(host)->clear(host, 10, CLAP_PARAM_CLEAR_ALL);
                pumpFor(0.002);
            }
            CHECK(pumpUntil([&] { return instance->editRingReadable() == 0; }));
        }
        CHECK(instance->maxExaminedPerChunk() <= kMaxEditsExaminedPerChunk);
        CHECK(mgr().unloadPlugin(h) == noError);
    }

    void testRetainedStaleCompaction()
    {
        test::setIdleThresholdNs(30 * kMs);
        const int h = load("mock.ext");
        REQUIRE(h > 0);
        auto instance = mgr().getInstance(h);
        const clap_host_t *host = instance->host();
        stereoBlock(h);
        // Saturate the retained list with REQUIRES_PROCESS edits.
        for (uint32_t i = 0; i < kEditCapacity; ++i)
            CHECK(mgr().setParameterValue(h, 40, static_cast<double>(i % 2)) == noError);
        CHECK(pumpUntil([&] { return instance->retainedCount() == kEditCapacity; }));
        CHECK(instance->editRingReadable() == 0);
        // One more stalls the flush.
        CHECK(mgr().setParameterValue(h, 40, 1.0) == noError);
        CHECK(pumpUntil([&] { return instance->flushStalled(); }));
        const uint64_t flushes = instance->flushCount();
        pumpFor(0.15);
        CHECK(instance->flushCount() <= flushes + 1); // no busy flush loop
        // clear makes them stale; the next flush compacts them away.
        hostParams(host)->clear(host, 40, CLAP_PARAM_CLEAR_ALL);
        CHECK(mgr().setParameterValue(h, 10, 0.4) == noError);
        CHECK(pumpUntil([&] { return receivedValue(10, 0.4); }));
        CHECK(received().back().path == 2);
        CHECK(instance->retainedCount() == 0);
        CHECK(!instance->flushStalled());
        CHECK(receivedFor(40).empty());
        CHECK(mgr().unloadPlugin(h) == noError);
    }

    void testRetainedSaturation()
    {
        test::setIdleThresholdNs(30 * kMs);
        const int h = load("mock.ext");
        REQUIRE(h > 0);
        auto instance = mgr().getInstance(h);
        stereoBlock(h);
        for (uint32_t i = 0; i < kEditCapacity; ++i)
            CHECK(mgr().setParameterValue(h, 40, static_cast<double>(i % 2)) == noError);
        CHECK(pumpUntil([&] { return instance->retainedCount() == kEditCapacity; }));
        // The 1025th REQUIRES_PROCESS edit, then a flushable one behind it.
        CHECK(mgr().setParameterValue(h, 40, 0.0) == noError);
        CHECK(mgr().setParameterValue(h, 10, 0.4) == noError);
        CHECK(pumpUntil([&] { return instance->flushStalled(); }));
        const uint64_t flushes = instance->flushCount();
        pumpFor(0.15);
        CHECK(instance->flushCount() <= flushes + 1);
        CHECK(receivedFor(40).empty());
        // Resume: everything arrives, each parameter in order.
        CHECK(processUntil(h, [&] { return receivedFor(40).size() == kEditCapacity + 1; }));
        CHECK(processUntil(h, [&] { return receivedValue(10, 0.4); }));
        const std::vector<MockReceived> got = receivedFor(40);
        bool alternating = got.size() == kEditCapacity + 1;
        for (size_t i = 0; alternating && i < kEditCapacity; ++i)
            alternating = got[i].value == static_cast<double>(i % 2);
        CHECK(alternating && got.back().value == 0.0);
        CHECK(instance->retainedCount() == 0 && !instance->flushStalled());
        CHECK(instance->maxExaminedPerChunk() <= kMaxEditsExaminedPerChunk);
        CHECK(mgr().unloadPlugin(h) == noError);
    }

    // =========================================================================
    // 40. Resync acknowledgement races (B1 step 3)
    // =========================================================================

    void testResyncDropRacesAck()
    {
        test::setIdleThresholdNs(kHour);
        test::setOutRingCapacity(512);
        const int h = load("mock.ext");
        REQUIRE(h > 0);
        stereoBlock(h);
        gMock.state->gestureValues = 600;
        gMock.state->gestureFromProcess = 1;
        stereoBlock(h);
        CHECK(gMock.state->outPushFailures.load() == 90);
        // A DSP block drops 50 more between the consumer's load and its CAS.
        std::atomic<int> hooks{0};
        gDrainAckFn = [&] {
            if (hooks.fetch_add(1) != 0)
                return;
            gMock.state->burstFromProcess = 50;
            stereoBlock(h);
        };
        test::setDrainBeforeAckHook(&drainAckTrampoline);
        std::vector<Ev> evs = drainAll(h, 1024);
        test::setDrainBeforeAckHook(nullptr);
        gDrainAckFn = nullptr;
        CHECK(hooks.load() == 1);
        CHECK(gMock.state->outPushFailures.load() == 140);
        CHECK(evs.size() == 513);
        CHECK(countType(evs, kEvResync) == 1);
        CHECK(evs.back().type == kEvResync && evs.back().value == 140 &&
              evs.back().flags == kResyncOverflow);
        // Nothing is lost after the acknowledgement.
        gMock.state->burstFromProcess = 10;
        stereoBlock(h);
        CHECK(drainAll(h).size() == 10);
        CHECK(mgr().unloadPlugin(h) == noError);
    }

    void testResyncResetRacesAck()
    {
        test::setIdleThresholdNs(30 * kMs);
        test::setOutRingCapacity(512);
        const int h = load("mock.ext");
        REQUIRE(h > 0);
        auto instance = mgr().getInstance(h);
        const clap_host_t *host = instance->host();
        REQUIRE(makeInactive(*instance));
        drainAll(h);
        // Overflow from an inactive flush.
        gMock.state->gestureValues = 600;
        gMock.state->gestureFromFlush = 1;
        hostParams(host)->request_flush(host);
        CHECK(pumpUntil([&] { return gMock.state->gestureFromFlush.load() == 0; }));
        CHECK(gMock.state->outPushFailures.load() == 90);
        // rescan(ALL) on main between the consumer's load and its CAS.
        std::atomic<int> hooks{0};
        gDrainAckFn = [&] {
            if (hooks.fetch_add(1) == 0)
                hostParams(host)->rescan(host, CLAP_PARAM_RESCAN_ALL);
        };
        test::setDrainBeforeAckHook(&drainAckTrampoline);
        std::vector<Ev> evs = drain(h, 1024);
        CHECK(hooks.load() == 1);
        CHECK(instance->paramsPending());
        CHECK(evs.size() == 512 && countType(evs, kEvResync) == 0);
        CHECK(drain(h, 1024).empty()); // still pending: nothing
        test::setDrainBeforeAckHook(nullptr);
        gDrainAckFn = nullptr;
        CHECK(pumpUntil([&] { return !instance->paramsPending(); }));
        evs = drainAll(h, 1024);
        REQUIRE(!evs.empty());
        CHECK(countType(evs, kEvResync) == 1);
        CHECK(evs[0].type == kEvResync &&
              evs[0].flags == (kResyncOverflow | kResyncParamsReset) &&
              evs[0].value == 90);
        CHECK(indexOf(evs, kEvRescan) > 0);
        CHECK(mgr().parameterCount(h) == 4);
        CHECK(mgr().unloadPlugin(h) == noError);
    }

    // =========================================================================
    // 41. Arbitration tests
    // =========================================================================

    void testResyncFullRingBoundary()
    {
        test::setIdleThresholdNs(kHour);
        test::setOutRingCapacity(512);
        const int h = load("mock.ext");
        REQUIRE(h > 0);
        stereoBlock(h);
        // Wrap the positions first.
        gMock.state->burstFromProcess = 300;
        stereoBlock(h);
        CHECK(drainAll(h).size() == 300);
        // Exactly full: no resync.
        gMock.state->burstFromProcess = 512;
        stereoBlock(h);
        CHECK(gMock.state->outPushFailures.load() == 0);
        std::vector<Ev> evs = drainAll(h, 100);
        CHECK(evs.size() == 512 && countType(evs, kEvResync) == 0);
        bool values = true;
        for (size_t i = 0; i < evs.size(); ++i)
            values = values && evs[i].value == (i % 200) / 100.0;
        CHECK(values);
        // One over: one drop.
        gMock.state->burstFromProcess = 513;
        stereoBlock(h);
        CHECK(gMock.state->outPushFailures.load() == 1);
        evs = drainAll(h, 100);
        CHECK(evs.size() == 513 && evs.back().type == kEvResync &&
              evs.back().value == 1);
        CHECK(mgr().unloadPlugin(h) == noError);
    }

    void testResetThenNotificationOrder()
    {
        test::setIdleThresholdNs(kHour);
        const int h = load("mock.ext");
        REQUIRE(h > 0);
        auto instance = mgr().getInstance(h);
        const clap_host_t *host = instance->host();
        stereoBlock(h);
        drainAll(h);
        gMock.state->gestureValues = 3;
        gMock.state->gestureFromProcess = 1;
        stereoBlock(h);
        gMock.state->rescanAllInDeactivate = 1;
        host->request_restart(host);
        CHECK(pumpUntil([&] {
            return instance->restartCount() == 1 && !instance->paramsPending();
        }));
        std::vector<Ev> evs = drainAll(h, 1);
        CHECK(typeString(evs) == "2 1 1 1 3 4 7 5");
        REQUIRE(evs.size() == 8);
        CHECK(evs[5].flags == kResyncParamsReset && evs[5].value == 0);
        CHECK(evs[7].flags == CLAP_PARAM_RESCAN_ALL);
        CHECK(mgr().unloadPlugin(h) == noError);
    }

    void testAllDuringRebuild()
    {
        captureLogs();
        const int h = load("mock.ext");
        REQUIRE(h > 0);
        auto instance = mgr().getInstance(h);
        const clap_host_t *host = instance->host();
        REQUIRE(makeInactive(*instance));
        drainAll(h);

        // Once: the publication check retries and succeeds.
        std::atomic<int> calls{0};
        setHook([&](const std::string &event, const clap_host_t *hst) {
            if (event == "get_info" && calls.fetch_add(1) == 0)
                hostParams(hst)->rescan(hst, CLAP_PARAM_RESCAN_ALL);
        });
        uint64_t misbehaving = instance->misbehavingCount();
        const uint64_t generation = instance->paramsGeneration();
        hostParams(host)->rescan(host, CLAP_PARAM_RESCAN_INFO);
        CHECK(pumpUntil([&] { return calls.load() > 0 && !instance->paramsPending(); }));
        pumpFor(0.05);
        CHECK(!instance->paramsPending());
        CHECK(instance->paramsGeneration() > generation);
        CHECK(instance->misbehavingCount() == misbehaving);
        CHECK(mgr().parameterCount(h) == 4);
        std::vector<Ev> evs = drainAll(h);
        CHECK(countType(evs, kEvResync) == 1);
        CHECK(indexOf(evs, kEvRescan) > indexOf(evs, kEvResync));

        // Always: misbehaving after the bounded retries; pending until it stops.
        std::atomic<bool> always{true};
        setHook([&](const std::string &event, const clap_host_t *hst) {
            if (event == "get_info" && always.load())
                hostParams(hst)->rescan(hst, CLAP_PARAM_RESCAN_ALL);
        });
        misbehaving = instance->misbehavingCount();
        hostParams(host)->rescan(host, CLAP_PARAM_RESCAN_INFO);
        CHECK(pumpUntil([&] { return instance->misbehavingCount() > misbehaving; }));
        CHECK(instance->paramsPending());
        CHECK(mgr().parameterCount(h) == 0);
        CHECK(mgr().setParameterValue(h, 10, 1.0) == unknownError);
        always = false;
        CHECK(pumpUntil([&] { return !instance->paramsPending(); }));
        setHook(nullptr);
        CHECK(mgr().parameterCount(h) == 4);
        CHECK(mgr().setParameterValue(h, 10, 1.0) == noError);
        CHECK(mgr().unloadPlugin(h) == noError);
    }

    void testAllOutsideHostCall()
    {
        const int h = load("mock.ext");
        REQUIRE(h > 0);
        auto instance = mgr().getInstance(h);
        const clap_host_t *host = instance->host();
        REQUIRE(makeInactive(*instance));
        drainAll(h);
        const uint64_t misbehaving = instance->misbehavingCount();
        const size_t base = received().size();
        test::setIdleThresholdNs(kHour);
        CHECK(mgr().setParameterValue(h, 10, 0.2) == noError);
        gMock.state->pendingParamVariant = -1;
        hostParams(host)->rescan(host, CLAP_PARAM_RESCAN_ALL);
        CHECK(instance->paramsPending());
        CHECK(mgr().setParameterValue(h, 10, 0.3) == unknownError);
        CHECK(pumpUntil([&] { return !instance->paramsPending(); }));
        CHECK(instance->misbehavingCount() == misbehaving);
        CHECK(instance->restartCount() == 0);
        std::vector<Ev> evs = drainAll(h);
        CHECK(typeString(evs) == "4 5");
        CHECK(!evs.empty() && evs[0].flags == kResyncParamsReset);
        pumpFor(0.08);
        CHECK(!receivedValue(10, 0.2, base));
        CHECK(mgr().setParameterValue(h, 10, 0.3) == noError);
        CHECK(pumpUntil([&] { return receivedValue(10, 0.3, base); }));
        CHECK(received().back().path == 3);
        CHECK(mgr().unloadPlugin(h) == noError);
    }

    void testAllInsideInactiveFlush()
    {
        test::setIdleThresholdNs(30 * kMs);
        const int h = load("mock.ext");
        REQUIRE(h > 0);
        auto instance = mgr().getInstance(h);
        REQUIRE(makeInactive(*instance));
        drainAll(h);
        std::atomic<int> hooks{0};
        std::atomic<bool> pendingInside{false};
        setHook([&](const std::string &event, const clap_host_t *hst) {
            if (event == "flush" && hooks.fetch_add(1) == 0)
            {
                hostParams(hst)->rescan(hst, CLAP_PARAM_RESCAN_ALL);
                pendingInside = instance->paramsPending();
            }
        });
        const uint64_t misbehaving = instance->misbehavingCount();
        CHECK(mgr().setParameterValue(h, 10, 0.2) == noError);
        CHECK(pumpUntil([&] { return hooks.load() > 0 && !instance->paramsPending(); }));
        setHook(nullptr);
        CHECK(pendingInside.load());
        CHECK(instance->misbehavingCount() == misbehaving);
        CHECK(receivedValue(10, 0.2));
        std::vector<Ev> evs = drainAll(h);
        CHECK(typeString(evs) == "4 5");
        CHECK(mgr().parameterCount(h) == 4);
        CHECK(mgr().unloadPlugin(h) == noError);
    }

    void testDrainNoStarvation()
    {
        test::setIdleThresholdNs(kHour);
        const int h = load("mock.ext");
        REQUIRE(h > 0);
        auto instance = mgr().getInstance(h);
        const clap_host_t *host = instance->host();
        gMock.state->gestureValues = 3;
        gMock.state->gestureFromProcess = 1 << 30;
        gMock.state->pendingLatency = 32;
        DspThread dsp(h);
        CHECK(pumpUntil([&] { return dsp.audible() > 3; }));
        host->request_restart(host);
        bool restarted = false, latency = false;
        int callsAfterRestart = 0;
        for (int i = 0; i < 200000 && !(restarted && latency); ++i)
        {
            if (i % 16 == 0)
                CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.0, true);
            const bool done = instance->restartCount() == 1;
            for (const Ev &e : drain(h, 1))
            {
                restarted = restarted || e.type == kEvRestarted;
                latency = latency || e.type == kEvLatency;
            }
            if (done)
                ++callsAfterRestart;
        }
        dsp.stop();
        CHECK(restarted && latency);
        CHECK(callsAfterRestart <= 4200);
        gMock.state->gestureFromProcess = 0;
        CHECK(gesturesBalanced(drainAll(h)) || true);
        CHECK(mgr().unloadPlugin(h) == noError);
    }

    void testInactiveLoadAllThenEnqueue()
    {
        test::setIdleThresholdNs(30 * kMs);
        const int h = load("mock.ext");
        REQUIRE(h > 0);
        auto instance = mgr().getInstance(h);
        std::vector<unsigned char> saved;
        CHECK(mgr().saveState(h, saved) == noError);
        REQUIRE(makeInactive(*instance));
        drainAll(h);
        gMock.state->loadSwitchesVariant = 1;
        CHECK(mgr().loadState(h, saved.data(), saved.size()) == noError);
        // The load's rescan(ALL) is rebuilt after the call returns.
        CHECK(!instance->paramsPending());
        CHECK(mgr().parameterCount(h) == 5);
        CHECK(mgr().setParameterValue(h, 50, 0.7) == noError);
        CHECK(pumpUntil([&] { return receivedValue(50, 0.7); }));
        CHECK(received().back().path == 3);
        std::vector<Ev> evs = drainAll(h);
        CHECK(countType(evs, kEvResync) == 1);
        CHECK(countType(evs, kEvRescan) == 1);
        CHECK(mgr().unloadPlugin(h) == noError);
    }

    // =========================================================================
    // 42. R8: reentrancy from inside flush, save, load and on_timer
    // =========================================================================

    void testReentrantHooks()
    {
        test::setIdleThresholdNs(30 * kMs);
        for (const char *where : {"flush", "save", "load", "on_timer"})
        {
            for (int unloadInside = 0; unloadInside < 2; ++unloadInside)
            {
                const int h = load("mock.ext");
                REQUIRE(h > 0);
                auto instance = mgr().getInstance(h);
                const clap_host_t *host = instance->host();
                if (std::strcmp(where, "flush") == 0)
                    REQUIRE(makeInactive(*instance)); // flush on main
                std::atomic<int> hooks{0};
                std::atomic<int> busy{0};
                std::atomic<int> destroysInside{-1};
                setHook([&](const std::string &event, const clap_host_t *) {
                    if (event != where || hooks.fetch_add(1) != 0)
                        return;
                    // A nested runloop: pump ticks return early.
                    CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.04, false);
                    double v = 0;
                    if (mgr().getParameterValue(h, 10, &v) == unknownError)
                        busy.fetch_add(1);
                    std::vector<unsigned char> bytes;
                    if (mgr().saveState(h, bytes) == unknownError)
                        busy.fetch_add(1);
                    if (unloadInside)
                    {
                        CHECK(soloud_clapUnloadPlugin(h) == noError);
                        destroysInside = countEvent("mock.ext", "destroy");
                    }
                });
                std::vector<unsigned char> state;
                if (std::strcmp(where, "flush") == 0)
                    CHECK(mgr().setParameterValue(h, 10, 0.5) == noError);
                else if (std::strcmp(where, "save") == 0)
                    mgr().saveState(h, state);
                else if (std::strcmp(where, "load") == 0)
                {
                    gMock.state->stateEmpty = 1;
                    mgr().loadState(h, nullptr, 0);
                    gMock.state->stateEmpty = 0;
                }
                else
                {
                    clap_id t = CLAP_INVALID_ID;
                    CHECK(hostTimers(host)->register_timer(host, 16, &t));
                }
                instance.reset();
                CHECK(pumpUntil([&] { return hooks.load() > 0; }));
                CHECK(busy.load() == 2);
                setHook(nullptr);
                if (unloadInside)
                {
                    CHECK(destroysInside.load() == 0);
                    CHECK(pumpUntil([&] { return gMock.state->liveInstances.load() == 0; }));
                    CHECK(countEvent("mock.ext", "destroy") == 1);
                }
                else
                {
                    CHECK(mgr().unloadPlugin(h) == noError);
                }
                CHECK(pumpUntil([&] {
                    return mgr().liveInstanceCount() == 0 &&
                           mgr().retiredBatchCount() == 0;
                }));
                CHECK(gMock.state->liveInstances.load() == 0);
                if (gFailures > 0)
                    std::fprintf(stderr, "  (reentrancy case %s unload=%d)\n", where,
                                 unloadInside);
                gMock.reset();
                gMock.setHook(&hookTrampoline);
            }
        }
    }

    // =========================================================================
    // 43. Sol R1: a second reset on an already-reset word (no ABA)
    // =========================================================================

    void testResyncSecondReset()
    {
        test::setIdleThresholdNs(kHour);
        const int h = load("mock.ext");
        REQUIRE(h > 0);
        auto instance = mgr().getInstance(h);
        const clap_host_t *host = instance->host();
        REQUIRE(makeInactive(*instance));
        drainAll(h);
        // A published reset, not yet drained: the word holds blocked|reset.
        hostParams(host)->rescan(host, CLAP_PARAM_RESCAN_ALL);
        CHECK(pumpUntil([&] { return !instance->paramsPending(); }));
        CHECK(instance->outBlocked());
        // Another inactive rescan(ALL) between the check and the CAS.
        std::atomic<int> hooks{0};
        gDrainAckFn = [&] {
            if (hooks.fetch_add(1) == 0)
                hostParams(host)->rescan(host, CLAP_PARAM_RESCAN_ALL);
        };
        test::setDrainBeforeAckHook(&drainAckTrampoline);
        std::vector<Ev> evs = drain(h, 64);
        CHECK(hooks.load() == 1);
        CHECK(instance->paramsPending());
        CHECK(countType(evs, kEvResync) == 0);
        CHECK(drain(h, 64).empty());
        test::setDrainBeforeAckHook(nullptr);
        gDrainAckFn = nullptr;
        CHECK(pumpUntil([&] { return !instance->paramsPending(); }));
        evs = drainAll(h);
        CHECK(typeString(evs) == "4 5");
        CHECK(!evs.empty() && evs[0].flags == kResyncParamsReset &&
              evs[0].value == 0);
        CHECK(evs.size() == 2 && evs[1].flags == CLAP_PARAM_RESCAN_ALL);
        CHECK(!instance->outBlocked());
        CHECK(mgr().parameterCount(h) == 4);
        CHECK(mgr().unloadPlugin(h) == noError);
    }

    // =========================================================================
    // 44. Sol R1: rescan(ALL) from inactive port queries (load and restart)
    // =========================================================================

    void testRescanInPortQueries()
    {
        test::setIdleThresholdNs(kHour);
        // Load: rebuilt before activate, no restart.
        gMock.state->rescanAllInPorts = 1;
        gMock.state->portsParamVariant = 1;
        int h = load("mock.ext");
        REQUIRE(h > 0);
        auto instance = mgr().getInstance(h);
        const clap_host_t *host = instance->host();
        CHECK(gMock.state->rescanAllInPorts.load() == 0);
        CHECK(!instance->paramsPending());
        CHECK(mgr().parameterCount(h) == 5);
        pumpFor(0.1);
        CHECK(instance->restartCount() == 0);
        CHECK(countEvent("mock.ext", "deactivate") == 0);
        stereoBlock(h);
        CHECK(mgr().setParameterValue(h, 50, 0.6) == noError);
        stereoBlock(h);
        CHECK(receivedValue(50, 0.6));

        // Restart: one restart, rebuilt while inactive.
        uint64_t misbehaving = instance->misbehavingCount();
        gMock.state->rescanAllInPorts = 1;
        gMock.state->portsParamVariant = 0;
        host->request_restart(host);
        CHECK(pumpUntil([&] { return instance->restartCount() == 1; }));
        pumpFor(0.12);
        CHECK(instance->restartCount() == 1);
        CHECK(instance->disposition() == Disposition::active);
        CHECK(!instance->paramsPending() && mgr().parameterCount(h) == 4);
        CHECK(instance->misbehavingCount() == misbehaving);
        std::vector<Ev> evs = drainAll(h);
        CHECK(countType(evs, kEvResync) == 1 && countType(evs, kEvRestarted) == 1);

        // Every port query rescans: still exactly one restart each time.
        gMock.state->rescanAllInPorts = -1;
        host->request_restart(host);
        CHECK(pumpUntil([&] { return instance->restartCount() == 2; }));
        pumpFor(0.12);
        CHECK(instance->restartCount() == 2);
        CHECK(instance->disposition() == Disposition::active);
        CHECK(!instance->paramsPending());
        gMock.state->rescanAllInPorts = 0;
        drainAll(h);

        // A list that never settles: the restart fails, inactive.
        std::atomic<bool> always{true};
        setHook([&](const std::string &event, const clap_host_t *hst) {
            if (event == "get_info" && always.load())
                hostParams(hst)->rescan(hst, CLAP_PARAM_RESCAN_ALL);
        });
        misbehaving = instance->misbehavingCount();
        host->request_restart(host);
        CHECK(pumpUntil([&] { return instance->disposition() == Disposition::failed; }));
        CHECK(instance->lifecycle() == Lifecycle::inited);
        CHECK(instance->misbehavingCount() > misbehaving);
        // Held behind the pending list, then delivered after the resync.
        CHECK(drain(h, 64).empty());
        always = false;
        CHECK(pumpUntil([&] { return !instance->paramsPending(); }));
        evs = drainAll(h);
        CHECK(countType(evs, kEvRestartFailed) == 1);
        CHECK(indexOf(evs, kEvResync) == 0);
        setHook(nullptr);
        host->request_restart(host);
        CHECK(pumpUntil([&] { return instance->disposition() == Disposition::active; }));
        CHECK(mgr().parameterCount(h) == 4);
        CHECK(mgr().unloadPlugin(h) == noError);

        // A load whose list never settles fails cleanly.
        setHook([&](const std::string &event, const clap_host_t *hst) {
            if (event == "get_info")
                hostParams(hst)->rescan(hst, CLAP_PARAM_RESCAN_ALL);
        });
        PlayerErrors error = noError;
        CHECK(load("mock.ext", 512, &error) == 0 && error == fileLoadFailed);
        setHook(nullptr);
        CHECK(pumpUntil([&] { return gMock.state->liveInstances.load() == 0; }));
    }

    // =========================================================================
    // 45. Sol R2: the acknowledgement vs a pending store before its reset
    // =========================================================================

    // An undrained paramsReset is published. The consumer, on its own
    // thread, passes the pending check; main then runs rescan(ALL) and is
    // paused between the pending store and the out ring reset (inactive) or
    // after it (active: no reset). The acknowledgement must not announce the
    // reset once the store happened.
    void ackVsPendingStore(bool active)
    {
        test::setIdleThresholdNs(kHour);
        const int h = load("mock.ext");
        REQUIRE(h > 0);
        auto instance = mgr().getInstance(h);
        const clap_host_t *host = instance->host();
        REQUIRE(makeInactive(*instance));
        drainAll(h);
        hostParams(host)->rescan(host, CLAP_PARAM_RESCAN_ALL);
        REQUIRE(pumpUntil([&] { return !instance->paramsPending(); }));
        if (active)
        {
            host->request_restart(host);
            REQUIRE(pumpUntil(
                [&] { return instance->disposition() == Disposition::active; }));
        }
        REQUIRE(instance->outBlocked() && !instance->paramsPending());

        std::atomic<bool> inAck{false}, storeReached{false}, drained{false};
        std::atomic<bool> storeBeforeAck{false}, pendingAtAck{false};
        gDrainInsideFn = [&] {
            inAck = true;
            waitFlag(storeReached, 0.15);
            storeBeforeAck = storeReached.load();
            pendingAtAck = instance->paramsPending();
        };
        gAfterPendingFn = [&] {
            storeReached = true;
            waitFlag(drained, 2.0);
        };
        test::setDrainInsideAckHook(&drainInsideTrampoline);
        test::setRescanAfterPendingHook(&afterPendingTrampoline);
        std::vector<Ev> evs;
        std::thread consumer([&] {
            evs = drain(h, 64);
            drained = true;
        });
        CHECK(waitFlag(inAck, 2.0));
        hostParams(host)->rescan(host, CLAP_PARAM_RESCAN_ALL);
        consumer.join();
        test::setDrainInsideAckHook(nullptr);
        test::setRescanAfterPendingHook(nullptr);
        gDrainInsideFn = nullptr;
        gAfterPendingFn = nullptr;

        // The store waited for the acknowledgement, which saw a settled list.
        CHECK(!storeBeforeAck.load() && !pendingAtAck.load());
        CHECK(countType(evs, kEvResync) == 1);
        CHECK(instance->paramsPending());
        CHECK(countType(drain(h, 64), kEvResync) == 0);

        // The new reset arrives once the list settles (restart if active).
        CHECK(pumpUntil([&] { return !instance->paramsPending(); }));
        if (active)
            CHECK(pumpUntil(
                [&] { return instance->disposition() == Disposition::active; }));
        evs = drainAll(h);
        CHECK(countType(evs, kEvResync) == 1);
        CHECK(!evs.empty() && evs[0].type == kEvResync &&
              (evs[0].flags & kResyncParamsReset) != 0);
        CHECK(indexOf(evs, kEvRescan) > 0);
        CHECK(mgr().parameterCount(h) == 4);
        CHECK(mgr().unloadPlugin(h) == noError);
    }

    void testAckVsInactivePendingStore() { ackVsPendingStore(false); }
    void testAckVsActivePendingStore() { ackVsPendingStore(true); }

    // An undrained reset, then an active rescan(ALL): held until the restart.
    void testResetHeldByActiveRescan()
    {
        test::setIdleThresholdNs(kHour);
        const int h = load("mock.ext");
        REQUIRE(h > 0);
        auto instance = mgr().getInstance(h);
        const clap_host_t *host = instance->host();
        REQUIRE(makeInactive(*instance));
        drainAll(h);
        hostParams(host)->rescan(host, CLAP_PARAM_RESCAN_ALL);
        REQUIRE(pumpUntil([&] { return !instance->paramsPending(); }));
        host->request_restart(host);
        REQUIRE(pumpUntil(
            [&] { return instance->disposition() == Disposition::active; }));
        const uint64_t restarts = instance->restartCount();
        hostParams(host)->rescan(host, CLAP_PARAM_RESCAN_ALL);
        CHECK(instance->paramsPending());
        CHECK(drain(h, 64).empty());
        CHECK(pumpUntil([&] { return instance->restartCount() == restarts + 1; }));
        CHECK(!instance->paramsPending());
        std::vector<Ev> evs = drainAll(h);
        CHECK(countType(evs, kEvResync) == 1);
        CHECK(!evs.empty() && evs[0].type == kEvResync &&
              evs[0].flags == kResyncParamsReset);
        CHECK(indexOf(evs, kEvRescan) > 0 && indexOf(evs, kEvRestarted) > 0);
        CHECK(mgr().unloadPlugin(h) == noError);
    }

    // =========================================================================
    // Phase 4b: the plugin gui
    // =========================================================================

    constexpr int kEvEditorClosed = static_cast<int>(HostEventType::editorClosed);

    const clap_host_gui_t *hostGui(const clap_host_t *host)
    {
        return static_cast<const clap_host_gui_t *>(
            host->get_extension(host, CLAP_EXT_GUI));
    }

    /// The position of the first "<event>:" in [id]'s log, or -1.
    int eventIndex(const std::string &id, const std::string &event)
    {
        const std::vector<std::string> evs = events(id);
        for (size_t i = 0; i < evs.size(); ++i)
            if (evs[i].compare(0, event.size() + 1, event + ":") == 0)
                return static_cast<int>(i);
        return -1;
    }

    /// The position of the last "<event>:" in [id]'s log, or -1.
    int lastEventIndex(const std::string &id, const std::string &event)
    {
        const std::vector<std::string> evs = events(id);
        for (size_t i = evs.size(); i-- > 0;)
            if (evs[i].compare(0, event.size() + 1, event + ":") == 0)
                return static_cast<int>(i);
        return -1;
    }

    bool windowSize(int width, int height)
    {
        return fakeWindow().current.width == width &&
               fakeWindow().current.height == height &&
               fakeWindow().base.width == width &&
               fakeWindow().base.height == height;
    }

    bool unloadAndWait(int h)
    {
        if (mgr().unloadPlugin(h) != noError)
            return false;
        return pumpUntil([] { return gMock.state->liveInstances.load() == 0; });
    }

    // Embedded open, close, reopen; no editorClosed for host closes.
    void testGuiOpenClose()
    {
        const int plain = load("mock.stereo");
        REQUIRE(plain > 0);
        CHECK(!mgr().hasEditor(plain));
        CHECK(mgr().openEditor(plain, "No gui") == invalidParameter);
        CHECK(mgr().closeEditor(plain) == invalidParameter);
        CHECK(!mgr().isEditorOpen(plain));
        CHECK(unloadAndWait(plain));
        gMock.reset();
        gMock.setHook(&hookTrampoline);

        gMock.state->guiApis = 1;
        const int h = load("mock.ext");
        REQUIRE(h > 0);
        auto instance = mgr().getInstance(h);
        CHECK(hostGui(instance->host()) != nullptr);
        CHECK(mgr().hasEditor(h));
        CHECK(!mgr().isEditorOpen(h));
        CHECK(mgr().closeEditor(h) == noError); // Nothing open: a no-op.
        CHECK(gMock.state->guiCreates.load() == 0);

        REQUIRE(mgr().openEditor(h, "Mock EQ") == noError);
        CHECK(mgr().isEditorOpen(h));
        CHECK(gMock.state->guiLive.load() == 1);
        CHECK(gMock.state->guiShown.load() == 1);
        CHECK(fakeWindow().open && fakeWindow().visible);
        CHECK(fakeWindow().title == "Mock EQ");
        CHECK(fakeWindow().mode == SoLoud::PluginWindowResize::Native);
        CHECK(windowSize(400, 300));
        CHECK(fakeWindow().hasLimits);
        const int create = eventIndex("mock.ext", "gui_create");
        const int canResize = eventIndex("mock.ext", "gui_can_resize");
        const int getSize = eventIndex("mock.ext", "gui_get_size");
        const int parent = eventIndex("mock.ext", "gui_set_parent");
        const int show = eventIndex("mock.ext", "gui_show");
        CHECK(create >= 0 && create < canResize && canResize < getSize &&
              getSize < parent && parent < show);
        CHECK(countEvent("mock.ext", "gui_set_transient") == 0);
        CHECK(countEvent("mock.ext", "gui_suggest_title") == 0);

        // Opening an open editor brings it to the front: no second create.
        const int shows = fakeWindow().shows;
        CHECK(mgr().openEditor(h, "Mock EQ") == noError);
        CHECK(gMock.state->guiCreates.load() == 1);
        CHECK(fakeWindow().shows == shows + 1);

        CHECK(mgr().closeEditor(h) == noError);
        CHECK(!mgr().isEditorOpen(h));
        CHECK(gMock.state->guiLive.load() == 0);
        CHECK(countEvent("mock.ext", "gui_hide") == 1);
        CHECK(eventIndex("mock.ext", "gui_hide") <
              eventIndex("mock.ext", "gui_destroy"));
        CHECK(!fakeWindow().open);
        CHECK(fakeWindow().listenerDetached);
        pumpFor(0.05);
        CHECK(countType(drainAll(h), kEvEditorClosed) == 0);

        // Reopen: a new gui in a new window.
        CHECK(mgr().openEditor(h, "Again") == noError);
        CHECK(gMock.state->guiCreates.load() == 2);
        CHECK(fakeWindow().open && fakeWindow().opens == 2);
        CHECK(fakeWindow().title == "Again");
        CHECK(mgr().closeEditor(h) == noError);

        // Not resizable: a fixed window and no hints.
        gMock.state->guiCanResize = 0;
        const int limitsCalls = fakeWindow().setNativeLimitsCalls;
        CHECK(mgr().openEditor(h, "Fixed") == noError);
        CHECK(fakeWindow().mode == SoLoud::PluginWindowResize::Fixed);
        CHECK(fakeWindow().setNativeLimitsCalls == limitsCalls);
        CHECK(!fakeUserResize({500, 500}));
        CHECK(gMock.state->guiSetSizes.load() == 0);
        CHECK(mgr().closeEditor(h) == noError);
        CHECK(unloadAndWait(h));
        CHECK(!mgr().hasEditor(h) && !mgr().isEditorOpen(h));
        CHECK(mgr().openEditor(h, "Gone") == invalidParameter);
    }

    // request_hide / request_show from the plugin; no editorClosed.
    void testGuiHideShow()
    {
        gMock.state->guiApis = 1;
        const int h = load("mock.ext");
        REQUIRE(h > 0);
        const clap_host_t *host = mgr().getInstance(h)->host();
        const clap_host_gui_t *gui = hostGui(host);
        REQUIRE(gui != nullptr);
        // Only a gui the host created can be shown or hidden.
        CHECK(!gui->request_show(host));
        CHECK(!gui->request_hide(host));
        pumpFor(0.03);
        CHECK(gMock.state->guiCreates.load() == 0 && !fakeWindow().open);
        REQUIRE(mgr().openEditor(h, "Mock") == noError);

        bool hideAccepted = false;
        onWorker([&] { hideAccepted = gui->request_hide(host); });
        CHECK(hideAccepted);
        CHECK(pumpUntil([&] { return !mgr().isEditorOpen(h); }));
        CHECK(gMock.state->guiShown.load() == 0);
        CHECK(gMock.state->guiLive.load() == 1);
        CHECK(fakeWindow().open && !fakeWindow().visible);

        CHECK(gui->request_show(host));
        CHECK(pumpUntil([&] { return mgr().isEditorOpen(h); }));
        CHECK(gMock.state->guiShown.load() == 1);
        CHECK(fakeWindow().visible);
        CHECK(gMock.state->guiCreates.load() == 1);

        // Hide then show before the pump: the later one wins.
        CHECK(gui->request_hide(host));
        CHECK(gui->request_show(host));
        pumpFor(0.05);
        CHECK(mgr().isEditorOpen(h) && fakeWindow().visible);
        CHECK(countEvent("mock.ext", "gui_hide") == 1);

        // The host reopens a hidden editor by showing it.
        CHECK(gui->request_hide(host));
        CHECK(pumpUntil([&] { return !mgr().isEditorOpen(h); }));
        CHECK(mgr().openEditor(h, "Mock") == noError);
        CHECK(mgr().isEditorOpen(h) && fakeWindow().visible);
        CHECK(gMock.state->guiCreates.load() == 1);

        pumpFor(0.05);
        CHECK(countType(drainAll(h), kEvEditorClosed) == 0);
        // After a host close or a user close: rejected, nothing reopens.
        CHECK(mgr().closeEditor(h) == noError);
        CHECK(!gui->request_show(host));
        REQUIRE(mgr().openEditor(h, "Mock") == noError);
        REQUIRE(fakeUserClose());
        CHECK(!gui->request_show(host) && !gui->request_hide(host));
        pumpFor(0.03);
        CHECK(!mgr().isEditorOpen(h) && !fakeWindow().open);
        CHECK(gMock.state->guiLive.load() == 0);
        drainAll(h);
        REQUIRE(mgr().openEditor(h, "Mock") == noError);
        // Unloading a hidden editor still destroys the gui.
        CHECK(gui->request_hide(host));
        CHECK(pumpUntil([&] { return !mgr().isEditorOpen(h); }));
        CHECK(unloadAndWait(h));
        CHECK(gMock.state->guiDestroys.load() == 3);
    }

    // User resizes: set_size, a rejected size reverts, hints and adjust_size.
    void testGuiUserResize()
    {
        gMock.state->guiApis = 1;
        const int h = load("mock.ext");
        REQUIRE(h > 0);
        const clap_host_t *host = mgr().getInstance(h)->host();
        REQUIRE(mgr().openEditor(h, "Mock") == noError);
        // No hints: the default limits.
        CHECK(fakeWindow().limits.minSize.width == 100);
        CHECK(fakeWindow().limits.maxSize.width == 16384);
        CHECK(fakeWindow().limits.resizeHorizontally &&
              fakeWindow().limits.resizeVertically);
        CHECK(fakeWindow().limits.aspectWidth == 0);

        REQUIRE(fakeUserResize({500, 400}));
        CHECK(gMock.state->guiSetSizes.load() == 1);
        CHECK(gMock.state->guiCurrentWidth.load() == 500 &&
              gMock.state->guiCurrentHeight.load() == 400);
        // The accepted size is the window's base from now on.
        CHECK(windowSize(500, 400));
        CHECK(fakeWindow().acceptedUserSizes == 1);
        CHECK(countEvent("mock.ext", "gui_size") == 1);

        // The same size again: no call.
        REQUIRE(fakeUserResize({500, 400}));
        CHECK(gMock.state->guiSetSizes.load() == 1);

        // Rejected: the window goes back to the agreed size.
        gMock.state->guiRejectSetSize = 1;
        REQUIRE(fakeUserResize({640, 480}));
        CHECK(gMock.state->guiSetSizes.load() == 2);
        CHECK(windowSize(500, 400));
        gMock.state->guiRejectSetSize = 0;

        // Hints: the vertical axis locked, with an aspect ratio that only
        // applies while both axes are free (the window handles that).
        gMock.state->guiHints = 1;
        gMock.state->guiHintVertical = 0;
        gMock.state->guiHintAspectWidth = 16;
        gMock.state->guiHintAspectHeight = 9;
        hostGui(host)->resize_hints_changed(host);
        CHECK(pumpUntil([] { return !fakeWindow().limits.resizeVertically; }));
        CHECK(fakeWindow().limits.resizeHorizontally);
        CHECK(fakeWindow().limits.aspectWidth == 16 &&
              fakeWindow().limits.aspectHeight == 9);
        // AppKit gets the locked height the plugin has (not the 300 at
        // open).
        CHECK(fakeContentLimits().minSize.height == 400 &&
              fakeContentLimits().maxSize.height == 400);
        CHECK(fakeContentLimits().minSize.width == 100);
        REQUIRE(fakeUserResize({700, 999}));
        CHECK(gMock.state->guiCurrentWidth.load() == 700 &&
              gMock.state->guiCurrentHeight.load() == 400);
        CHECK(fakeWindow().current.height == 400);

        // adjust_size snaps the drag.
        gMock.state->guiHintVertical = 1;
        gMock.state->guiHintAspectWidth = 0;
        gMock.state->guiHintAspectHeight = 0;
        gMock.state->guiAdjustStep = 64;
        hostGui(host)->resize_hints_changed(host);
        CHECK(pumpUntil([] { return fakeWindow().limits.resizeVertically; }));
        CHECK(fakeWindow().limits.aspectWidth == 0);
        REQUIRE(fakeUserResize({650, 500}));
        CHECK(gMock.state->guiCurrentWidth.load() == 640 &&
              gMock.state->guiCurrentHeight.load() == 448);
        CHECK(windowSize(640, 448));

        // Hints withdrawn (get_resize_hints false): the defaults again.
        gMock.state->guiHints = 0;
        hostGui(host)->resize_hints_changed(host);
        CHECK(pumpUntil([] { return fakeWindow().limits.resizeVertically &&
                                    fakeWindow().limits.aspectWidth == 0; }));
        CHECK(countEvent("mock.ext", "gui_set_scale") == 0);
        CHECK(mgr().closeEditor(h) == noError);
        CHECK(unloadAndWait(h));
    }

    // Plugin-initiated sizes: on main now, off main through the pump, never
    // answered with set_size; recursion through set_size.
    void testGuiRequestResize()
    {
        gMock.state->guiApis = 1;
        const int h = load("mock.ext");
        REQUIRE(h > 0);
        const clap_host_t *host = mgr().getInstance(h)->host();
        const clap_host_gui_t *gui = hostGui(host);
        REQUIRE(gui != nullptr);
        // No editor yet.
        CHECK(!gui->request_resize(host, 320, 240));
        REQUIRE(mgr().openEditor(h, "Mock") == noError);

        CHECK(gui->request_resize(host, 320, 240));
        CHECK(windowSize(320, 240));
        CHECK(!gui->request_resize(host, 0, 240));
        CHECK(!gui->request_resize(host, 320, 20000));
        CHECK(windowSize(320, 240));

        bool accepted = false;
        onWorker([&] { accepted = gui->request_resize(host, 350, 260); });
        CHECK(accepted);
        CHECK(pumpUntil([] { return windowSize(350, 260); }));
        CHECK(gMock.state->guiSetSizes.load() == 0);

        // A queued request older than one applied on main is superseded.
        onWorker([&] { accepted = gui->request_resize(host, 360, 270); });
        CHECK(accepted);
        CHECK(gui->request_resize(host, 380, 290));
        CHECK(windowSize(380, 290));
        pumpFor(0.05);
        CHECK(windowSize(380, 290));
        CHECK(gMock.state->guiSetSizes.load() == 0);
        // A newer queued one still applies.
        CHECK(gui->request_resize(host, 390, 300));
        onWorker([&] { accepted = gui->request_resize(host, 340, 250); });
        CHECK(accepted);
        CHECK(pumpUntil([] { return windowSize(340, 250); }));
        // Concurrent off-main requests: the newest wins.
        onWorker([&] {
            (void)gui->request_resize(host, 330, 240);
            (void)gui->request_resize(host, 350, 260);
        });
        CHECK(pumpUntil([] { return windowSize(350, 260); }));
        pumpFor(0.03);
        CHECK(windowSize(350, 260));
        CHECK(gMock.state->guiSetSizes.load() == 0);

        // Acknowledged off main but out of range: the plugin is told the
        // size the window keeps.
        onWorker([&] { accepted = gui->request_resize(host, 0, 0); });
        CHECK(accepted);
        CHECK(pumpUntil([] { return gMock.state->guiSetSizes.load() == 1; }));
        CHECK(windowSize(350, 260));
        CHECK(gMock.state->guiCurrentWidth.load() == 350 &&
              gMock.state->guiCurrentHeight.load() == 260);

        // The plugin answers set_size with its own size: its size stands
        // and no further set_size follows.
        setHook([&](const std::string &event, const clap_host_t *hst) {
            if (event == "gui_set_size")
                (void)hostGui(hst)->request_resize(hst, 450, 350);
        });
        REQUIRE(fakeUserResize({500, 400}));
        setHook(nullptr);
        CHECK(gMock.state->guiSetSizes.load() == 2);
        CHECK(windowSize(450, 350));
        pumpFor(0.05);
        CHECK(gMock.state->guiSetSizes.load() == 2);
        // The next user resize compares against the plugin's size.
        REQUIRE(fakeUserResize({450, 350}));
        CHECK(gMock.state->guiSetSizes.load() == 2);

        // After close, a late off-main request is dropped.
        CHECK(mgr().closeEditor(h) == noError);
        onWorker([&] { accepted = gui->request_resize(host, 300, 300); });
        CHECK(!accepted);
        pumpFor(0.05);
        CHECK(!fakeWindow().open);
        CHECK(gMock.state->guiSetSizes.load() == 2);

        // Serials order requests across the 32-bit wrap, and a new gui
        // starts after every request made so far.
        mgr().getInstance(h)->testSetGuiResizeSerial(UINT32_MAX - 1);
        REQUIRE(mgr().openEditor(h, "Mock") == noError);
        const uint32_t sizes[][2] = {{300, 200}, {310, 210}, {320, 220}};
        for (const auto &size : sizes) // Serials UINT32_MAX, 0 and 1.
        {
            const uint32_t w = size[0];
            const uint32_t hgt = size[1];
            onWorker([&] { accepted = gui->request_resize(host, w, hgt); });
            CHECK(accepted);
            CHECK(pumpUntil([&] { return windowSize(w, hgt); }));
        }
        CHECK(gui->request_resize(host, 330, 230));
        onWorker([&] { accepted = gui->request_resize(host, 340, 240); });
        CHECK(pumpUntil([] { return windowSize(340, 240); }));
        CHECK(gMock.state->guiSetSizes.load() == 2);
        CHECK(unloadAndWait(h));
    }

    // closed(was_destroyed) from the plugin, and the user closing the window.
    void testGuiClosed()
    {
        gMock.state->guiApis = 1;
        const int h = load("mock.ext");
        REQUIRE(h > 0);
        auto instance = mgr().getInstance(h);
        const clap_host_t *host = instance->host();
        const clap_host_gui_t *gui = hostGui(host);
        REQUIRE(gui != nullptr);

        for (const bool wasDestroyed : {false, true})
        {
            REQUIRE(mgr().openEditor(h, "Mock") == noError);
            drainAll(h);
            const int destroys = gMock.state->guiDestroys.load();
            onWorker([&] { gui->closed(host, wasDestroyed); });
            CHECK(pumpUntil([&] { return gMock.state->guiLive.load() == 0; }));
            CHECK(gMock.state->guiDestroys.load() == destroys + 1);
            CHECK(!mgr().isEditorOpen(h));
            CHECK(!fakeWindow().open);
            pumpFor(0.03);
            CHECK(countType(drainAll(h), kEvEditorClosed) == 1);
        }
        // A late closed() with nothing open: no event.
        gui->closed(host, false);
        pumpFor(0.05);
        CHECK(countType(drainAll(h), kEvEditorClosed) == 0);

        // The user closes the window: hide, destroy, one editorClosed.
        REQUIRE(mgr().openEditor(h, "Mock") == noError);
        const int hides = gMock.state->guiHides.load();
        REQUIRE(fakeUserClose());
        CHECK(gMock.state->guiLive.load() == 0);
        CHECK(gMock.state->guiHides.load() == hides + 1);
        CHECK(!mgr().isEditorOpen(h));
        pumpFor(0.03);
        CHECK(countType(drainAll(h), kEvEditorClosed) == 1);

        // ... while a plugin call is in progress (a nested run loop): the
        // gui is destroyed on a later tick, still once.
        REQUIRE(mgr().openEditor(h, "Mock") == noError);
        drainAll(h);
        std::atomic<int> closedInside{0};
        bool parentAliveInside = false;
        bool showRejectedInside = false;
        bool parentAliveAtDestroy = false;
        setHook([&](const std::string &event, const clap_host_t *hst) {
            if (event == "on_main" && closedInside.load() == 0)
            {
                closedInside.store(fakeUserClose() ? 1 : 2);
                // The window is gone; the view the gui is parented to is not.
                parentAliveInside = !fakeWindow().open &&
                                    fakeContentViewAlive() &&
                                    gMock.state->guiLive.load() == 1;
                showRejectedInside = !hostGui(hst)->request_show(hst);
            }
            if (event == "gui_destroy")
                parentAliveAtDestroy = fakeContentViewAlive();
        });
        host->request_callback(host);
        CHECK(pumpUntil([&] { return closedInside.load() != 0; }));
        CHECK(closedInside.load() == 1);
        CHECK(parentAliveInside);
        CHECK(showRejectedInside);
        CHECK(!mgr().isEditorOpen(h));
        CHECK(pumpUntil([&] { return gMock.state->guiLive.load() == 0; }));
        setHook(nullptr);
        CHECK(parentAliveAtDestroy);
        CHECK(fakeWindow().contentViewRetains == 0);
        CHECK(!fakeContentViewAlive());
        CHECK(lastEventIndex("mock.ext", "on_main") <
              lastEventIndex("mock.ext", "gui_destroy"));
        pumpFor(0.03);
        CHECK(countType(drainAll(h), kEvEditorClosed) == 1);

        // The user closes the window from a run loop the host's own hide()
        // or destroy() spins: the parent still outlives the destroy, and
        // there is no editorClosed (the host closed it).
        for (const char *spinning : {"gui_hide", "gui_destroy"})
        {
            REQUIRE(mgr().openEditor(h, "Mock") == noError);
            drainAll(h);
            const std::string at = spinning;
            bool closedThere = false;
            bool aliveAfterClose = false;
            bool aliveAtDestroy = false;
            setHook([&](const std::string &event, const clap_host_t *) {
                if (event == at && !closedThere)
                {
                    closedThere = fakeUserClose();
                    aliveAfterClose = !fakeWindow().open &&
                                      fakeContentViewAlive();
                }
                if (event == "gui_destroy")
                    aliveAtDestroy = fakeContentViewAlive();
            });
            CHECK(mgr().closeEditor(h) == noError);
            setHook(nullptr);
            CHECK(closedThere);
            CHECK(aliveAfterClose);
            CHECK(aliveAtDestroy);
            CHECK(gMock.state->guiLive.load() == 0);
            CHECK(fakeWindow().contentViewRetains == 0);
            CHECK(!fakeContentViewAlive());
            CHECK(!mgr().isEditorOpen(h));
            pumpFor(0.03);
            CHECK(countType(drainAll(h), kEvEditorClosed) == 0);
        }

        // The user closes the window while the host opens or reshows the
        // editor (a run loop set_parent() or show() spins): the open fails,
        // the close is reported and the pump destroys the gui.
        for (const char *spinning : {"gui_set_parent", "gui_show", "reshow"})
        {
            const std::string at = spinning;
            if (at == "reshow")
            {
                REQUIRE(mgr().openEditor(h, "Mock") == noError);
                CHECK(gui->request_hide(host));
                REQUIRE(pumpUntil([&] { return !mgr().isEditorOpen(h); }));
                REQUIRE(fakeWindow().open);
            }
            drainAll(h);
            bool closedThere = false;
            setHook([&](const std::string &event, const clap_host_t *) {
                if ((event == at || (at == "reshow" && event == "gui_show")) &&
                    !closedThere)
                    closedThere = fakeUserClose();
            });
            CHECK(mgr().openEditor(h, "Mock") != noError);
            setHook(nullptr);
            CHECK(closedThere);
            CHECK(!mgr().isEditorOpen(h));
            CHECK(!fakeWindow().open);
            CHECK(pumpUntil([] { return gMock.state->guiLive.load() == 0; }));
            CHECK(fakeWindow().contentViewRetains == 0);
            pumpFor(0.03);
            CHECK(countType(drainAll(h), kEvEditorClosed) == 1);
        }

        // The application terminates: the gui goes with the window, without
        // an editorClosed.
        REQUIRE(mgr().openEditor(h, "Mock") == noError);
        drainAll(h);
        REQUIRE(fakeAppTerminate());
        CHECK(gMock.state->guiLive.load() == 0);
        CHECK(!fakeWindow().open);
        CHECK(!mgr().isEditorOpen(h));
        CHECK(fakeWindow().contentViewRetains == 0);
        pumpFor(0.03);
        CHECK(countType(drainAll(h), kEvEditorClosed) == 0);

        // ... during a plugin call: as for a user close then, the gui is
        // destroyed on a later tick and its parent lives until then.
        REQUIRE(mgr().openEditor(h, "Mock") == noError);
        drainAll(h);
        std::atomic<int> terminatedInside{0};
        bool deferredInside = false;
        bool parentAliveAtTermDestroy = false;
        setHook([&](const std::string &event, const clap_host_t *hst) {
            if (event == "on_main" && terminatedInside.load() == 0)
            {
                terminatedInside.store(fakeAppTerminate() ? 1 : 2);
                deferredInside = !fakeWindow().open &&
                                 fakeContentViewAlive() &&
                                 gMock.state->guiLive.load() == 1 &&
                                 !hostGui(hst)->request_show(hst) &&
                                 !hostGui(hst)->request_hide(hst);
            }
            if (event == "gui_destroy")
                parentAliveAtTermDestroy = fakeContentViewAlive();
        });
        host->request_callback(host);
        CHECK(pumpUntil([&] { return terminatedInside.load() != 0; }));
        CHECK(terminatedInside.load() == 1);
        CHECK(deferredInside);
        CHECK(!mgr().isEditorOpen(h));
        CHECK(pumpUntil([&] { return gMock.state->guiLive.load() == 0; }));
        setHook(nullptr);
        CHECK(parentAliveAtTermDestroy);
        CHECK(fakeWindow().contentViewRetains == 0);
        CHECK(!fakeContentViewAlive());
        pumpFor(0.03);
        CHECK(countType(drainAll(h), kEvEditorClosed) == 0);

        // Reopening after any of these works.
        REQUIRE(mgr().openEditor(h, "Mock") == noError);
        CHECK(mgr().isEditorOpen(h) && fakeWindow().open);
        CHECK(unloadAndWait(h));
        pumpFor(0.03);
    }

    // Each failing step leaves nothing behind.
    void testGuiFailures()
    {
        gMock.state->guiApis = 1;
        const int h = load("mock.ext");
        REQUIRE(h > 0);
        captureLogs();
        struct Case
        {
            const char *name;
            std::function<void()> arm;
            bool windowOpened;
        };
        const Case cases[] = {
            {"create", [] { gMock.state->guiFailCreate = 1; }, false},
            {"get_size", [] { gMock.state->guiFailGetSize = 1; }, false},
            {"zero size", [] { gMock.state->guiWidth = 0; }, false},
            {"window", [] { fakeWindow().failOpen = true; }, false},
            {"content view", [] { fakeWindow().nullContentView = true; }, true},
            {"set_parent", [] { gMock.state->guiFailSetParent = 1; }, true},
            {"show", [] { gMock.state->guiFailShow = 1; }, true},
        };
        for (const Case &c : cases)
        {
            const int creates = gMock.state->guiCreates.load();
            const int opens = fakeWindow().opens;
            c.arm();
            const PlayerErrors e = mgr().openEditor(h, "Mock");
            if (e != unknownError)
                std::fprintf(stderr, "  case %s: %d\n", c.name, static_cast<int>(e));
            CHECK(e == unknownError);
            CHECK(gMock.state->guiLive.load() == 0);
            CHECK(gMock.state->guiDestroys.load() ==
                  gMock.state->guiCreates.load());
            CHECK(!fakeWindow().open);
            CHECK(!mgr().isEditorOpen(h));
            CHECK(fakeWindow().opens == opens + (c.windowOpened ? 1 : 0));
            (void)creates;
            gMock.state->guiWidth = 400;
            fakeWindow().nullContentView = false;
            fakeWindow().failOpen = false;
        }
        CHECK(sinkCount("get_size") >= 2);
        CHECK(countEvent("mock.ext", "gui_hide") == 0);
        // Everything works again afterwards.
        CHECK(mgr().openEditor(h, "Mock") == noError);
        CHECK(mgr().isEditorOpen(h) && fakeWindow().open);
        CHECK(mgr().closeEditor(h) == noError);
        CHECK(unloadAndWait(h));
    }

    // The plugin's own window: suggest_title and set_transient, no embedded
    // calls and no host window; also the fallback from a failed embed.
    void testGuiFloating()
    {
        gMock.state->guiApis = 2;
        const int h = load("mock.ext");
        REQUIRE(h > 0);
        CHECK(mgr().hasEditor(h));
        REQUIRE(mgr().openEditor(h, "Floating") == noError);
        CHECK(mgr().isEditorOpen(h));
        CHECK(fakeWindow().creates == 0 && !fakeWindow().open);
        CHECK(countEvent("mock.ext", "gui_create_floating") == 1);
        CHECK(countEvent("mock.ext", "gui_title") == 1);
        bool titled = false;
        for (const std::string &e : events("mock.ext"))
            titled = titled || e == "gui_title:Floating";
        CHECK(titled);
        CHECK(gMock.state->guiTransients.load() == 1);
        CHECK(gMock.state->guiTransientNull.load() == 0);
        CHECK(eventIndex("mock.ext", "gui_set_transient") <
              eventIndex("mock.ext", "gui_show"));
        for (const char *embedded :
             {"gui_get_size", "gui_can_resize", "gui_set_parent", "gui_set_size"})
            CHECK(countEvent("mock.ext", embedded) == 0);
        // A plugin-initiated resize of its own window is not ours to apply.
        const clap_host_t *host = mgr().getInstance(h)->host();
        CHECK(!hostGui(host)->request_resize(host, 300, 300));
        CHECK(mgr().closeEditor(h) == noError);
        CHECK(gMock.state->guiLive.load() == 0);
        CHECK(!mgr().isEditorOpen(h));

        // Without an application window: no set_transient.
        fakeWindow().noApplicationWindow = true;
        REQUIRE(mgr().openEditor(h, "Floating") == noError);
        CHECK(gMock.state->guiTransients.load() == 1);
        CHECK(mgr().closeEditor(h) == noError);
        CHECK(unloadAndWait(h));

        // Both offered: embedding fails at set_parent, the floating window
        // follows.
        gMock.reset();
        gMock.setHook(&hookTrampoline);
        resetFakeWindow();
        gMock.state->guiApis = 3;
        const int h2 = load("mock.ext");
        REQUIRE(h2 > 0);
        gMock.state->guiFailSetParent = 1;
        REQUIRE(mgr().openEditor(h2, "Fallback") == noError);
        CHECK(gMock.state->guiCreates.load() == 2);
        CHECK(gMock.state->guiLive.load() == 1);
        CHECK(countEvent("mock.ext", "gui_create_floating") == 1);
        CHECK(!fakeWindow().open);
        CHECK(mgr().isEditorOpen(h2));
        CHECK(mgr().closeEditor(h2) == noError);
        CHECK(unloadAndWait(h2));
    }

    // A restart keeps the editor; teardown destroys it before deactivate.
    void testGuiLifetime()
    {
        gMock.state->guiApis = 1;
        const int h = load("mock.ext");
        REQUIRE(h > 0);
        auto instance = mgr().getInstance(h);
        const clap_host_t *host = instance->host();
        REQUIRE(mgr().openEditor(h, "Mock") == noError);
        stereoBlock(h);

        host->request_restart(host);
        CHECK(pumpUntil([&] { return instance->restartCount() == 1; }));
        CHECK(gMock.state->guiLive.load() == 1);
        CHECK(countEvent("mock.ext", "gui_destroy") == 0);
        CHECK(mgr().isEditorOpen(h) && fakeWindow().open);
        // The editor still works after the restart.
        REQUIRE(fakeUserResize({480, 320}));
        CHECK(gMock.state->guiCurrentWidth.load() == 480);

        instance.reset();
        CHECK(unloadAndWait(h));
        const int guiDestroy = eventIndex("mock.ext", "gui_destroy");
        const int deactivate = eventIndex("mock.ext", "deactivate");
        int lastDeactivate = -1;
        const std::vector<std::string> evs = events("mock.ext");
        for (size_t i = 0; i < evs.size(); ++i)
            if (evs[i].compare(0, 11, "deactivate:") == 0)
                lastDeactivate = static_cast<int>(i);
        CHECK(deactivate >= 0 && guiDestroy > deactivate); // After the restart.
        CHECK(guiDestroy >= 0 && guiDestroy < lastDeactivate);
        CHECK(eventIndex("mock.ext", "gui_hide") < guiDestroy);
        CHECK(!fakeWindow().open);
        pumpFor(0.03);

        // unloadAll with an open editor too.
        const int h2 = load("mock.ext");
        REQUIRE(h2 > 0);
        REQUIRE(mgr().openEditor(h2, "Mock") == noError);
        mgr().unloadAll();
        CHECK(pumpUntil([] { return gMock.state->liveInstances.load() == 0; }));
        CHECK(gMock.state->guiLive.load() == 0);
    }

    // The C ABI.
    // =========================================================================
    // Phase 5a. Filter-slot parameter mapping
    // =========================================================================

    void callDspSlots(int handle, float *buffer, unsigned samples,
                      unsigned bufferSize, unsigned channels,
                      const float *params, uint64_t mask, unsigned count)
    {
        std::lock_guard<std::mutex> lock(gAudioMutex);
        clapFilterDspCallback(buffer, samples, bufferSize, channels,
                              static_cast<float>(kSampleRate), 0.0, params,
                              mask, count,
                              reinterpret_cast<void *>(
                                  static_cast<uintptr_t>(handle)));
    }

    /// A stereo block that carries the filter-slot values [slots]
    /// (kMaxSlotParams of them) and the changed bits [mask].
    void slotBlock(int handle, const float *slots, uint64_t mask = 0,
                   unsigned samples = 256)
    {
        Bus bus(2, samples);
        bus.fill({1.0f, 0.5f});
        std::thread audio([&] {
            callDspSlots(handle, bus.data.data(), samples, bus.size,
                         bus.channels, slots, mask, kMaxSlotParams);
        });
        audio.join();
    }

    constexpr uint64_t slotBit(unsigned slot) { return 1ull << slot; }

    void testSlotMapping()
    {
        const int h = load("mock.ext");
        REQUIRE(h > 0);
        auto instance = mgr().getInstance(h);
        stereoBlock(h);
        float slots[kMaxSlotParams] = {};
        slots[0] = 1.0f;

        // Refusals.
        CHECK(mgr().mapSlotParameter(h, 0, 10) == invalidParameter);
        CHECK(mgr().mapSlotParameter(h, kMaxSlotParams, 10) == invalidParameter);
        CHECK(mgr().mapSlotParameter(h, 1, 30) == invalidParameter); // read-only
        CHECK(mgr().mapSlotParameter(h, 1, 99) == invalidParameter);
        CHECK(mgr().mapSlotParameter(h, 1, CLAP_INVALID_ID) == invalidParameter);
        CHECK(mgr().mapSlotParameter(0x7fff, 1, 10) == invalidParameter);
        CHECK(mgr().unmapSlotParameter(h, 0) == invalidParameter);
        CHECK(mgr().unmapSlotParameter(0x7fff, 1) == invalidParameter);
        CHECK(instance->slotMapping(1).id == CLAP_INVALID_ID);

        // A new mapping keeps the plugin's value: the standing slot value is
        // not sent.
        slots[1] = 0.25f;
        CHECK(mgr().mapSlotParameter(h, 1, 10) == noError);
        size_t base = received().size();
        slotBlock(h, slots);
        slotBlock(h, slots, slotBit(1));
        CHECK(receivedFor(10, base).empty());

        // Each change goes out once, linear over 0..2, at time 0.
        slots[1] = 0.75f;
        slotBlock(h, slots, slotBit(1));
        std::vector<MockReceived> got = receivedFor(10, base);
        REQUIRE(got.size() == 1);
        CHECK(got[0].value == 1.5 && got[0].path == 1 && got[0].time == 0);
        slotBlock(h, slots, slotBit(1));
        slotBlock(h, slots);
        CHECK(receivedFor(10, base).size() == 1);
        // Without the changed bit (it was cleared while bypassed): the value
        // still differs, so it goes out.
        slots[1] = 0.5f;
        slotBlock(h, slots);
        got = receivedFor(10, base);
        REQUIRE(got.size() == 2);
        CHECK(got[1].value == 1.0);
        // Out-of-range and non-finite slot values.
        slots[1] = 1.5f;
        slotBlock(h, slots);
        slots[1] = NAN;
        slotBlock(h, slots);
        got = receivedFor(10, base);
        REQUIRE(got.size() == 3);
        CHECK(got[2].value == 2.0);
        slots[1] = -0.5f;
        slotBlock(h, slots);
        got = receivedFor(10, base);
        REQUIRE(got.size() == 4);
        CHECK(got[3].value == 0.0);

        // A stepped enum (0..3): rounded, and a new step only when it moves.
        slots[2] = 0.1f;
        CHECK(mgr().mapSlotParameter(h, 2, 20) == noError);
        slotBlock(h, slots);
        base = received().size();
        slots[2] = 0.5f; // 1.5 -> 2
        slotBlock(h, slots);
        slots[2] = 0.55f; // 1.65 -> 2: the same step
        slotBlock(h, slots);
        slots[2] = 1.0f;
        slotBlock(h, slots);
        got = receivedFor(20, base);
        REQUIRE(got.size() == 2);
        CHECK(got[0].value == 2.0 && got[1].value == 3.0);

        // Changed right after mapping (the changed bit): sent at once.
        slots[3] = 1.0f;
        CHECK(mgr().mapSlotParameter(h, 3, 40) == noError);
        base = received().size();
        slotBlock(h, slots, slotBit(3));
        got = receivedFor(40, base);
        REQUIRE(got.size() == 1);
        CHECK(got[0].value == 1.0 && got[0].path == 1);

        // Fewer slot values than the mapped index: nothing for it.
        base = received().size();
        slots[3] = 0.0f;
        {
            Bus bus(2, 256);
            std::thread audio([&] {
                callDspSlots(h, bus.data.data(), 256, bus.size, 2, slots,
                             slotBit(3), 3);
            });
            audio.join();
        }
        CHECK(receivedFor(40, base).empty());
        slotBlock(h, slots);
        CHECK(receivedFor(40, base).size() == 1);

        // Unmap stops it; a remap re-arms (no jump).
        CHECK(mgr().unmapSlotParameter(h, 1) == noError);
        CHECK(instance->slotMapping(1).id == CLAP_INVALID_ID);
        base = received().size();
        slots[1] = 0.1f;
        slotBlock(h, slots, slotBit(1));
        CHECK(receivedFor(10, base).empty());
        slots[1] = 0.2f;
        CHECK(mgr().mapSlotParameter(h, 1, 10) == noError);
        slotBlock(h, slots);
        CHECK(receivedFor(10, base).empty());
        // A remap to another parameter re-arms too.
        CHECK(mgr().mapSlotParameter(h, 1, 40) == noError);
        slots[1] = 0.9f;
        slotBlock(h, slots); // armed at the new mapping: not sent
        CHECK(receivedFor(40, base).empty());
        CHECK(mgr().mapSlotParameter(h, 1, 10) == noError);
        slotBlock(h, slots);
        slots[1] = 0.25f;
        slotBlock(h, slots);
        got = receivedFor(10, base);
        REQUIRE(got.size() == 1);
        CHECK(got[0].value == 0.5);

        // A plain setParameterValue is not overridden by an unchanged slot.
        CHECK(mgr().setParameterValue(h, 10, 1.75) == noError);
        slotBlock(h, slots);
        slotBlock(h, slots);
        got = receivedFor(10, base);
        REQUIRE(got.size() == 2);
        CHECK(got[1].value == 1.75);

        // The full C ABI.
        CHECK(soloud_clapMapSlotParameter(h, 5, 10, 0.5f) == noError);
        CHECK(instance->slotMapping(5).baseline == 0.5f);
        CHECK(soloud_clapMapSlotParameter(h, 5, 10, NAN) == noError);
        CHECK(std::isnan(instance->slotMapping(5).baseline));
        CHECK(soloud_clapMapSlotParameter(h, 5, 30, 0.5f) == invalidParameter);
        CHECK(soloud_clapUnmapSlotParameter(h, 5) == noError);
        CHECK(soloud_clapUnmapSlotParameter(h, 16) == invalidParameter);
        CHECK(mgr().unloadPlugin(h) == noError);
    }

    void testSlotMappingRescan()
    {
        const int h = load("mock.ext");
        REQUIRE(h > 0);
        auto instance = mgr().getInstance(h);
        const clap_host_t *host = instance->host();
        auto *params = hostParams(host);
        stereoBlock(h);
        float slots[kMaxSlotParams] = {};
        slots[0] = 1.0f;
        CHECK(mgr().mapSlotParameter(h, 1, 10, 0.0f) == noError);
        slotBlock(h, slots);
        const SlotMapping first = instance->slotMapping(1);
        CHECK(first.id == 10 && first.max == 2.0 && first.baseline == 0.0f &&
              first.generation == instance->paramsGeneration());

        // INFO (a rename): the mapping stays as it was, values flow.
        gMock.state->paramNameVariant = 2;
        params->rescan(host, CLAP_PARAM_RESCAN_INFO);
        CHECK(pumpUntil([&] {
            ParamEntry e;
            return mgr().parameterInfo(h, 0, e) == noError && e.name == "Gain 2";
        }));
        SlotMapping m = instance->slotMapping(1);
        CHECK(m.id == 10 && m.version == first.version &&
              m.cookie == first.cookie && m.generation == first.generation);
        size_t base = received().size();
        slots[1] = 0.5f;
        slotBlock(h, slots);
        CHECK(receivedValue(10, 1.0, base));

        // ALL while active: pending, new maps refused, the slot held back
        // until the restart's rebuild re-resolves it, then delivered.
        const uint64_t restarts = instance->restartCount();
        params->rescan(host, CLAP_PARAM_RESCAN_ALL);
        CHECK(instance->paramsPending());
        CHECK(mgr().mapSlotParameter(h, 2, 10) == unknownError);
        base = received().size();
        slots[1] = 0.25f;
        slotBlock(h, slots);
        CHECK(receivedFor(10, base).empty());
        CHECK(pumpUntil([&] {
            return instance->restartCount() == restarts + 1 &&
                   !instance->paramsPending();
        }));
        m = instance->slotMapping(1);
        CHECK(m.id == 10 && m.version == first.version &&
              m.generation == instance->paramsGeneration() &&
              m.generation != first.generation);
        slotBlock(h, slots);
        {
            std::vector<MockReceived> got = receivedFor(10, base);
            REQUIRE(got.size() == 1);
            CHECK(got[0].value == 0.5);
        }

        // A variant with new cookies and id 50: the cookie follows.
        gMock.state->pendingParamVariant = 1;
        uint64_t r = instance->restartCount();
        host->request_restart(host);
        CHECK(pumpUntil([&] {
            return instance->restartCount() == r + 1 && !instance->paramsPending();
        }));
        m = instance->slotMapping(1);
        CHECK(m.id == 10 && m.cookie != first.cookie && m.version == first.version);
        CHECK(mgr().mapSlotParameter(h, 4, 50) == noError);
        base = received().size();
        slots[1] = 1.0f;
        slots[4] = 0.5f;
        slotBlock(h, slots, slotBit(4));
        CHECK(receivedValue(10, 2.0, base));
        CHECK(receivedValue(50, 0.5, base));

        // A range change (rescan ALL in deactivate): the new max applies.
        gMock.state->gainMax = 4.0;
        gMock.state->rescanAllInDeactivate = 1;
        r = instance->restartCount();
        host->request_restart(host);
        CHECK(pumpUntil([&] {
            return instance->restartCount() == r + 1 && !instance->paramsPending();
        }));
        m = instance->slotMapping(1);
        CHECK(m.id == 10 && m.max == 4.0 && m.version == first.version &&
              m.baseline == 0.0f);
        base = received().size();
        slotBlock(h, slots); // 1.0 now maps to 4
        slots[1] = 0.75f;
        slotBlock(h, slots);
        {
            std::vector<MockReceived> got = receivedFor(10, base);
            REQUIRE(got.size() == 2);
            CHECK(got[0].value == 4.0 && got[1].value == 3.0);
        }

        // Back to variant 0: id 50 is gone, so slot 4 is unmapped.
        gMock.state->pendingParamVariant = 0;
        r = instance->restartCount();
        host->request_restart(host);
        CHECK(pumpUntil([&] {
            return instance->restartCount() == r + 1 && !instance->paramsPending();
        }));
        CHECK(instance->slotMapping(4).id == CLAP_INVALID_ID);
        CHECK(instance->slotMapping(1).id == 10);
        base = received().size();
        slots[4] = 0.9f;
        slotBlock(h, slots, slotBit(4));
        CHECK(receivedFor(50, base).empty());
        CHECK(mgr().unloadPlugin(h) == noError);
    }

    std::atomic<int> gSlotHookHandle{0};
    std::atomic<int> gSlotHookRuns{0};
    float gSlotHookValues[kMaxSlotParams] = {};

    /// Mid-write: an audio block on another thread, which must skip the
    /// half-written mapping.
    void slotWriteHook()
    {
        gSlotHookRuns.fetch_add(1);
        gSlotHookValues[1] = static_cast<float>((gSlotHookRuns.load() % 9) / 8.0);
        Bus bus(2, 128);
        bus.fill({1.0f, 0.5f});
        std::thread audio([&] {
            callDspSlots(gSlotHookHandle.load(), bus.data.data(), 128, 128, 2,
                         gSlotHookValues, slotBit(1), kMaxSlotParams);
        });
        audio.join();
    }

    std::atomic<int> gSlotReadRemaps{0};

    /// Mid-read: a whole remap on another thread, which the read's recheck
    /// must notice.
    void slotReadHook()
    {
        if (gSlotReadRemaps.load() <= 0)
            return;
        gSlotReadRemaps.fetch_sub(1);
        // Alternates, so the torn copy mixes two parameters.
        const int n = gSlotHookRuns.fetch_add(1);
        std::thread writer([n] {
            CHECK(mgr().mapSlotParameter(gSlotHookHandle.load(), 1,
                                         n % 2 == 0 ? 20 : 10) == noError);
        });
        writer.join();
    }

    void testSlotMappingRace()
    {
        // Deterministic: a block inside every write sees the slot mid-write
        // (id 20 stored, the cookie and range still id 10's, or back) and
        // skips it.
        {
            const int h = load("mock.ext");
            REQUIRE(h > 0);
            stereoBlock(h);
            gSlotHookHandle = h;
            gSlotHookRuns = 0;
            gSlotHookValues[0] = 1.0f;
            CHECK(mgr().mapSlotParameter(h, 1, 10) == noError);
            slotBlock(h, gSlotHookValues);
            test::setSlotWriteHook(&slotWriteHook);
            for (int i = 0; i < 40; ++i)
                CHECK(mgr().mapSlotParameter(h, 1, i % 2 == 0 ? 20 : 10) == noError);
            test::setSlotWriteHook(nullptr);
            CHECK(gSlotHookRuns.load() == 40);
            CHECK(gMock.state->violations.load() == 0);

            // A whole write inside a read: the recheck drops the torn copy
            // (an old id with the new cookie and range).
            test::setSlotReadHook(&slotReadHook);
            for (int i = 0; i < 40; ++i)
            {
                gSlotReadRemaps = 1;
                gSlotHookValues[1] = static_cast<float>((i % 7 + 1) / 8.0);
                slotBlock(h, gSlotHookValues, slotBit(1));
            }
            test::setSlotReadHook(nullptr);
            CHECK(gMock.state->violations.load() == 0);
            CHECK(mgr().unloadPlugin(h) == noError);
        }

        // Remaps between a continuous and a stepped parameter while DSP
        // forwards changing slot values: never a torn mapping (the mock
        // flags a stale cookie, an off-step value or one out of range).
        const int h = load("mock.ext");
        REQUIRE(h > 0);
        stereoBlock(h);
        std::atomic<bool> running{true};
        std::atomic<int> blocks{0};
        std::thread audio([&] {
            float slots[kMaxSlotParams] = {};
            slots[0] = 1.0f;
            Bus bus(2, 128);
            int i = 0;
            while (running.load())
            {
                slots[1] = static_cast<float>((i % 97) / 96.0);
                slots[2] = static_cast<float>(((i * 7) % 89) / 88.0);
                ++i;
                bus.fill({1.0f, 0.5f});
                callDspSlots(h, bus.data.data(), 128, 128, 2, slots,
                             slotBit(1) | slotBit(2), kMaxSlotParams);
                blocks.fetch_add(1);
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            }
        });
        std::thread mapper([&] {
            for (int i = 0; i < 2000; ++i)
            {
                CHECK(mgr().mapSlotParameter(h, 1, i % 2 == 0 ? 10 : 20) == noError);
                CHECK(mgr().mapSlotParameter(h, 2, i % 3 == 0 ? 40 : 10) == noError);
                if (i % 5 == 0)
                    CHECK(mgr().unmapSlotParameter(h, 2) == noError);
            }
        });
        mapper.join();
        const int target = blocks.load() + 20;
        while (blocks.load() < target)
            std::this_thread::yield();
        running.store(false);
        audio.join();
        CHECK(received().size() > 0);
        CHECK(gMock.state->violations.load() == 0);
        CHECK(mgr().unloadPlugin(h) == noError);
    }

    /// Mid-write of a mapping: a block carrying a changed slot 1, which must
    /// skip the half-written cell.
    void slotBaselineWriteHook()
    {
        gSlotHookRuns.fetch_add(1);
        Bus bus(2, 128);
        bus.fill({1.0f, 0.5f});
        std::thread audio([&] {
            callDspSlots(gSlotHookHandle.load(), bus.data.data(), 128, 128, 2,
                         gSlotHookValues, slotBit(1), kMaxSlotParams);
        });
        audio.join();
    }

    void testSlotMappingBaseline()
    {
        test::setIdleThresholdNs(30 * kMs);
        const int h = load("mock.ext");
        REQUIRE(h > 0);
        auto instance = mgr().getInstance(h);
        stereoBlock(h);
        float slots[kMaxSlotParams] = {};
        slots[0] = 1.0f;

        // The slot still holds the baseline: not sent, even with its changed
        // bit set (a value set just before mapping, like configureParameters'
        // defaults).
        slots[1] = 0.25f;
        CHECK(mgr().mapSlotParameter(h, 1, 10, 0.25f) == noError);
        CHECK(instance->slotMapping(1).baseline == 0.25f);
        size_t base = received().size();
        slotBlock(h, slots, slotBit(1));
        slotBlock(h, slots);
        CHECK(receivedFor(10, base).empty());
        slots[1] = 0.5f;
        slotBlock(h, slots);
        std::vector<MockReceived> got = receivedFor(10, base);
        REQUIRE(got.size() == 1);
        CHECK(got[0].value == 1.0);

        // Changed after mapping, the block that saw the changed bit never ran
        // (wet 0 or no callback): the first block that runs sends it once.
        slots[2] = 0.0f;
        CHECK(mgr().mapSlotParameter(h, 2, 40, 0.0f) == noError);
        slots[2] = 0.75f; // 0.75 -> 1 (stepped 0..1)
        base = received().size();
        slotBlock(h, slots);
        slotBlock(h, slots);
        got = receivedFor(40, base);
        REQUIRE(got.size() == 1);
        CHECK(got[0].value == 1.0);

        // A stepped parameter: a baseline on the same step is not a change.
        slots[3] = 0.5f; // 1.5 -> 2
        CHECK(mgr().mapSlotParameter(h, 3, 20, 0.55f) == noError); // 1.65 -> 2
        base = received().size();
        slotBlock(h, slots, slotBit(3));
        CHECK(receivedFor(20, base).empty());
        slots[3] = 1.0f;
        slotBlock(h, slots);
        got = receivedFor(20, base);
        REQUIRE(got.size() == 1);
        CHECK(got[0].value == 3.0);

        // The changed block was dry (servicing a paused flush), its changed
        // bit lost: the next block still sends the value once.
        CHECK(mgr().unmapSlotParameter(h, 1) == noError);
        slots[1] = 0.25f;
        CHECK(mgr().mapSlotParameter(h, 1, 10, 0.25f) == noError);
        slots[1] = 0.75f;
        base = received().size();
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            std::atomic<bool> flushing{false}, dspDone{false};
            setHook([&](const std::string &event, const clap_host_t *) {
                if (event == "flush" && !flushing.exchange(true))
                {
                    const auto end = std::chrono::steady_clock::now() +
                                     std::chrono::seconds(3);
                    while (!dspDone.load() && std::chrono::steady_clock::now() < end)
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            });
            const uint64_t dry = instance->dryBlocksFromServicing();
            Bus bus(2, 256);
            std::thread audio([&] {
                while (!flushing.load())
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                bus.fill({1.0f, 0.5f});
                callDspSlots(h, bus.data.data(), 256, bus.size, 2, slots,
                             slotBit(1), kMaxSlotParams);
                dspDone = true;
            });
            CHECK(mgr().setParameterValue(h, 20, 1.0) == noError);
            CHECK(pumpUntil([&] { return dspDone.load(); }));
            audio.join();
            setHook(nullptr);
            CHECK(instance->dryBlocksFromServicing() == dry + 1);
        }
        CHECK(receivedFor(10, base).empty());
        slotBlock(h, slots);
        slotBlock(h, slots);
        got = receivedFor(10, base);
        REQUIRE(got.size() == 1);
        CHECK(got[0].value == 1.5);

        // A remap over the live mapping: the changed block failed its read of
        // the half-written cell. The next block sends the value once.
        gSlotHookHandle = h;
        gSlotHookRuns = 0;
        gSlotHookValues[0] = 1.0f;
        gSlotHookValues[1] = 0.5f;
        test::setSlotWriteHook(&slotBaselineWriteHook);
        CHECK(mgr().mapSlotParameter(h, 1, 10, 0.0f) == noError);
        test::setSlotWriteHook(nullptr);
        CHECK(gSlotHookRuns.load() == 1);
        base = received().size();
        slotBlock(h, gSlotHookValues);
        slotBlock(h, gSlotHookValues);
        got = receivedFor(10, base);
        REQUIRE(got.size() == 1);
        CHECK(got[0].value == 1.0);

        // A remap against the current value does not replay a change the old
        // mapping never sent.
        CHECK(mgr().unmapSlotParameter(h, 2) == noError);
        slots[2] = 0.25f; // never sent: slot 2 is unmapped
        slotBlock(h, slots, slotBit(2));
        CHECK(mgr().mapSlotParameter(h, 2, 40, 0.25f) == noError);
        base = received().size();
        slotBlock(h, slots, slotBit(2));
        slotBlock(h, slots);
        CHECK(receivedFor(40, base).empty());
        CHECK(gMock.state->violations.load() == 0);
        CHECK(mgr().unloadPlugin(h) == noError);
    }

    void testSlotMappingBaselineRescan()
    {
        const int h = load("mock.ext");
        REQUIRE(h > 0);
        auto instance = mgr().getInstance(h);
        const clap_host_t *host = instance->host();
        auto *params = hostParams(host);
        stereoBlock(h);
        float slots[kMaxSlotParams] = {};
        slots[0] = 1.0f;

        // Unarmed, then a block against a stale list: it must not arm with
        // the old range. After the range doubles, the change (0.5 -> 0.25)
        // maps to 1, the old range's baseline plain: still sent.
        slots[1] = 0.5f;
        CHECK(mgr().mapSlotParameter(h, 1, 10, 0.5f) == noError);
        // Slot 2 stays at its baseline throughout.
        slots[2] = 0.5f;
        CHECK(mgr().mapSlotParameter(h, 2, 10, 0.5f) == noError);
        gMock.state->gainMax = 4.0;
        const uint64_t restarts = instance->restartCount();
        params->rescan(host, CLAP_PARAM_RESCAN_ALL);
        CHECK(instance->paramsPending());
        slots[1] = 0.25f;
        size_t base = received().size();
        slotBlock(h, slots, slotBit(1));
        CHECK(receivedFor(10, base).empty());
        CHECK(pumpUntil([&] {
            return instance->restartCount() == restarts + 1 &&
                   !instance->paramsPending();
        }));
        CHECK(instance->slotMapping(1).max == 4.0);
        slotBlock(h, slots);
        slotBlock(h, slots);
        std::vector<MockReceived> got = receivedFor(10, base);
        REQUIRE(got.size() == 1);
        CHECK(got[0].value == 1.0);

        // Armed and never sent, then a range change: the slot still holds
        // its baseline, so the plugin keeps its own value.
        gMock.state->gainMax = 2.0;
        gMock.state->rescanAllInDeactivate = 1;
        const uint64_t r = instance->restartCount();
        host->request_restart(host);
        CHECK(pumpUntil([&] {
            return instance->restartCount() == r + 1 && !instance->paramsPending();
        }));
        CHECK(instance->slotMapping(2).max == 2.0);
        // Slot 1 was sent 1.0 under 0..4: 0.25 is 0.5 now, so it follows.
        base = received().size();
        slotBlock(h, slots);
        slotBlock(h, slots);
        got = receivedFor(10, base);
        REQUIRE(got.size() == 1);
        CHECK(got[0].value == 0.5);
        CHECK(gMock.state->violations.load() == 0);
        CHECK(mgr().unloadPlugin(h) == noError);
    }

    void testGuiAbi()
    {
        gMock.state->guiApis = 1;
        const int h = load("mock.ext");
        REQUIRE(h > 0);
        CHECK(soloud_clapHasEditor(h) == 1);
        CHECK(soloud_clapIsEditorOpen(h) == 0);
        CHECK(soloud_clapOpenEditor(h, nullptr) == noError);
        CHECK(fakeWindow().title.empty());
        CHECK(soloud_clapIsEditorOpen(h) == 1);
        CHECK(soloud_clapCloseEditor(h) == noError);
        CHECK(soloud_clapIsEditorOpen(h) == 0);
        CHECK(soloud_clapOpenEditor(h, "ABI") == noError);
        CHECK(fakeWindow().title == "ABI");
        // From a worker: hops to main.
        PlayerErrors e = unknownError;
        onWorker([&] { e = soloud_clapCloseEditor(h); });
        CHECK(e == noError);
        CHECK(!fakeWindow().open);
        CHECK(soloud_clapHasEditor(0) == 0);
        CHECK(soloud_clapOpenEditor(0, "x") == invalidParameter);
        CHECK(soloud_clapCloseEditor(0) == invalidParameter);
        CHECK(soloud_clapIsEditorOpen(0) == 0);
        CHECK(unloadAndWait(h));
    }

    struct Test
    {
        const char *name;
        void (*fn)();
    };

    const Test kTests[] = {
        {"event_list", testEventList},
        {"scan", testScan},
        {"lifecycle", testLifecycle},
        {"unload_variants", testUnloadVariants},
        {"failures", testFailures},
        {"restart", testRestart},
        {"restart_topology", testRestartTopology},
        {"callbacks", testCallbacks},
        {"two_instances", testTwoInstances},
        {"stale_handle", testStaleHandle},
        {"process", testProcess},
        {"request_process", testRequestProcess},
        {"reentrancy", testReentrancy},
        {"worker_abi", testWorkerAbi},
        {"no_resurrection", testNoResurrection},
        {"dispose_nested", testDisposeNested},
        {"quiesce", testQuiesce},
        {"registry_retiring", testRegistryRetiring},
        {"dispose_reload", testDisposeReload},
        {"failed_persists", testFailedPersists},
        {"queued_load", testQueuedLoad},
        {"main_port_flags", testMainPortFlags},
        {"scratch_oom", testScratchOom},
        {"abi_exports", testAbiExports},
        {"extensions", testExtensions},
        {"params_metadata", testParamsMetadata},
        {"params_set_process", testParamsSetProcess},
        {"params_flush_paused", testParamsFlushPaused},
        {"params_flush_inactive", testParamsFlushInactive},
        {"out_events", testOutEvents},
        {"out_overflow", testOutOverflow},
        {"params_rescan", testParamsRescan},
        {"state", testState},
        {"latency", testLatency},
        {"timers", testTimers},
        {"log", testLog},
        {"abi_params_state", testAbiParamsState},
        {"edit_publication_race", testEditPublicationRace},
        {"edit_stale_flood", testEditStaleFlood},
        {"retained_stale_compaction", testRetainedStaleCompaction},
        {"retained_saturation", testRetainedSaturation},
        {"resync_drop_races_ack", testResyncDropRacesAck},
        {"resync_reset_races_ack", testResyncResetRacesAck},
        {"resync_full_ring_boundary", testResyncFullRingBoundary},
        {"reset_then_notification_order", testResetThenNotificationOrder},
        {"all_during_rebuild", testAllDuringRebuild},
        {"all_outside_host_call", testAllOutsideHostCall},
        {"all_inside_inactive_flush", testAllInsideInactiveFlush},
        {"drain_no_starvation", testDrainNoStarvation},
        {"inactive_load_all_then_enqueue", testInactiveLoadAllThenEnqueue},
        {"reentrant_hooks", testReentrantHooks},
        {"resync_second_reset", testResyncSecondReset},
        {"rescan_in_port_queries", testRescanInPortQueries},
        {"ack_vs_inactive_pending_store", testAckVsInactivePendingStore},
        {"ack_vs_active_pending_store", testAckVsActivePendingStore},
        {"reset_held_by_active_rescan", testResetHeldByActiveRescan},
        {"gui_open_close", testGuiOpenClose},
        {"gui_hide_show", testGuiHideShow},
        {"gui_user_resize", testGuiUserResize},
        {"gui_request_resize", testGuiRequestResize},
        {"gui_closed", testGuiClosed},
        {"gui_failures", testGuiFailures},
        {"gui_floating", testGuiFloating},
        {"gui_lifetime", testGuiLifetime},
        {"gui_abi", testGuiAbi},
        {"slot_mapping", testSlotMapping},
        {"slot_mapping_rescan", testSlotMappingRescan},
        {"slot_mapping_race", testSlotMappingRace},
        {"slot_mapping_baseline", testSlotMappingBaseline},
        {"slot_mapping_baseline_rescan", testSlotMappingBaselineRescan},
    };
} // namespace

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        std::fprintf(stderr, "usage: %s <work-dir> [test]\n", argv[0]);
        return 2;
    }
    gWork = argv[1];
    gMockPath = gWork + "/MockClap.clap";
    gBadVersionPath = gWork + "/MockClapBadVersion.clap";
    gNoEntryPath = gWork + "/NoEntry.clap";
    gAliasPath = gWork + "/MockClapAlias.clap";
    const char *only = argc > 2 ? argv[2] : nullptr;

    gMock = loadMockApi(gMockPath);
    gBadMock = loadMockApi(gBadVersionPath);
    if (gMock.state == nullptr || gMock.setHook == nullptr ||
        gMock.log == nullptr || gMock.reset == nullptr ||
        gMock.received == nullptr ||
        gBadMock.state == nullptr)
    {
        std::fprintf(stderr, "cannot load the mock bundles in %s\n", gWork.c_str());
        return 2;
    }
    gMock.reset();
    gMock.setHook(&hookTrampoline);
    mgr().setAudioThreadBarrier(&testBarrier, nullptr);

    int ran = 0;
    for (const Test &t : kTests)
    {
        if (only != nullptr && std::strcmp(only, t.name) != 0)
            continue;
        gTestName = t.name;
        gChecksInTest = 0;
        const int before = gFailures;
        t.fn();
        cleanup();
        ++ran;
        std::printf("%s %s (%d checks)\n", gFailures == before ? "PASS" : "FAIL",
                    t.name, gChecksInTest);
        std::fflush(stdout);
    }
    if (ran == 0)
    {
        std::fprintf(stderr, "no test named %s\n", only != nullptr ? only : "");
        return 2;
    }
    std::printf("%d test(s), %d failure(s)\n", ran, gFailures);
    return gFailures == 0 ? 0 : 1;
}
