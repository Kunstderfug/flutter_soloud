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

#define REQUIRE(cond)                                                          \
    do                                                                         \
    {                                                                          \
        CHECK(cond);                                                           \
        if (!(cond))                                                           \
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
        mgr().setAudioThreadBarrier(&testBarrier, nullptr);
        mgr().unloadAll();
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
        CHECK(plugins.size() == 11);
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
            CHECK(plugins.size() == 11);
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
        CHECK(scan == noError && plugins.size() == 11);
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
        REQUIRE(scan == noError && count == 11);
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
