#ifdef SOLOUD_ENABLE_CLAP

#include "clap_module.h"

#include <clap/version.h>

#include <CoreFoundation/CoreFoundation.h>

#include <atomic>
#include <climits>
#include <cstdlib>

namespace soloud_clap
{
    namespace detail
    {
        enum class ModuleState
        {
            loading,
            ready,
            retiring,
        };

        struct ModuleEntry
        {
            std::string path;
            // Guarded by the registry mutex.
            ModuleState state = ModuleState::loading;
            size_t refs = 0;
            std::thread::id ownerThread;
            // Written by the loading thread before the entry turns Ready, and
            // read only while a reference is held.
            CFBundleRef bundle = nullptr;
            const clap_plugin_entry_t *entry = nullptr;
            const clap_plugin_factory_t *factory = nullptr;
        };
    } // namespace detail

    using detail::ModuleEntry;
    using detail::ModuleState;

#ifdef SOLOUD_CLAP_TEST_HOOKS
    namespace
    {
        std::atomic<void (*)()> gRegistryWaitHook{nullptr};
    } // namespace

    void test::setRegistryWaitHook(void (*hook)())
    {
        gRegistryWaitHook.store(hook, std::memory_order_release);
    }
#endif

    // ModuleRef --------------------------------------------------------------

    ModuleRef::ModuleRef(const ModuleRef &other) : mEntry(other.mEntry)
    {
        if (mEntry != nullptr)
            ModuleRegistry::instance().addRef(*mEntry);
    }

    ModuleRef &ModuleRef::operator=(const ModuleRef &other)
    {
        if (this != &other)
        {
            ModuleRef copy(other);
            *this = std::move(copy);
        }
        return *this;
    }

    ModuleRef::ModuleRef(ModuleRef &&other) noexcept
        : mEntry(std::move(other.mEntry))
    {
    }

    ModuleRef &ModuleRef::operator=(ModuleRef &&other) noexcept
    {
        if (this != &other)
        {
            reset();
            mEntry = std::move(other.mEntry);
        }
        return *this;
    }

    ModuleRef::~ModuleRef() { reset(); }

    void ModuleRef::reset()
    {
        if (mEntry != nullptr)
            ModuleRegistry::instance().release(std::move(mEntry));
        mEntry = nullptr;
    }

    const clap_plugin_entry_t *ModuleRef::entry() const noexcept
    {
        return mEntry != nullptr ? mEntry->entry : nullptr;
    }

    const clap_plugin_factory_t *ModuleRef::factory() const noexcept
    {
        return mEntry != nullptr ? mEntry->factory : nullptr;
    }

    const std::string &ModuleRef::path() const noexcept
    {
        static const std::string kEmpty;
        return mEntry != nullptr ? mEntry->path : kEmpty;
    }

    // ModuleRegistry ---------------------------------------------------------

    ModuleRegistry &ModuleRegistry::instance()
    {
        static ModuleRegistry *registry = new ModuleRegistry();
        return *registry;
    }

    std::string ModuleRegistry::canonicalPath(const std::string &path)
    {
        if (path.empty())
            return {};
        char resolved[PATH_MAX];
        if (::realpath(path.c_str(), resolved) == nullptr)
            return {};
        return resolved;
    }

    size_t ModuleRegistry::entryCount()
    {
        std::lock_guard<std::mutex> lock(mMutex);
        return mEntries.size();
    }

    void ModuleRegistry::addRef(ModuleEntry &entry)
    {
        std::lock_guard<std::mutex> lock(mMutex);
        ++entry.refs;
    }

