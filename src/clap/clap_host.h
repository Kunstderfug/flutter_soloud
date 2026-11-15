// CLAP plugin host (macOS). Built only with SOLOUD_ENABLE_CLAP; see
// docs/clap_hosting_plan.md. The FFI surface is in src/clap_bindings.cpp.
//
// Ownership model (Phase 2):
// - Every plugin lifecycle call runs on the main thread. Entry points called
//   off main publish state under the manager mutex and then wait for main with
//   dispatch_sync_f, holding no lock.
// - DSP reaches an instance only through the lock-free handle table and its
//   atomic Disposition. Unloading clears the table, marks the instance Retired
//   and queues it in a retirement batch; the batch is torn down on main only
//   after the audio barrier has run (or the device is known to be stopped).
// - A 16 ms main-queue timer (the pump) services, per instance and in order:
//   request_restart, request_callback, a pending parameter rebuild, the
//   flush service and the plugin's timers; then it drains retirement batches.
//   It runs while anything is live (Phase 3 adds the last three steps).
#pragma once

#ifdef SOLOUD_ENABLE_CLAP

#include "../enums.h"
#include "clap_instance.h"
#include "clap_module.h"

#include <dispatch/dispatch.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace soloud_clap
{
    /// The CLAP version this host was built against (vendored headers).
    uint32_t hostClapVersionMajor();
    uint32_t hostClapVersionMinor();
    uint32_t hostClapVersionRevision();

    /// One plugin descriptor of a scanned bundle (copied strings).
    struct ScannedPlugin
    {
        std::string id;
        std::string name;
        std::string vendor;
        std::string version;
        /// The descriptor's features joined with ';'.
        std::string features;
    };

    class ClapHostManager
    {
    public:
        /// The process-wide manager (leaked, never destroyed).
        static ClapHostManager &instance();

        /// Lists the plugins of the bundle at [bundlePath]. Runs on main.
        PlayerErrors scanBundle(const char *bundlePath,
                                std::vector<ScannedPlugin> &out);

        /// Creates, inits and activates effect [pluginId] of [bundlePath].
        /// Runs on main. [channels] is the bus width it will run on (1..32);
        /// DSP adapts to the width of each call.
        PlayerErrors loadEffect(const char *bundlePath, const char *pluginId,
                                unsigned sampleRate, unsigned maxBlockSize,
                                unsigned channels, int *outHandle);

        /// Unloads [handle]: retires it, runs the audio barrier, then tears it
        /// down on main (later, from the pump, if it is in a lifecycle call).
        PlayerErrors unloadPlugin(int handle);

        /// Unloads every instance and releases the module cache, so each
        /// module's deinit() runs after its last destroy().
        void unloadAll();

        /// For engine dispose, under the engine locks with the device stopped:
        /// retires everything, makes no plugin call and runs no barrier, then
        /// posts the teardown to main asynchronously.
        void retireAllForDispose();

        /// The barrier unload and restart run before touching an instance that
        /// DSP may still be using. It returns once no audio callback that may
        /// have loaded an instance pointer from the table is still running.
        void setAudioThreadBarrier(void (*barrier)(void *), void *userData);

        /// Lock-free, for DSP: null for an unknown or stale handle.
        ClapInstance *getInstanceAudioThread(int handle) noexcept;

        // --- Phase 3: parameters, state, latency, events ---------------------
        // Any thread unless noted. An unknown or unloaded handle gives
        // invalidParameter (0 for the counts). Main hops return unknownError
        // when the instance is busy: called re-entrantly from inside one of its
        // own plugin calls.
        uint32_t latencySamples(int handle);
        /// 0 while the list is being rescanned.
        int parameterCount(int handle);
        PlayerErrors parameterInfo(int handle, int index, ParamEntry &out);
        /// Any non-audio thread; never waits for main.
        PlayerErrors setParameterValue(int handle, clap_id id, double value);
        // Main hops.
        PlayerErrors getParameterValue(int handle, clap_id id, double *outValue);
        PlayerErrors valueToText(int handle, clap_id id, double value,
                                 char *outText, int textLen);
        PlayerErrors textToValue(int handle, clap_id id, const char *text,
                                 double *outValue);
        PlayerErrors saveState(int handle, std::vector<unsigned char> &out);
        PlayerErrors loadState(int handle, const unsigned char *data,
                               size_t size);
        /// Any non-audio thread; never waits for main.
        PlayerErrors drainEvents(int handle, int capacity, int *outTypes,
                                 unsigned *outParamIds, double *outValues,
                                 unsigned *outFlags, int *outCount);

        // --- Phase 4b: plugin editor -----------------------------------------
        /// Any thread: whether the plugin offers a cocoa gui.
        bool hasEditor(int handle);
        /// Any thread: whether the editor is on screen.
        bool isEditorOpen(int handle);
        // Main hops. Opening an open editor brings it to the front.
        PlayerErrors openEditor(int handle, const std::string &title);
        PlayerErrors closeEditor(int handle);

        // --- introspection (tests) ------------------------------------------
        std::shared_ptr<ClapInstance> getInstance(int handle);
        size_t liveInstanceCount();
        size_t retiredBatchCount();
        size_t cachedModuleCount();
        /// Main only.
        bool pumpRunning() const noexcept { return mPump != nullptr; }

        /// A handle is `(generation << kSlotBits) | slot`, as for VST3.
        static constexpr unsigned int kSlotBits = 10;
        static constexpr size_t kTableSize = size_t{1} << kSlotBits;
        static constexpr uint32_t kMaxGeneration = (1u << (31 - kSlotBits)) - 1;
        static size_t slotOf(int handle) noexcept
        {
            return handle > 0 ? static_cast<size_t>(handle) & (kTableSize - 1)
                              : 0;
        }

    private:
        struct RetiredBatch
        {
            std::vector<std::shared_ptr<ClapInstance>> instances;
            std::vector<ModuleRef> modules;
            /// Guarded by mMutex. False until the barrier has run.
            bool audioQuiesced = false;
        };

        ClapHostManager() = default;

        PlayerErrors scanOnMain(const char *bundlePath,
                                std::vector<ScannedPlugin> &out);
        /// [epoch] is the one loadEffect saw before marshalling: an engine
        /// dispose or unloadAll after it cancels the load.
        PlayerErrors loadOnMain(const std::string &bundlePath,
                                const std::string &pluginId, unsigned sampleRate,
                                unsigned maxBlockSize, uint64_t epoch,
                                int *outHandle);
        /// Retires [instances] (and [modules]) into one new batch under mMutex,
        /// runs the barrier, marks the batch quiesced and drains on main.
        void retireAndDrain(std::vector<std::shared_ptr<ClapInstance>> instances,
                            std::vector<ModuleRef> modules);

        bool epochChanged(uint64_t epoch);
        /// Resolves [handle] and runs [fn] on main under the instance's call
        /// guard (plan R8).
        template <typename Fn>
        PlayerErrors mainHop(int handle, Fn &&fn);
        void restart(ClapInstance &instance);
        static void teardown(ClapInstance &instance);
        void runBarrier();

        // Main only. One acquisition of mMainServicing covers a whole tick.
        void servicePendingBody();
        void drainRetiredBody();
        void drainRetiredGuarded();
        void updatePump();
        static void pumpTick(void *context);
        static void drainTrampoline(void *context);

        std::mutex mMutex;
        // Guarded by mMutex.
        uint32_t mNextGeneration = 1;
        uint64_t mEpoch = 0;
        std::unordered_map<int, std::shared_ptr<ClapInstance>> mInstances;
        std::unordered_map<std::string, ModuleRef> mModuleCache;
        std::vector<std::shared_ptr<RetiredBatch>> mBatches;
        void (*mBarrier)(void *) = nullptr;
        void *mBarrierUserData = nullptr;

        std::array<std::atomic<ClapInstance *>, kTableSize> mTable{};

        // Main only.
        bool mMainServicing = false;
        dispatch_source_t mPump = nullptr;
    };

#ifdef SOLOUD_CLAP_TEST_HOOKS
    namespace test
    {
        /// Called by clapFilterDspCallback between the table load and the
        /// disposition check, with the handle (null to clear).
        void setAfterTableLoadHook(void (*hook)(int handle));
        /// Called by loadEffect after it captured the epoch and before it
        /// marshals to main (null to clear).
        void setBeforeLoadMarshalHook(void (*hook)());
    } // namespace test
#endif
} // namespace soloud_clap

extern "C"
{
    /// SoLoudFilterDspCallback_t for CLAP effects. [aUserData] is the handle
    /// (soloud_clapGetPluginUserData).
    void clapFilterDspCallback(float *aBuffer, unsigned int aSamples,
                               unsigned int aBufferSize, unsigned int aChannels,
                               float aSamplerate, double aTime,
                               const float *aParams, uint64_t aParamChangedMask,
                               unsigned int aParamCount, void *aUserData);
}

#endif // SOLOUD_ENABLE_CLAP
