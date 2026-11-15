#ifdef SOLOUD_ENABLE_CLAP

#include "clap_host.h"

#include <clap/clap.h>

#include <pthread.h>

#include <algorithm>
#include <cstdint>

static_assert(CLAP_VERSION_MAJOR == 1 && CLAP_VERSION_MINOR >= 2,
              "the CLAP host is written against CLAP 1.2");

namespace soloud_clap
{
    uint32_t hostClapVersionMajor() { return CLAP_VERSION_MAJOR; }
    uint32_t hostClapVersionMinor() { return CLAP_VERSION_MINOR; }
    uint32_t hostClapVersionRevision() { return CLAP_VERSION_REVISION; }

    namespace
    {
        constexpr unsigned kMaxBlockSize = 65536;
        constexpr unsigned kMaxBusChannels = 32;
        constexpr uint64_t kPumpPeriodNs = 16ull * 1000ull * 1000ull;

        bool onMainThread() { return pthread_main_np() != 0; }

        /// Runs [fn] on the main thread and waits for it. From a worker it
        /// uses dispatch_sync_f: the caller must hold no lock that main may
        /// wait for, and must not be an audio thread.
        template <typename Fn>
        void runOnMain(Fn &&fn)
        {
            if (onMainThread())
            {
                fn();
                return;
            }
            dispatch_sync_f(dispatch_get_main_queue(), &fn, [](void *context) {
                (*static_cast<Fn *>(context))();
            });
        }

        PlayerErrors errorFor(AcquireStatus status)
        {
            switch (status)
            {
            case AcquireStatus::ok:
                return noError;
            case AcquireStatus::notFound:
                return fileNotFound;
            case AcquireStatus::loadFailed:
            case AcquireStatus::noEntry:
            case AcquireStatus::badVersion:
                return dllNotFound;
            case AcquireStatus::initFailed:
            case AcquireStatus::noFactory:
                return fileLoadFailed;
            case AcquireStatus::busy:
                return unknownError;
            }
            return unknownError;
        }

        std::string copyString(const char *s) { return s != nullptr ? s : ""; }

#ifdef SOLOUD_CLAP_TEST_HOOKS
        std::atomic<void (*)(int)> gAfterTableLoadHook{nullptr};
        std::atomic<void (*)()> gBeforeLoadMarshalHook{nullptr};
#endif
    } // namespace

#ifdef SOLOUD_CLAP_TEST_HOOKS
    void test::setAfterTableLoadHook(void (*hook)(int handle))
    {
        gAfterTableLoadHook.store(hook, std::memory_order_release);
    }

    void test::setBeforeLoadMarshalHook(void (*hook)())
    {
        gBeforeLoadMarshalHook.store(hook, std::memory_order_release);
    }
#endif

    ClapHostManager &ClapHostManager::instance()
    {
        // Leaked: no static destructor, no exit-time teardown in Phase 2.
        static ClapHostManager *manager = new ClapHostManager();
        return *manager;
    }

    void ClapHostManager::setAudioThreadBarrier(void (*barrier)(void *),
                                                void *userData)
    {
        std::lock_guard<std::mutex> lock(mMutex);
        mBarrier = barrier;
        mBarrierUserData = userData;
    }

    void ClapHostManager::runBarrier()
    {
        void (*barrier)(void *) = nullptr;
        void *userData = nullptr;
        {
            std::lock_guard<std::mutex> lock(mMutex);
            barrier = mBarrier;
            userData = mBarrierUserData;
        }
        if (barrier != nullptr)
            barrier(userData);
    }

    ClapInstance *ClapHostManager::getInstanceAudioThread(int handle) noexcept
    {
        const size_t slot = slotOf(handle);
        if (slot == 0)
            return nullptr;
        // The instance outlives any DSP call that loaded it: teardown waits
        // for the barrier (or a stopped device) before touching it.
        ClapInstance *instance = mTable[slot].load(std::memory_order_acquire);
        return instance != nullptr && instance->handle() == handle ? instance
                                                                   : nullptr;
    }

    std::shared_ptr<ClapInstance> ClapHostManager::getInstance(int handle)
    {
        std::lock_guard<std::mutex> lock(mMutex);
        auto it = mInstances.find(handle);
        return it != mInstances.end() ? it->second : nullptr;
    }

