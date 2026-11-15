// FFI entry points of the CLAP plugin host, declared in bindings.h. Kept out
// of bindings.cpp so the CLAP host stays one seam. Always compiled: without
// SOLOUD_ENABLE_CLAP every function is an inert stub that initialises its
// outputs and returns notImplemented.
#include "bindings.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <initializer_list>

#ifdef SOLOUD_ENABLE_CLAP
#include "clap/clap_host.h"
#include "plugin_audio_barrier.h"

#include <string>
#include <vector>
#endif

namespace
{
    void clearScanOutputs(int *outCount, char ***outIds, char ***outNames,
                          char ***outVendors, char ***outVersions,
                          char ***outFeatures)
    {
        if (outCount != nullptr)
            *outCount = 0;
        for (char ***out : {outIds, outNames, outVendors, outVersions,
                            outFeatures})
        {
            if (out != nullptr)
                *out = nullptr;
        }
    }

#ifdef SOLOUD_ENABLE_CLAP
    soloud_clap::ClapHostManager &manager()
    {
        auto &m = soloud_clap::ClapHostManager::instance();
        // Every entry point sets it, so whichever runs first installs the
        // engine's barrier before anything can be unloaded or restarted.
        m.setAudioThreadBarrier(&soloud_pluginAudioThreadBarrier, nullptr);
        return m;
    }

    char *duplicate(const std::string &s)
    {
        char *buffer = static_cast<char *>(std::malloc(s.size() + 1));
        if (buffer != nullptr)
            std::memcpy(buffer, s.c_str(), s.size() + 1);
        return buffer;
    }
#endif
} // namespace

extern "C"
{
    FFI_PLUGIN_EXPORT int soloud_clapIsSupported(void)
    {
#ifdef SOLOUD_ENABLE_CLAP
        return 1;
#else
        return 0;
#endif
    }

    FFI_PLUGIN_EXPORT void soloud_clapGetHostClapVersion(
        unsigned int *outMajor,
        unsigned int *outMinor,
        unsigned int *outRevision)
    {
        unsigned int major = 0, minor = 0, revision = 0;
#ifdef SOLOUD_ENABLE_CLAP
        major = soloud_clap::hostClapVersionMajor();
        minor = soloud_clap::hostClapVersionMinor();
        revision = soloud_clap::hostClapVersionRevision();
#endif
        if (outMajor != nullptr)
            *outMajor = major;
        if (outMinor != nullptr)
            *outMinor = minor;
        if (outRevision != nullptr)
            *outRevision = revision;
    }

    FFI_PLUGIN_EXPORT enum PlayerErrors soloud_clapScanBundle(
        const char *bundlePath,
        int *outCount,
        char ***outIds,
        char ***outNames,
        char ***outVendors,
        char ***outVersions,
        char ***outFeatures)
    {
        clearScanOutputs(outCount, outIds, outNames, outVendors, outVersions,
                         outFeatures);
#ifdef SOLOUD_ENABLE_CLAP
        if (bundlePath == nullptr || outCount == nullptr || outIds == nullptr ||
            outNames == nullptr || outVendors == nullptr ||
            outVersions == nullptr || outFeatures == nullptr)
        {
            return invalidParameter;
        }
        std::vector<soloud_clap::ScannedPlugin> plugins;
        const PlayerErrors error = manager().scanBundle(bundlePath, plugins);
        if (error != noError || plugins.empty())
            return error;

        const size_t count = plugins.size();
        char **arrays[5] = {};
        for (char **&array : arrays)
            array = static_cast<char **>(std::calloc(count, sizeof(char *)));
        bool ok = true;
        for (char **array : arrays)
            ok = ok && array != nullptr;
        for (size_t i = 0; ok && i < count; ++i)
        {
            const auto &p = plugins[i];
            arrays[0][i] = duplicate(p.id);
            arrays[1][i] = duplicate(p.name);
            arrays[2][i] = duplicate(p.vendor);
            arrays[3][i] = duplicate(p.version);
            arrays[4][i] = duplicate(p.features);
            for (char **array : arrays)
                ok = ok && array[i] != nullptr;
        }
        if (!ok)
        {
            soloud_clapFreeScanResults(static_cast<int>(count), arrays[0],
                                       arrays[1], arrays[2], arrays[3],
                                       arrays[4]);
            return outOfMemory;
        }
        *outCount = static_cast<int>(count);
        *outIds = arrays[0];
        *outNames = arrays[1];
        *outVendors = arrays[2];
        *outVersions = arrays[3];
        *outFeatures = arrays[4];
        return noError;
#else
        (void)bundlePath;
        return notImplemented;
#endif
    }

    FFI_PLUGIN_EXPORT void soloud_clapFreeScanResults(
        int count,
        char **ids,
        char **names,
        char **vendors,
        char **versions,
        char **features)
    {
        for (char **array : {ids, names, vendors, versions, features})
        {
            if (array == nullptr)
                continue;
            for (int i = 0; i < count; ++i)
                std::free(array[i]);
            std::free(array);
        }
    }

    FFI_PLUGIN_EXPORT enum PlayerErrors soloud_clapLoadEffect(
        const char *bundlePath,
        const char *pluginId,
        unsigned int sampleRate,
        unsigned int maxBlockSize,
        unsigned int channels,
        int *outPluginHandle)
    {
#ifdef SOLOUD_ENABLE_CLAP
        return manager().loadEffect(bundlePath, pluginId, sampleRate,
                                    maxBlockSize, channels, outPluginHandle);
#else
        (void)bundlePath;
        (void)pluginId;
        (void)sampleRate;
        (void)maxBlockSize;
        (void)channels;
        if (outPluginHandle != nullptr)
            *outPluginHandle = 0;
        return notImplemented;
#endif
    }

    FFI_PLUGIN_EXPORT enum PlayerErrors soloud_clapUnloadPlugin(int pluginHandle)
    {
#ifdef SOLOUD_ENABLE_CLAP
        return manager().unloadPlugin(pluginHandle);
#else
        (void)pluginHandle;
        return notImplemented;
#endif
    }

    FFI_PLUGIN_EXPORT void soloud_clapHostUnloadAll(void)
    {
#ifdef SOLOUD_ENABLE_CLAP
        manager().unloadAll();
#endif
    }

    FFI_PLUGIN_EXPORT void *soloud_clapGetFilterCallbackPtr(void)
    {
#ifdef SOLOUD_ENABLE_CLAP
        SoLoudFilterDspCallback_t callback = &clapFilterDspCallback;
        return reinterpret_cast<void *>(callback);
#else
        return nullptr;
#endif
    }

    FFI_PLUGIN_EXPORT void *soloud_clapGetPluginUserData(int pluginHandle)
    {
#ifdef SOLOUD_ENABLE_CLAP
        if (pluginHandle <= 0)
            return nullptr;
        return reinterpret_cast<void *>(static_cast<uintptr_t>(pluginHandle));
#else
        (void)pluginHandle;
        return nullptr;
#endif
    }
}
