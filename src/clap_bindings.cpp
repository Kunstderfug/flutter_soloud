// FFI entry points of the CLAP plugin host, declared in fork/fork_bindings.h.
// Kept out of bindings.cpp so the CLAP host stays one seam. Always compiled:
// without SOLOUD_ENABLE_CLAP every function is an inert stub that initialises
// its outputs and returns notImplemented.
#include "fork/fork_bindings.h"

#include <algorithm>
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

    void copyString(const std::string &s, char *out, int len)
    {
        if (out == nullptr || len <= 0)
            return;
        const size_t n = std::min(s.size(), static_cast<size_t>(len - 1));
        std::memcpy(out, s.data(), n);
        out[n] = '\0';
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

    FFI_PLUGIN_EXPORT enum PlayerErrors soloud_clapLoadInstrument(
        const char *bundlePath,
        const char *pluginId,
        unsigned int sampleRate,
        unsigned int maxBlockSize,
        unsigned int channels,
        int *outPluginHandle)
    {
#ifdef SOLOUD_ENABLE_CLAP
        return manager().loadInstrument(bundlePath, pluginId, sampleRate,
                                        maxBlockSize, channels,
                                        outPluginHandle);
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

    FFI_PLUGIN_EXPORT void *soloud_clapGetSourceCallbackPtr(void)
    {
#ifdef SOLOUD_ENABLE_CLAP
        SoLoudSourceDspCallback_t callback = &clapSourceDspCallback;
        return reinterpret_cast<void *>(callback);
#else
        return nullptr;
#endif
    }

    FFI_PLUGIN_EXPORT enum PlayerErrors soloud_clapSendNote(
        int pluginHandle,
        int isOn,
        int channel,
        int key,
        double velocity,
        unsigned int sampleOffset)
    {
#ifdef SOLOUD_ENABLE_CLAP
        return manager().sendNote(pluginHandle, isOn != 0, channel, key,
                                  velocity, sampleOffset);
#else
        (void)pluginHandle;
        (void)isOn;
        (void)channel;
        (void)key;
        (void)velocity;
        (void)sampleOffset;
        return notImplemented;
#endif
    }

    FFI_PLUGIN_EXPORT int soloud_clapGetNoteDialect(int pluginHandle)
    {
#ifdef SOLOUD_ENABLE_CLAP
        return static_cast<int>(manager().noteDialect(pluginHandle));
#else
        (void)pluginHandle;
        return 0;
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

    // --- parameters, state, latency, events --------------------------------

    FFI_PLUGIN_EXPORT unsigned int soloud_clapGetLatencySamples(int pluginHandle)
    {
#ifdef SOLOUD_ENABLE_CLAP
        return manager().latencySamples(pluginHandle);
#else
        (void)pluginHandle;
        return 0;
#endif
    }

    FFI_PLUGIN_EXPORT int soloud_clapGetParameterCount(int pluginHandle)
    {
#ifdef SOLOUD_ENABLE_CLAP
        return manager().parameterCount(pluginHandle);
#else
        (void)pluginHandle;
        return 0;
#endif
    }

    FFI_PLUGIN_EXPORT enum PlayerErrors soloud_clapGetParameterInfo(
        int pluginHandle,
        int paramIndex,
        unsigned int *outParamId,
        char *outName,
        int nameLen,
        char *outModule,
        int moduleLen,
        double *outMin,
        double *outMax,
        double *outDefault,
        unsigned int *outFlags)
    {
        if (outParamId != nullptr)
            *outParamId = 0xFFFFFFFFu; // CLAP_INVALID_ID
        if (outName != nullptr && nameLen > 0)
            outName[0] = '\0';
        if (outModule != nullptr && moduleLen > 0)
            outModule[0] = '\0';
        for (double *out : {outMin, outMax, outDefault})
        {
            if (out != nullptr)
                *out = 0;
        }
        if (outFlags != nullptr)
            *outFlags = 0;
#ifdef SOLOUD_ENABLE_CLAP
        soloud_clap::ParamEntry entry;
        const PlayerErrors error =
            manager().parameterInfo(pluginHandle, paramIndex, entry);
        if (error != noError)
            return error;
        if (outParamId != nullptr)
            *outParamId = entry.id;
        copyString(entry.name, outName, nameLen);
        copyString(entry.module, outModule, moduleLen);
        if (outMin != nullptr)
            *outMin = entry.min;
        if (outMax != nullptr)
            *outMax = entry.max;
        if (outDefault != nullptr)
            *outDefault = entry.def;
        if (outFlags != nullptr)
            *outFlags = entry.flags;
        return noError;
#else
        (void)pluginHandle;
        (void)paramIndex;
        return notImplemented;
#endif
    }

    FFI_PLUGIN_EXPORT enum PlayerErrors soloud_clapGetParameterValue(
        int pluginHandle,
        unsigned int paramId,
        double *outValue)
    {
        if (outValue != nullptr)
            *outValue = 0;
#ifdef SOLOUD_ENABLE_CLAP
        if (outValue == nullptr)
            return invalidParameter;
        return manager().getParameterValue(pluginHandle, paramId, outValue);
#else
        (void)pluginHandle;
        (void)paramId;
        return notImplemented;
#endif
    }

    FFI_PLUGIN_EXPORT enum PlayerErrors soloud_clapParameterValueToText(
        int pluginHandle,
        unsigned int paramId,
        double value,
        char *outText,
        int textLen)
    {
        if (outText != nullptr && textLen > 0)
            outText[0] = '\0';
#ifdef SOLOUD_ENABLE_CLAP
        if (outText == nullptr || textLen <= 0)
            return invalidParameter;
        return manager().valueToText(pluginHandle, paramId, value, outText,
                                     textLen);
#else
        (void)pluginHandle;
        (void)paramId;
        (void)value;
        return notImplemented;
#endif
    }

    FFI_PLUGIN_EXPORT enum PlayerErrors soloud_clapParameterTextToValue(
        int pluginHandle,
        unsigned int paramId,
        const char *text,
        double *outValue)
    {
        if (outValue != nullptr)
            *outValue = 0;
#ifdef SOLOUD_ENABLE_CLAP
        if (text == nullptr || outValue == nullptr)
            return invalidParameter;
        return manager().textToValue(pluginHandle, paramId, text, outValue);
#else
        (void)pluginHandle;
        (void)paramId;
        (void)text;
        return notImplemented;
#endif
    }

    FFI_PLUGIN_EXPORT enum PlayerErrors soloud_clapSetParameterValue(
        int pluginHandle,
        unsigned int paramId,
        double plainValue)
    {
#ifdef SOLOUD_ENABLE_CLAP
        return manager().setParameterValue(pluginHandle, paramId, plainValue);
#else
        (void)pluginHandle;
        (void)paramId;
        (void)plainValue;
        return notImplemented;
#endif
    }

    FFI_PLUGIN_EXPORT enum PlayerErrors soloud_clapMapSlotParameter(
        int pluginHandle,
        unsigned int slotParamIndex,
        unsigned int paramId,
        float slotValue)
    {
#ifdef SOLOUD_ENABLE_CLAP
        return manager().mapSlotParameter(pluginHandle, slotParamIndex,
                                          paramId, slotValue);
#else
        (void)pluginHandle;
        (void)slotParamIndex;
        (void)paramId;
        (void)slotValue;
        return notImplemented;
#endif
    }

    FFI_PLUGIN_EXPORT enum PlayerErrors soloud_clapUnmapSlotParameter(
        int pluginHandle,
        unsigned int slotParamIndex)
    {
#ifdef SOLOUD_ENABLE_CLAP
        return manager().unmapSlotParameter(pluginHandle, slotParamIndex);
#else
        (void)pluginHandle;
        (void)slotParamIndex;
        return notImplemented;
#endif
    }

    FFI_PLUGIN_EXPORT enum PlayerErrors soloud_clapGetState(
        int pluginHandle,
        unsigned char **outData,
        unsigned int *outSize)
    {
        if (outData != nullptr)
            *outData = nullptr;
        if (outSize != nullptr)
            *outSize = 0;
#ifdef SOLOUD_ENABLE_CLAP
        if (outData == nullptr || outSize == nullptr)
            return invalidParameter;
        std::vector<unsigned char> bytes;
        const PlayerErrors error = manager().saveState(pluginHandle, bytes);
        if (error != noError || bytes.empty())
            return error;
        // The state limit (64 MiB) keeps the size inside unsigned int.
        auto *buffer = static_cast<unsigned char *>(std::malloc(bytes.size()));
        if (buffer == nullptr)
            return outOfMemory;
        std::memcpy(buffer, bytes.data(), bytes.size());
        *outData = buffer;
        *outSize = static_cast<unsigned int>(bytes.size());
        return noError;
#else
        (void)pluginHandle;
        return notImplemented;
#endif
    }

    FFI_PLUGIN_EXPORT enum PlayerErrors soloud_clapSetState(
        int pluginHandle,
        const unsigned char *data,
        unsigned int size)
    {
#ifdef SOLOUD_ENABLE_CLAP
        return manager().loadState(pluginHandle, data, size);
#else
        (void)pluginHandle;
        (void)data;
        (void)size;
        return notImplemented;
#endif
    }

    FFI_PLUGIN_EXPORT enum PlayerErrors soloud_clapDrainEvents(
        int pluginHandle,
        int capacity,
        int *outTypes,
        unsigned int *outParamIds,
        double *outValues,
        unsigned int *outFlags,
        int *outCount)
    {
        if (outCount != nullptr)
            *outCount = 0;
#ifdef SOLOUD_ENABLE_CLAP
        return manager().drainEvents(pluginHandle, capacity, outTypes,
                                     outParamIds, outValues, outFlags,
                                     outCount);
#else
        (void)pluginHandle;
        (void)capacity;
        (void)outTypes;
        (void)outParamIds;
        (void)outValues;
        (void)outFlags;
        return notImplemented;
#endif
    }

    FFI_PLUGIN_EXPORT int soloud_clapHasEditor(int pluginHandle)
    {
#ifdef SOLOUD_ENABLE_CLAP
        return manager().hasEditor(pluginHandle) ? 1 : 0;
#else
        (void)pluginHandle;
        return 0;
#endif
    }

    FFI_PLUGIN_EXPORT enum PlayerErrors soloud_clapOpenEditor(
        int pluginHandle,
        const char *windowTitle)
    {
#ifdef SOLOUD_ENABLE_CLAP
        return manager().openEditor(pluginHandle,
                                    windowTitle != nullptr ? windowTitle : "");
#else
        (void)pluginHandle;
        (void)windowTitle;
        return notImplemented;
#endif
    }

    FFI_PLUGIN_EXPORT enum PlayerErrors soloud_clapCloseEditor(int pluginHandle)
    {
#ifdef SOLOUD_ENABLE_CLAP
        return manager().closeEditor(pluginHandle);
#else
        (void)pluginHandle;
        return notImplemented;
#endif
    }

    FFI_PLUGIN_EXPORT int soloud_clapIsEditorOpen(int pluginHandle)
    {
#ifdef SOLOUD_ENABLE_CLAP
        return manager().isEditorOpen(pluginHandle) ? 1 : 0;
#else
        (void)pluginHandle;
        return 0;
#endif
    }
}