    size_t ClapHostManager::liveInstanceCount()
    {
        std::lock_guard<std::mutex> lock(mMutex);
        return mInstances.size();
    }

    size_t ClapHostManager::retiredBatchCount()
    {
        std::lock_guard<std::mutex> lock(mMutex);
        return mBatches.size();
    }

    size_t ClapHostManager::cachedModuleCount()
    {
        std::lock_guard<std::mutex> lock(mMutex);
        return mModuleCache.size();
    }

    bool ClapHostManager::epochChanged(uint64_t epoch)
    {
        std::lock_guard<std::mutex> lock(mMutex);
        return mEpoch != epoch;
    }

    // --- scan --------------------------------------------------------------

    PlayerErrors ClapHostManager::scanBundle(const char *bundlePath,
                                             std::vector<ScannedPlugin> &out)
    {
        out.clear();
        if (bundlePath == nullptr || bundlePath[0] == '\0')
            return invalidParameter;
        PlayerErrors result = unknownError;
        runOnMain([&] { result = scanOnMain(bundlePath, out); });
        return result;
    }

    PlayerErrors ClapHostManager::scanOnMain(const char *bundlePath,
                                             std::vector<ScannedPlugin> &out)
    {
        AcquireStatus status = AcquireStatus::ok;
        // A cached module is shared: no second init().
        ModuleRef module = ModuleRegistry::instance().acquire(bundlePath, status);
        if (!module)
            return errorFor(status);
        const clap_plugin_factory_t *factory = module.factory();
        const uint32_t count = factory->get_plugin_count(factory);
        for (uint32_t i = 0; i < count; ++i)
        {
            const clap_plugin_descriptor_t *desc =
                factory->get_plugin_descriptor(factory, i);
            if (desc == nullptr || desc->id == nullptr ||
                !clap_version_is_compatible(desc->clap_version))
                continue;
            ScannedPlugin plugin;
            plugin.id = desc->id;
            plugin.name = copyString(desc->name);
            plugin.vendor = copyString(desc->vendor);
            plugin.version = copyString(desc->version);
            if (desc->features != nullptr)
            {
                for (const char *const *f = desc->features; *f != nullptr; ++f)
                {
                    if (!plugin.features.empty())
                        plugin.features += ';';
                    plugin.features += *f;
                }
            }
            out.push_back(std::move(plugin));
        }
        return noError;
    }

    // --- load --------------------------------------------------------------

    PlayerErrors ClapHostManager::loadEffect(const char *bundlePath,
                                             const char *pluginId,
                                             unsigned sampleRate,
                                             unsigned maxBlockSize,
                                             unsigned channels, int *outHandle)
    {
        if (outHandle != nullptr)
            *outHandle = 0;
        if (bundlePath == nullptr || bundlePath[0] == '\0' ||
            pluginId == nullptr || pluginId[0] == '\0' || outHandle == nullptr ||
            sampleRate == 0 || maxBlockSize == 0 ||
            maxBlockSize > kMaxBlockSize || channels == 0 ||
            channels > kMaxBusChannels)
            return invalidParameter;
        const std::string path = bundlePath;
        const std::string id = pluginId;
        // Captured before waiting for main: a dispose or unloadAll that runs
        // while this load is queued cancels it.
        uint64_t epoch = 0;
        {
            std::lock_guard<std::mutex> lock(mMutex);
            epoch = mEpoch;
        }
#ifdef SOLOUD_CLAP_TEST_HOOKS
        if (auto hook = gBeforeLoadMarshalHook.load(std::memory_order_acquire))
            hook();
#endif
        PlayerErrors result = unknownError;
        int handle = 0;
        runOnMain([&] {
            result =
                loadOnMain(path, id, sampleRate, maxBlockSize, epoch, &handle);
        });
        *outHandle = result == noError ? handle : 0;
        return result;
    }

