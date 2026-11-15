// CLAP module (DSO) registry: one entry per canonical bundle path, with
// explicit Loading / Ready / Retiring states and a reference count, so that
// clap_entry.init() and deinit() for a path never overlap each other or any
// other call into that DSO (entry.h, CLAP 1.2), whichever thread drops the
// last reference. See docs/clap_hosting_plan.md, Phase 2 (N4/F6).
#pragma once

#ifdef SOLOUD_ENABLE_CLAP

#include <clap/entry.h>
#include <clap/factory/plugin-factory.h>

#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

namespace soloud_clap
{
    enum class AcquireStatus
    {
        ok,
        /// The path does not resolve to an existing bundle.
        notFound,
        /// The bundle could not be created or its executable not loaded.
        loadFailed,
        /// The bundle has no `clap_entry` symbol.
        noEntry,
        /// `clap_entry.clap_version` is not compatible with this host.
        badVersion,
        /// `clap_entry.init()` returned false.
        initFailed,
        /// The plugin factory is missing.
        noFactory,
        /// Reentrant acquire of a path this thread is loading or retiring.
        busy,
    };

    class ModuleRegistry;

    namespace detail
    {
        struct ModuleEntry;
    }

    /// A counted reference to an inited module. Copying adds a reference;
    /// dropping the last one runs `deinit()` on the dropping thread.
    class ModuleRef
    {
    public:
        ModuleRef() = default;
        ModuleRef(const ModuleRef &other);
        ModuleRef &operator=(const ModuleRef &other);
        ModuleRef(ModuleRef &&other) noexcept;
        ModuleRef &operator=(ModuleRef &&other) noexcept;
        ~ModuleRef();

        explicit operator bool() const noexcept { return mEntry != nullptr; }
        void reset();

        const clap_plugin_entry_t *entry() const noexcept;
        const clap_plugin_factory_t *factory() const noexcept;
        const std::string &path() const noexcept;

    private:
        friend class ModuleRegistry;
        explicit ModuleRef(std::shared_ptr<detail::ModuleEntry> entry)
            : mEntry(std::move(entry))
        {
        }
        std::shared_ptr<detail::ModuleEntry> mEntry;
    };

    class ModuleRegistry
    {
    public:
        /// The process-wide registry (leaked, never destroyed).
        static ModuleRegistry &instance();

        /// The realpath of [path], or empty when it does not exist.
        static std::string canonicalPath(const std::string &path);

        /// Returns a reference to the inited module for [path], loading and
        /// initing it when absent. Waits while another thread is loading or
        /// retiring the same path. No registry lock is held during `init()`.
        ModuleRef acquire(const std::string &path, AcquireStatus &outStatus);

        /// Number of entries in any state (tests).
        size_t entryCount();

    private:
        friend class ModuleRef;
        ModuleRegistry() = default;

        void addRef(detail::ModuleEntry &entry);
        void release(std::shared_ptr<detail::ModuleEntry> entry);

        std::mutex mMutex;
        std::condition_variable mChanged;
        std::unordered_map<std::string, std::shared_ptr<detail::ModuleEntry>>
            mEntries;
    };
#ifdef SOLOUD_CLAP_TEST_HOOKS
    namespace test
    {
        /// Called by acquire, under the registry lock, each time it is about
        /// to wait for another thread's loading or retiring entry (null to
        /// clear). It must not call into the registry.
        void setRegistryWaitHook(void (*hook)());
    } // namespace test
#endif
} // namespace soloud_clap

#endif // SOLOUD_ENABLE_CLAP