    namespace
    {
        /// Opens the bundle at [path] and inits its entry. On failure nothing
        /// is left inited and [outBundle] is released.
        AcquireStatus openModule(const std::string &path, CFBundleRef &outBundle,
                                 const clap_plugin_entry_t *&outEntry,
                                 const clap_plugin_factory_t *&outFactory)
        {
            outBundle = nullptr;
            outEntry = nullptr;
            outFactory = nullptr;

            CFURLRef url = CFURLCreateFromFileSystemRepresentation(
                kCFAllocatorDefault,
                reinterpret_cast<const UInt8 *>(path.c_str()),
                static_cast<CFIndex>(path.size()), true);
            if (url == nullptr)
                return AcquireStatus::loadFailed;
            CFBundleRef bundle = CFBundleCreate(kCFAllocatorDefault, url);
            CFRelease(url);
            if (bundle == nullptr)
                return AcquireStatus::loadFailed;
            if (!CFBundleLoadExecutable(bundle))
            {
                CFRelease(bundle);
                return AcquireStatus::loadFailed;
            }
            const auto *entry = static_cast<const clap_plugin_entry_t *>(
                CFBundleGetDataPointerForName(bundle, CFSTR("clap_entry")));
            if (entry == nullptr)
            {
                CFRelease(bundle);
                return AcquireStatus::noEntry;
            }
            if (!clap_version_is_compatible(entry->clap_version) ||
                entry->init == nullptr || entry->deinit == nullptr ||
                entry->get_factory == nullptr)
            {
                CFRelease(bundle);
                return AcquireStatus::badVersion;
            }
            if (!entry->init(path.c_str()))
            {
                // A failed init() leaves the DSO uninitialised: no deinit().
                CFRelease(bundle);
                return AcquireStatus::initFailed;
            }
            const auto *factory = static_cast<const clap_plugin_factory_t *>(
                entry->get_factory(CLAP_PLUGIN_FACTORY_ID));
            if (factory == nullptr || factory->get_plugin_count == nullptr ||
                factory->get_plugin_descriptor == nullptr ||
                factory->create_plugin == nullptr)
            {
                entry->deinit();
                CFRelease(bundle);
                return AcquireStatus::noFactory;
            }
            outBundle = bundle;
            outEntry = entry;
            outFactory = factory;
            return AcquireStatus::ok;
        }
    } // namespace

    ModuleRef ModuleRegistry::acquire(const std::string &path,
                                      AcquireStatus &outStatus)
    {
        const std::string canonical = canonicalPath(path);
        if (canonical.empty())
        {
            outStatus = AcquireStatus::notFound;
            return {};
        }
        const std::thread::id self = std::this_thread::get_id();

        std::unique_lock<std::mutex> lock(mMutex);
        for (;;)
        {
            auto it = mEntries.find(canonical);
            if (it == mEntries.end())
                break;
            const std::shared_ptr<ModuleEntry> entry = it->second;
            if (entry->state == ModuleState::ready)
            {
                ++entry->refs;
                outStatus = AcquireStatus::ok;
                return ModuleRef(entry);
            }
            if (entry->ownerThread == self)
            {
                outStatus = AcquireStatus::busy;
                return {};
            }
#ifdef SOLOUD_CLAP_TEST_HOOKS
            if (auto hook = gRegistryWaitHook.load(std::memory_order_acquire))
                hook();
#endif
            mChanged.wait(lock);
        }

        auto entry = std::make_shared<ModuleEntry>();
        entry->path = canonical;
        entry->state = ModuleState::loading;
        entry->ownerThread = self;
        mEntries.emplace(canonical, entry);
        lock.unlock();

        CFBundleRef bundle = nullptr;
        const clap_plugin_entry_t *clapEntry = nullptr;
        const clap_plugin_factory_t *factory = nullptr;
        const AcquireStatus status =
            openModule(canonical, bundle, clapEntry, factory);

        lock.lock();
        if (status == AcquireStatus::ok)
        {
            entry->bundle = bundle;
            entry->entry = clapEntry;
            entry->factory = factory;
            entry->state = ModuleState::ready;
            entry->ownerThread = std::thread::id();
            entry->refs = 1;
        }
        else
        {
            mEntries.erase(canonical);
        }
        lock.unlock();
        mChanged.notify_all();

        outStatus = status;
        return status == AcquireStatus::ok ? ModuleRef(entry) : ModuleRef();
    }

    void ModuleRegistry::release(std::shared_ptr<ModuleEntry> entry)
    {
        std::unique_lock<std::mutex> lock(mMutex);
        if (entry->refs == 0 || --entry->refs > 0 ||
            entry->state != ModuleState::ready)
            return;
        // The count reached zero: retire in the same critical section, so no
        // acquirer can see an entry that is inited but unreferenced.
        entry->state = ModuleState::retiring;
        entry->ownerThread = std::this_thread::get_id();
        lock.unlock();

        // No reference is left, so no other call into this DSO is in flight.
        entry->entry->deinit();

        lock.lock();
        auto it = mEntries.find(entry->path);
        if (it != mEntries.end() && it->second == entry)
            mEntries.erase(it);
        lock.unlock();
        mChanged.notify_all();

        // The executable stays loaded (CFBundleUnloadExecutable is never
        // called): plugins may leave threads or atexit handlers behind.
        CFRelease(entry->bundle);
        entry->bundle = nullptr;
    }
} // namespace soloud_clap

#endif // SOLOUD_ENABLE_CLAP