    PlayerErrors ClapHostManager::loadOnMain(const std::string &bundlePath,
                                             const std::string &pluginId,
                                             unsigned sampleRate,
                                             unsigned maxBlockSize,
                                             uint64_t epoch, int *outHandle)
    {
        // Cancelled while it waited for main: nothing is created.
        if (epochChanged(epoch))
            return unknownError;

        AcquireStatus status = AcquireStatus::ok;
        ModuleRef module = ModuleRegistry::instance().acquire(bundlePath, status);
        if (!module)
            return errorFor(status);
        const clap_plugin_factory_t *factory = module.factory();
        bool found = false;
        const uint32_t count = factory->get_plugin_count(factory);
        for (uint32_t i = 0; i < count && !found; ++i)
        {
            const clap_plugin_descriptor_t *desc =
                factory->get_plugin_descriptor(factory, i);
            found = desc != nullptr && desc->id != nullptr &&
                    pluginId == desc->id;
        }
        if (!found)
            return fileLoadFailed;

        ModuleRef cacheRef = module; // Published into the cache on success.
        const std::string cacheKey = module.path();
        auto instance = std::make_shared<ClapInstance>(
            std::move(module), pluginId, static_cast<double>(sampleRate),
            maxBlockSize);

        // The instance is unpublished until the end, so nothing else can
        // reach it: a failure, or an engine dispose / unloadAll meanwhile (the
        // epoch moved), tears it down right here.
        instance->mLifecycleBusy = true;
        auto fail = [&](PlayerErrors error) {
            teardown(*instance);
            return error;
        };
        // An engine dispose or unloadAll during a plugin call aborts the load.
        if (!instance->create())
            return fail(fileLoadFailed);
        if (!instance->init())
            return fail(fileLoadFailed);
        instance->cacheExtensions();
        if (epochChanged(epoch))
            return fail(unknownError);
        // Inactive: the parameter list, and any rescan latched by init.
        instance->rebuildParams();
        if (epochChanged(epoch))
            return fail(unknownError);
        PortLayout layout;
        std::string reason;
        if (!instance->readPorts(layout, reason))
            return fail(fileLoadFailed);
        if (epochChanged(epoch))
            return fail(unknownError);
        // A rescan(ALL) from the port queries: rebuilt while inactive.
        if (!instance->settleParamsBeforeActivate())
            return fail(fileLoadFailed);
        if (epochChanged(epoch))
            return fail(unknownError);
        if (!instance->installLayout(layout))
            return fail(outOfMemory);
        if (!instance->activate())
            return fail(fileLoadFailed);
        // Dart reads the getter after load: no latencyChanged here.
        instance->publishLatency(false);
        instance->afterPluginCall();
        if (epochChanged(epoch))
            return fail(unknownError);

        int handle = 0;
        {
            std::lock_guard<std::mutex> lock(mMutex);
            if (mEpoch == epoch)
            {
                size_t slot = 0;
                for (size_t i = 1; i < kTableSize; ++i)
                {
                    if (mTable[i].load(std::memory_order_relaxed) == nullptr)
                    {
                        slot = i;
                        break;
                    }
                }
                if (slot != 0)
                {
                    if (mNextGeneration > kMaxGeneration)
                        mNextGeneration = 1;
                    handle = static_cast<int>((mNextGeneration++ << kSlotBits) |
                                              slot);
                    instance->setHandle(handle);
                    mInstances.emplace(handle, instance);
                    if (mModuleCache.find(cacheKey) == mModuleCache.end())
                        mModuleCache.emplace(cacheKey, std::move(cacheRef));
                    instance->mLifecycleBusy = false;
                    // Timers registered during init fire from now on (R9).
                    instance->enableTimers();
                    mTable[slot].store(instance.get(), std::memory_order_release);
                }
            }
        }
        if (handle == 0)
            return fail(epochChanged(epoch) ? unknownError : outOfMemory);
        updatePump();
        *outHandle = handle;
        return noError;
    }

    // --- unload ------------------------------------------------------------

    PlayerErrors ClapHostManager::unloadPlugin(int handle)
    {
        std::shared_ptr<ClapInstance> instance;
        {
            std::lock_guard<std::mutex> lock(mMutex);
            auto it = mInstances.find(handle);
            if (it == mInstances.end())
                return invalidParameter;
            instance = it->second;
        }
        retireAndDrain({instance}, {});
        return noError;
    }

