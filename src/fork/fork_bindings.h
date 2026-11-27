/// Declarations of the FFI_PLUGIN_EXPORT entry points that only this fork of
/// flutter_soloud adds. They live here, not in the upstream `src/bindings.h`,
/// so upstream merges touch fewer lines. `src/bindings.h` includes this header
/// at its end, so every file that includes it still sees the whole native ABI,
/// and both headers are ffigen entry points (see ffigen.yaml).
///
/// Definitions: `src/fork/fork_bindings.cpp` (engine power, capture and voice
/// groups) and `src/clap_bindings.cpp` (CLAP plugin hosting).

#pragma once

#ifndef FORK_BINDINGS_H
#define FORK_BINDINGS_H

// The macros, enums and typedefs these declarations use.
#include "../bindings.h"

#ifdef __cplusplus
extern "C"
{
#endif

  // Engine power: domain-idle output stop.

  /// Nonblocking one-shot domain-idle stop. False means unavailable or busy;
  /// the caller may retry. True means queued, not that stopping has completed.
  FFI_PLUGIN_EXPORT bool requestAudioDeviceIdleStop();

  // Native capture (src/capture/).

  /// List capture/input devices.
  FFI_PLUGIN_EXPORT void listCaptureDevices(char **devicesName, int **deviceId,
                                            int **isDefault, int *n_devices);

  FFI_PLUGIN_EXPORT void freeListCaptureDevices(char **devicesName,
                                                int **deviceId, int **isDefault,
                                                int n_devices);

  FFI_PLUGIN_EXPORT enum PlayerErrors
  startCapture(char *path, unsigned int sampleRate, unsigned int channels,
               unsigned int bufferSizeFrames, float inputGainDb,
               int captureDeviceID, char *mirrorPath,
               unsigned int mirrorFormat, unsigned int mirrorBitsPerSample,
               unsigned int *actualSampleRate, unsigned int *actualChannels,
               uint64_t *sessionStartHostTimeNanos,
               uint64_t *captureStartHostTimeNanos,
               unsigned int *actualMirrorFormat, unsigned int *mirrorActive);

  FFI_PLUGIN_EXPORT enum PlayerErrors startCaptureAndPlay(
      char *path, unsigned int soundHash, unsigned int busId,
      unsigned int sampleRate, unsigned int channels,
      unsigned int bufferSizeFrames, float volume, float pan,
      double startAtSeconds, bool looping, double loopingStartAt,
      float inputGainDb, int captureDeviceID, char *mirrorPath,
      unsigned int mirrorFormat, unsigned int mirrorBitsPerSample,
      unsigned int *handle, unsigned int *actualSampleRate,
      unsigned int *actualChannels, uint64_t *sessionStartHostTimeNanos,
      uint64_t *captureStartHostTimeNanos,
      uint64_t *playbackStartHostTimeNanos, unsigned int *actualMirrorFormat,
      unsigned int *mirrorActive);

  FFI_PLUGIN_EXPORT enum PlayerErrors
  stopCapture(unsigned int *sampleRate, unsigned int *channels,
              uint64_t *frameCount, uint64_t *sessionStartHostTimeNanos,
              uint64_t *captureStartHostTimeNanos,
              uint64_t *firstInputBufferHostTimeNanos,
              uint64_t *firstInputBufferFrameIndex,
              uint64_t *captureStopHostTimeNanos, unsigned int *mirrorFormat,
              unsigned int *mirrorSucceeded, uint64_t *mirrorFrameCount,
              uint64_t *writerOverflowFrames, uint64_t *writerSilenceFrames,
              unsigned int *writerFailed);

  FFI_PLUGIN_EXPORT enum PlayerErrors cancelCapture();

  FFI_PLUGIN_EXPORT int isCaptureRecording();

  FFI_PLUGIN_EXPORT enum PlayerErrors
  getCaptureClockSnapshot(uint64_t *hostTimeNanos,
                          uint64_t *sessionStartHostTimeNanos,
                          unsigned int *sampleRate,
                          uint64_t *inputDeviceFrame);

  FFI_PLUGIN_EXPORT enum PlayerErrors
  getCaptureLevelSnapshot(float *currentPeak, float *currentRms,
                          float *peakSinceLastRead, float *heldPeak,
                          uint64_t *frameCount);

  // Voice groups: one-deadline group start.

  /// Atomically schedule all prepared members of a voice group at one
  /// absolute engine deadline. Any failure leaves every member unchanged.
  FFI_PLUGIN_EXPORT enum VoiceGroupStartResult scheduleVoiceGroupStartAt(
      unsigned int voiceGroupHandle, unsigned int requiredMainHandle,
      int expectedMemberCount, double engineDeadline);

  // ---------------------------------------------------------------------------
  // CLAP plugin hosting (src/clap_bindings.cpp). Declared on every native
  // platform; without SOLOUD_ENABLE_CLAP (the `enable_clap` user define, macOS
  // only) the functions are inert stubs that initialise every output. The web
  // build does not compile clap_bindings.cpp, so they are not declared there.
  // ---------------------------------------------------------------------------
#ifndef __EMSCRIPTEN__

  /// Returns 1 if flutter_soloud was compiled with SOLOUD_ENABLE_CLAP (macOS
  /// only), or 0 otherwise.
  FFI_PLUGIN_EXPORT int soloud_clapIsSupported(void);

  /// Writes the CLAP version of the headers this host was built against, or
  /// 0.0.0 when CLAP hosting is not compiled in. Null outputs are skipped.
  FFI_PLUGIN_EXPORT void soloud_clapGetHostClapVersion(
      unsigned int *outMajor,
      unsigned int *outMinor,
      unsigned int *outRevision);

  /// Lists the plugins of the .clap bundle at [bundlePath] as parallel arrays
  /// of [outCount] strings: plugin ids, names, vendors, versions and features
  /// (joined with ';'). Free them with soloud_clapFreeScanResults(). Every
  /// output is cleared first. Callable from any non-audio thread that holds
  /// no lock the main thread may need: the scan runs on, and waits for, the
  /// main thread.
  FFI_PLUGIN_EXPORT enum PlayerErrors soloud_clapScanBundle(
      const char *bundlePath,
      int *outCount,
      char ***outIds,
      char ***outNames,
      char ***outVendors,
      char ***outVersions,
      char ***outFeatures);

  /// Frees the arrays returned by soloud_clapScanBundle(). Null arrays are
  /// skipped.
  FFI_PLUGIN_EXPORT void soloud_clapFreeScanResults(
      int count,
      char **ids,
      char **names,
      char **vendors,
      char **versions,
      char **features);

  /// Creates, inits and activates the audio effect [pluginId] of the bundle
  /// at [bundlePath] for [sampleRate] Hz, blocks of up to [maxBlockSize]
  /// (1..65536) frames and a bus of [channels] (1..32). Writes a positive
  /// handle to [outPluginHandle] on success and 0 otherwise. Callable from any
  /// non-audio thread that holds no lock the main thread may need: the plugin
  /// calls run on, and wait for, the main thread.
  FFI_PLUGIN_EXPORT enum PlayerErrors soloud_clapLoadEffect(
      const char *bundlePath,
      const char *pluginId,
      unsigned int sampleRate,
      unsigned int maxBlockSize,
      unsigned int channels,
      int *outPluginHandle);

  /// Unloads [pluginHandle]: the audio thread stops using it at once and the
  /// plugin is torn down on the main thread. invalidParameter for an unknown
  /// or already unloaded handle. Callable from any non-audio thread that
  /// holds no lock the main thread may need: it waits for the audio-thread
  /// barrier and the main thread.
  /// Like soloud_clapLoadEffect(), for the instrument [pluginId]: render it
  /// as a custom audio source with soloud_clapGetSourceCallbackPtr() and play
  /// it with soloud_clapSendNote(). Its audio inputs are silent. [channels] is
  /// the source's channel count (1..32); channels past the plugin's main
  /// output are silent.
  FFI_PLUGIN_EXPORT enum PlayerErrors soloud_clapLoadInstrument(
      const char *bundlePath,
      const char *pluginId,
      unsigned int sampleRate,
      unsigned int maxBlockSize,
      unsigned int channels,
      int *outPluginHandle);

  FFI_PLUGIN_EXPORT enum PlayerErrors soloud_clapUnloadPlugin(int pluginHandle);

  /// Unloads all hosted CLAP plugins and releases their modules. Same thread
  /// rules as soloud_clapUnloadPlugin(). A no-op when nothing is loaded.
  FFI_PLUGIN_EXPORT void soloud_clapHostUnloadAll(void);

  /// Returns `clapFilterDspCallback` (a `SoLoudFilterDspCallback_t`) for
  /// `CustomFilterSlot.setNativeCallback(callbackAddress: ...)`, or null when
  /// CLAP hosting is not compiled in.
  FFI_PLUGIN_EXPORT void *soloud_clapGetFilterCallbackPtr(void);

  /// Returns the `userdataAddress` for `CustomFilterSlot.setNativeCallback`
  /// of [pluginHandle] (the handle itself, so a stale one is a no-op), or null
  /// for a non-positive handle or when CLAP hosting is not compiled in.
  FFI_PLUGIN_EXPORT void *soloud_clapGetPluginUserData(int pluginHandle);

  /// Returns `clapSourceDspCallback` (a `SoLoudSourceDspCallback_t`) for
  /// `createCustomAudioSource(dspCallbackAddress: ...)` with the
  /// soloud_clapGetPluginUserData() of an instrument, or null when CLAP
  /// hosting is not compiled in. The source ends once the instrument is
  /// unloaded; it is silent for an effect handle.
  FFI_PLUGIN_EXPORT void *soloud_clapGetSourceCallbackPtr(void);

  /// Queues a note on ([isOn] != 0) or off for [key] (0..127) on [channel]
  /// (0..15) of note port 0, at [sampleOffset] frames into the next rendered
  /// block (clamped to the block). [velocity] (0..1, clamped) is sent as is
  /// to a CLAP-dialect plugin and scaled to 1..127 (on) or 0..127 (off) for
  /// a MIDI one. Any non-audio thread; never waits for the main thread.
  /// invalidParameter for an unknown handle, a value out of range or a
  /// plugin without a supported note dialect; outOfMemory when the queue
  /// (1024 notes) is full.
  FFI_PLUGIN_EXPORT enum PlayerErrors soloud_clapSendNote(
      int pluginHandle,
      int isOn,
      int channel,
      int key,
      double velocity,
      unsigned int sampleOffset);

  /// The dialect of note port 0 (ClapNoteDialect), or clapNoteDialectNone for
  /// an unknown handle or a plugin without a note port this host speaks. It
  /// can change when the plugin restarts. Any thread.
  FFI_PLUGIN_EXPORT int soloud_clapGetNoteDialect(int pluginHandle);

  // --- parameters, state, latency, events (plan Phase 3) ----------------------
  // Every output is initialised on every path. Strings are NUL-terminated and
  // truncated to their buffer; a buffer length <= 0 skips that string. An
  // unknown or unloaded handle gives invalidParameter (0 for the counts).
  // "Main hop" calls run on, and wait for, the main thread: call them from a
  // non-audio thread that holds no lock the main thread may need. A main hop
  // returns unknownError when the instance is busy, that is, when it is
  // called re-entrantly from inside one of the plugin's own calls.

  /// The plugin's latency in frames, or 0 (no latency extension, unknown
  /// handle). Any thread.
  FFI_PLUGIN_EXPORT unsigned int soloud_clapGetLatencySamples(int pluginHandle);

  /// The number of parameters, or 0 while the list is being rescanned (a
  /// clapEventParamsRescan follows). Any thread.
  FFI_PLUGIN_EXPORT int soloud_clapGetParameterCount(int pluginHandle);

  /// Describes the parameter at [paramIndex] (0..count-1): its id, name,
  /// module path, plain range, default and CLAP_PARAM_* flags. Any thread.
  /// invalidParameter for an index out of range or while rescanning.
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
      unsigned int *outFlags);

  /// The plugin's current plain value of [paramId]. Main hop.
  FFI_PLUGIN_EXPORT enum PlayerErrors soloud_clapGetParameterValue(
      int pluginHandle,
      unsigned int paramId,
      double *outValue);

  /// Formats [value] of [paramId] the plugin's way. Main hop.
  FFI_PLUGIN_EXPORT enum PlayerErrors soloud_clapParameterValueToText(
      int pluginHandle,
      unsigned int paramId,
      double value,
      char *outText,
      int textLen);

  /// Parses [text] into a plain value of [paramId]. Main hop.
  FFI_PLUGIN_EXPORT enum PlayerErrors soloud_clapParameterTextToValue(
      int pluginHandle,
      unsigned int paramId,
      const char *text,
      double *outValue);

  /// Queues [plainValue] (clamped to the range) for [paramId]; the plugin
  /// receives it with its next processed block, or a flush while it does not
  /// process. Any non-audio thread; never waits for the main thread.
  /// unknownError while the list is being rescanned or the queue is full.
  FFI_PLUGIN_EXPORT enum PlayerErrors soloud_clapSetParameterValue(
      int pluginHandle,
      unsigned int paramId,
      double plainValue);

  /// Makes parameter [slotParamIndex] (1..15) of the plugin's filter slot
  /// drive [paramId]: each change of the slot value (0..1, from a set, fade
  /// or oscillation) reaches the plugin at the start of the next processed
  /// block, mapped linearly onto the parameter's range (stepped, enum and
  /// bypass parameters rounded). [slotValue] is the slot value the caller
  /// read just before mapping: the first processed block sends the slot
  /// value only when it maps to a different plain value, even if blocks were
  /// skipped in between. NaN when unknown: the standing value is then not
  /// sent unless the slot changed since the last processed block (best
  /// effort). A later parameter-list change re-resolves the mapping, or
  /// drops it when the parameter is gone. invalidParameter for an unknown or
  /// read-only parameter, unknownError while the list is being rescanned.
  /// Any non-audio thread.
  FFI_PLUGIN_EXPORT enum PlayerErrors soloud_clapMapSlotParameter(
      int pluginHandle,
      unsigned int slotParamIndex,
      unsigned int paramId,
      float slotValue);

  /// Stops forwarding filter-slot parameter [slotParamIndex] (1..15).
  FFI_PLUGIN_EXPORT enum PlayerErrors soloud_clapUnmapSlotParameter(
      int pluginHandle,
      unsigned int slotParamIndex);

  /// Saves the plugin's state. On success [outData] is malloc'd: free it with
  /// nativeFree(). An empty state gives null data and size 0. notImplemented
  /// when the plugin has no state extension. Main hop.
  FFI_PLUGIN_EXPORT enum PlayerErrors soloud_clapGetState(
      int pluginHandle,
      unsigned char **outData,
      unsigned int *outSize);

  /// Loads a state saved by soloud_clapGetState(). Parameter edits queued
  /// before the call are dropped, and a clapEventParamsRescan (values, text)
  /// follows. Main hop.
  FFI_PLUGIN_EXPORT enum PlayerErrors soloud_clapSetState(
      int pluginHandle,
      const unsigned char *data,
      unsigned int size);

  /// Takes up to [capacity] pending events into the parallel arrays (types
  /// from ClapHostEventType) and writes how many to [outCount]. Call it again
  /// while it fills the arrays. Any non-audio thread, one at a time per
  /// handle; never waits for the main thread.
  FFI_PLUGIN_EXPORT enum PlayerErrors soloud_clapDrainEvents(
      int pluginHandle,
      int capacity,
      int *outTypes,
      unsigned int *outParamIds,
      double *outValues,
      unsigned int *outFlags,
      int *outCount);

  /// 1 when the plugin offers a cocoa gui (embedded or floating), else 0.
  /// Any thread.
  FFI_PLUGIN_EXPORT int soloud_clapHasEditor(int pluginHandle);

  /// Opens the plugin's gui: embedded in a host window titled [windowTitle],
  /// or the plugin's own floating window. An open editor comes to the front.
  /// invalidParameter without a gui or for an unknown handle; unknownError
  /// when the plugin fails or is busy. Waits for the main thread.
  FFI_PLUGIN_EXPORT enum PlayerErrors soloud_clapOpenEditor(
      int pluginHandle,
      const char *windowTitle);

  /// Closes and destroys the gui if open. No clapEventEditorClosed follows.
  /// Waits for the main thread.
  FFI_PLUGIN_EXPORT enum PlayerErrors soloud_clapCloseEditor(int pluginHandle);

  /// 1 while the gui is on screen, else 0. Any thread.
  FFI_PLUGIN_EXPORT int soloud_clapIsEditorOpen(int pluginHandle);
#endif // __EMSCRIPTEN__

#ifdef __cplusplus
}
#endif

#endif // FORK_BINDINGS_H