    void ClapHostManager::unloadAll()
    {
        retireAndDrain({}, {});
    }

    void ClapHostManager::retireAndDrain(
        std::vector<std::shared_ptr<ClapInstance>> instances,
        std::vector<ModuleRef> modules)
    {
        // An empty [instances] means everything, with the module cache.
        const bool all = instances.empty();
        auto batch = std::make_shared<RetiredBatch>();
        {
            std::lock_guard<std::mutex> lock(mMutex);
            if (all)
            {
                for (auto &entry : mInstances)
                    instances.push_back(entry.second);
                for (auto &entry : mModuleCache)
                    modules.push_back(std::move(entry.second));
                mModuleCache.clear();
                ++mEpoch;
            }
            for (auto &instance : instances)
            {
                auto it = mInstances.find(instance->handle());
                if (it == mInstances.end() || it->second != instance)
                    continue; // Already retired by someone else.
                const size_t slot = slotOf(instance->handle());
                if (mTable[slot].load(std::memory_order_relaxed) == instance.get())
                    mTable[slot].store(nullptr, std::memory_order_release);
                mInstances.erase(it);
                instance->retire();
                batch->instances.push_back(instance);
            }
            batch->modules = std::move(modules);
            if (batch->instances.empty() && batch->modules.empty())
                return;
            batch->audioQuiesced = false;
            mBatches.push_back(batch);
        }
        instances.clear();

        // No new DSP call can reach the batch now; wait out the ones that may
        // have loaded a pointer before the table was cleared.
        runBarrier();
        {
            std::lock_guard<std::mutex> lock(mMutex);
            batch->audioQuiesced = true;
        }
        batch.reset();

        runOnMain([this] {
            drainRetiredGuarded();
            updatePump();
        });
    }

    void ClapHostManager::retireAllForDispose()
    {
        bool posted = false;
        {
            std::lock_guard<std::mutex> lock(mMutex);
            auto batch = std::make_shared<RetiredBatch>();
            for (auto &entry : mInstances)
            {
                ClapInstance &instance = *entry.second;
                const size_t slot = slotOf(instance.handle());
                if (mTable[slot].load(std::memory_order_relaxed) == &instance)
                    mTable[slot].store(nullptr, std::memory_order_release);
                instance.retire();
                batch->instances.push_back(entry.second);
            }
            mInstances.clear();
            for (auto &entry : mModuleCache)
                batch->modules.push_back(std::move(entry.second));
            mModuleCache.clear();
            ++mEpoch;
            if (!batch->instances.empty() || !batch->modules.empty())
            {
                // The device is stopped: no mix can still be using them.
                batch->audioQuiesced = true;
                mBatches.push_back(std::move(batch));
                posted = true;
            }
        }
        // Always asynchronous, so no plugin call runs under the engine locks
        // even when dispose runs on main.
        if (posted)
            dispatch_async_f(dispatch_get_main_queue(), nullptr,
                             &ClapHostManager::drainTrampoline);
    }

    void ClapHostManager::drainTrampoline(void *)
    {
        ClapHostManager &manager = instance();
        manager.drainRetiredGuarded();
        manager.updatePump();
    }

    void ClapHostManager::teardown(ClapInstance &instance)
    {
        // Never aborts: continues from the recorded state to Destroyed.
        instance.mLifecycleBusy = true;
        // Timers off and rescans bump only, from here on (plan S6).
        instance.beginTeardown();
        instance.mRestartRequested.store(false, std::memory_order_release);
        instance.mCallbackRequested.store(false, std::memory_order_release);
        if (instance.plugin() != nullptr &&
            instance.lifecycle() != Lifecycle::destroyed)
        {
            // The gui goes first, while the plugin is still whole (plan 4b).
            instance.destroyEditor();
            instance.stopProcessingExclusive();
            if (instance.lifecycle() == Lifecycle::activated)
                instance.deactivate();
            // Registrations survive into destroy(): unregister answers true.
            instance.destroy();
        }
        instance.clearTimers();
        instance.mLifecycleBusy = false;
        instance.releaseModule();
    }

    void ClapHostManager::drainRetiredGuarded()
    {
        if (mMainServicing)
            return; // A tick or drain further up the stack finishes the work.
        mMainServicing = true;
        drainRetiredBody();
        mMainServicing = false;
    }

    void ClapHostManager::drainRetiredBody()
    {
        std::vector<std::shared_ptr<RetiredBatch>> quiesced;
        {
            std::lock_guard<std::mutex> lock(mMutex);
            for (auto &batch : mBatches)
            {
                if (batch->audioQuiesced)
                    quiesced.push_back(batch);
            }
        }
        for (auto &batch : quiesced)
        {
            bool done = true;
            for (auto &instance : batch->instances)
            {
                if (instance->plugin() == nullptr ||
                    instance->lifecycle() == Lifecycle::destroyed)
                    continue;
                if (instance->mLifecycleBusy)
                {
                    done = false; // Inside a lifecycle call: a later tick.
                    continue;
                }
                teardown(*instance);
            }
            if (done)
            {
                std::lock_guard<std::mutex> lock(mMutex);
                mBatches.erase(
                    std::remove(mBatches.begin(), mBatches.end(), batch),
                    mBatches.end());
            }
        }
        // Dropping the last batch references releases their modules here,
        // outside the manager mutex (deinit() may run).
        quiesced.clear();
    }

    // --- restart and callbacks (pump) ---------------------------------------

    void ClapHostManager::restart(ClapInstance &instance)
    {
        instance.mLifecycleBusy = true;
        struct BusyReset
        {
            ClapInstance &instance;
            ~BusyReset() { instance.mLifecycleBusy = false; }
        } busyReset{instance};
        auto retired = [&] {
            return instance.disposition() == Disposition::retired;
        };

        // Active -> Servicing (dry). Failed stays Failed (muted) throughout.
        if (!instance.transition(Disposition::active, Disposition::servicing) &&
            instance.disposition() != Disposition::failed)
            return; // Retired.
        runBarrier();
        instance.stopProcessingExclusive();
        if (retired())
            return;
        if (instance.lifecycle() == Lifecycle::activated)
        {
            instance.deactivate();
            if (retired())
                return;
        }
        // Deactivated: rebuild the parameter list (a pending rescan(ALL)
        // included) before anything below can fail-return (plan B2).
        instance.rebuildParams();
        if (retired())
            return;
        auto failRestart = [&] {
            instance.transition(Disposition::servicing, Disposition::failed);
            instance.notify(kNotifyRestartFailed);
        };
        PortLayout layout;
        std::string reason;
        const bool routable = instance.readPorts(layout, reason);
        if (retired())
            return;
        if (!routable)
        {
            failRestart();
            return;
        }
        // A rescan(ALL) from the port queries is rebuilt while inactive. A
        // list that keeps changing stays inactive (Failed + inited), where
        // the pump's rebuild step keeps servicing it.
        const bool settled = instance.settleParamsBeforeActivate();
        if (retired())
            return;
        if (!settled)
        {
            failRestart();
            return;
        }
        // Out of memory: the old configuration stays, processing stays off.
        if (!instance.installLayout(layout) || !instance.activate())
        {
            failRestart();
            return;
        }
        // The cached value is published before its notification.
        instance.publishLatency(true);
        instance.afterPluginCall();
        if (retired())
            return;
        if (instance.transition(Disposition::servicing, Disposition::active) ||
            instance.transition(Disposition::failed, Disposition::active))
        {
            instance.bumpRestartCount();
            instance.notify(kNotifyRestarted);
        }
    }

    void ClapHostManager::servicePendingBody()
    {
        std::vector<std::shared_ptr<ClapInstance>> snapshot;
        {
            std::lock_guard<std::mutex> lock(mMutex);
            snapshot.reserve(mInstances.size());
            for (auto &entry : mInstances)
                snapshot.push_back(entry.second);
        }
        auto skip = [](ClapInstance &instance) {
            return instance.mLifecycleBusy ||
                   instance.disposition() == Disposition::retired;
        };
        for (auto &instance : snapshot)
        {
            if (skip(*instance))
                continue;
            if (instance->mRestartRequested.exchange(false,
                                                     std::memory_order_acq_rel))
                restart(*instance);
            if (skip(*instance))
                continue;
            if (instance->mCallbackRequested.exchange(false,
                                                      std::memory_order_acq_rel))
            {
                InstanceCallGuard guard(*instance);
                instance->onMainThread();
                instance->afterPluginCall();
            }
            if (skip(*instance))
                continue;
            if (instance->rebuildStepNeeded())
            {
                InstanceCallGuard guard(*instance);
                instance->afterPluginCall();
            }
            if (skip(*instance))
                continue;
            {
                InstanceCallGuard guard(*instance);
                instance->serviceFlush(nowNs());
            }
            if (skip(*instance))
                continue;
            {
                InstanceCallGuard guard(*instance);
                instance->fireTimers(nowNs());
            }
            if (skip(*instance))
                continue;
            if (instance->editorServiceNeeded())
            {
                InstanceCallGuard guard(*instance);
                instance->serviceEditor();
            }
        }
    }

    // --- Phase 3: parameters, state, latency, events -------------------------

    template <typename Fn>
    PlayerErrors ClapHostManager::mainHop(int handle, Fn &&fn)
    {
        std::shared_ptr<ClapInstance> instance = getInstance(handle);
        if (!instance)
            return invalidParameter;
        PlayerErrors result = unknownError;
        runOnMain([&] {
            if (!instance->callable())
            {
                result = invalidParameter;
                return;
            }
            InstanceCallGuard guard(*instance);
            if (!guard.acquired())
            {
                result = unknownError; // Busy: a re-entrant call.
                return;
            }
            result = fn(*instance);
        });
        return result;
    }

    uint32_t ClapHostManager::latencySamples(int handle)
    {
        std::shared_ptr<ClapInstance> instance = getInstance(handle);
        return instance ? instance->latency() : 0;
    }

    int ClapHostManager::parameterCount(int handle)
    {
        std::shared_ptr<ClapInstance> instance = getInstance(handle);
        if (!instance)
            return 0;
        std::shared_ptr<const ParamCache> cache = instance->params();
        return cache->pending ? 0 : static_cast<int>(cache->entries.size());
    }

    PlayerErrors ClapHostManager::parameterInfo(int handle, int index,
                                                ParamEntry &out)
    {
        out = ParamEntry();
        std::shared_ptr<ClapInstance> instance = getInstance(handle);
        if (!instance)
            return invalidParameter;
        std::shared_ptr<const ParamCache> cache = instance->params();
        if (cache->pending || index < 0 ||
            static_cast<size_t>(index) >= cache->entries.size())
            return invalidParameter;
        out = cache->entries[static_cast<size_t>(index)];
        return noError;
    }

    PlayerErrors ClapHostManager::setParameterValue(int handle, clap_id id,
                                                    double value)
    {
        std::shared_ptr<ClapInstance> instance = getInstance(handle);
        if (!instance)
            return invalidParameter;
        return instance->setParameterValue(id, value);
    }

    PlayerErrors ClapHostManager::getParameterValue(int handle, clap_id id,
                                                    double *outValue)
    {
        if (outValue != nullptr)
            *outValue = 0;
        return mainHop(handle, [&](ClapInstance &instance) {
            return instance.getParameterValue(id, outValue);
        });
    }

    PlayerErrors ClapHostManager::valueToText(int handle, clap_id id,
                                              double value, char *outText,
                                              int textLen)
    {
        if (outText != nullptr && textLen > 0)
            outText[0] = '\0';
        return mainHop(handle, [&](ClapInstance &instance) {
            return instance.valueToText(id, value, outText, textLen);
        });
    }

    PlayerErrors ClapHostManager::textToValue(int handle, clap_id id,
                                              const char *text, double *outValue)
    {
        if (outValue != nullptr)
            *outValue = 0;
        return mainHop(handle, [&](ClapInstance &instance) {
            return instance.textToValue(id, text, outValue);
        });
    }

    PlayerErrors ClapHostManager::saveState(int handle,
                                            std::vector<unsigned char> &out)
    {
        out.clear();
        return mainHop(handle, [&](ClapInstance &instance) {
            return instance.saveState(out);
        });
    }

    PlayerErrors ClapHostManager::loadState(int handle,
                                            const unsigned char *data,
                                            size_t size)
    {
        if (data == nullptr && size > 0)
            return invalidParameter;
        return mainHop(handle, [&](ClapInstance &instance) {
            return instance.loadState(data, size);
        });
    }

    PlayerErrors ClapHostManager::drainEvents(int handle, int capacity,
                                              int *outTypes,
                                              unsigned *outParamIds,
                                              double *outValues,
                                              unsigned *outFlags, int *outCount)
    {
        if (outCount != nullptr)
            *outCount = 0;
        if (outCount == nullptr || capacity < 0 ||
            (capacity > 0 && (outTypes == nullptr || outParamIds == nullptr ||
                              outValues == nullptr || outFlags == nullptr)))
            return invalidParameter;
        std::shared_ptr<ClapInstance> instance = getInstance(handle);
        if (!instance)
            return invalidParameter;
        *outCount = instance->drainEvents(capacity, outTypes, outParamIds,
                                          outValues, outFlags);
        return noError;
    }

    // --- Phase 4b: plugin editor ---------------------------------------------

    bool ClapHostManager::hasEditor(int handle)
    {
        std::shared_ptr<ClapInstance> instance = getInstance(handle);
        return instance && instance->hasEditor();
    }

    bool ClapHostManager::isEditorOpen(int handle)
    {
        std::shared_ptr<ClapInstance> instance = getInstance(handle);
        return instance && instance->editorOpen();
    }

    PlayerErrors ClapHostManager::openEditor(int handle,
                                             const std::string &title)
    {
        return mainHop(handle, [&](ClapInstance &instance) {
            return instance.openEditor(title);
        });
    }

    PlayerErrors ClapHostManager::closeEditor(int handle)
    {
        return mainHop(handle, [&](ClapInstance &instance) {
            return instance.closeEditor();
        });
    }

    void ClapHostManager::pumpTick(void *)
    {
        ClapHostManager &manager = instance();
        if (manager.mMainServicing)
            return; // A nested runloop inside a plugin call: next tick.
        manager.mMainServicing = true;
        manager.servicePendingBody();
        manager.drainRetiredBody();
        manager.mMainServicing = false;
        manager.updatePump();
    }

    void ClapHostManager::updatePump()
    {
        bool needed = false;
        {
            std::lock_guard<std::mutex> lock(mMutex);
            needed = !mInstances.empty() || !mBatches.empty();
        }
        if (needed && mPump == nullptr)
        {
            mPump = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0,
                                           dispatch_get_main_queue());
            dispatch_source_set_timer(
                mPump, dispatch_time(DISPATCH_TIME_NOW, kPumpPeriodNs),
                kPumpPeriodNs, kPumpPeriodNs / 4);
            dispatch_set_context(mPump, nullptr);
            dispatch_source_set_event_handler_f(mPump, &ClapHostManager::pumpTick);
            dispatch_resume(mPump);
        }
        else if (!needed && mPump != nullptr)
        {
            dispatch_source_cancel(mPump);
            dispatch_release(mPump);
            mPump = nullptr;
        }
    }
} // namespace soloud_clap

extern "C" void clapFilterDspCallback(float *aBuffer, unsigned int aSamples,
                                      unsigned int aBufferSize,
                                      unsigned int aChannels,
                                      float /*aSamplerate*/, double /*aTime*/,
                                      const float * /*aParams*/,
                                      uint64_t /*aParamChangedMask*/,
                                      unsigned int /*aParamCount*/,
                                      void *aUserData)
{
    const int handle =
        static_cast<int>(reinterpret_cast<uintptr_t>(aUserData));
    soloud_clap::ClapInstance *instance =
        soloud_clap::ClapHostManager::instance().getInstanceAudioThread(handle);
    if (instance == nullptr)
        return;
#ifdef SOLOUD_CLAP_TEST_HOOKS
    if (auto hook = soloud_clap::gAfterTableLoadHook.load(
            std::memory_order_acquire))
        hook(handle);
#endif
    instance->processAudio(aBuffer, aSamples, aBufferSize, aChannels);
}

#endif // SOLOUD_ENABLE_CLAP
