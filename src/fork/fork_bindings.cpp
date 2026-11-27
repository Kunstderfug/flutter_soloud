// The fork-only FFI_PLUGIN_EXPORT entry points declared in fork_bindings.h
// (CLAP hosting is in src/clap_bindings.cpp). Moved out of src/bindings.cpp
// unchanged: each reaches the engine only through bindings_internal.h, which
// keeps the admission and lifetime rules it had there.
#include "fork_bindings.h"

#include "bindings_internal.h"
#include "../player.h"
#include "../plugin_audio_barrier.h"
#ifdef SOLOUD_ENABLE_CLAP
#include "../clap/clap_host.h"
#endif

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace
{
  /// Stateless: every use reads bindings.cpp's `player` afresh.
  const soloud_fork::EnginePlayer gPlayer{};

#if defined(SOLOUD_LIFECYCLE_TEST_HOOKS)
  /// Dispose calls these through soloud_fork::onDispose*, inert unless a test
  /// arms them: the first while it holds player_lifetime_mutex exclusively,
  /// the second where it shuts the plugin hosts down after releasing it.
  std::atomic<void (*)()> gDisposeUnderLifetimeLockHook{nullptr};
  std::atomic<void (*)()> gDisposePluginShutdownHook{nullptr};

  void soloudTestDisposeUnderLifetimeLock()
  {
    if (auto hook = gDisposeUnderLifetimeLockHook.load(std::memory_order_acquire))
      hook();
  }

  void soloudTestDisposePluginShutdown()
  {
    if (auto hook = gDisposePluginShutdownHook.load(std::memory_order_acquire))
      hook();
  }
#endif
} // namespace

namespace soloud_fork
{
  void onDisposeUnderLifetimeLock()
  {
#ifdef SOLOUD_ENABLE_CLAP
    // Retire-only: CLAP lifecycle calls belong to the main thread, so the
    // teardown is posted there. No mix can reach a retired instance.
    soloud_clap::ClapHostManager::instance().retireAllForDispose();
#endif
#if defined(SOLOUD_LIFECYCLE_TEST_HOOKS)
    soloudTestDisposeUnderLifetimeLock();
#endif
  }

  void onDisposePluginShutdown()
  {
#if defined(SOLOUD_LIFECYCLE_TEST_HOOKS)
    soloudTestDisposePluginShutdown();
#endif
  }
} // namespace soloud_fork

#ifdef __cplusplus
extern "C"
{
#endif

  /////////////////////////////////////////
  /// Engine power
  /////////////////////////////////////////

  FFI_PLUGIN_EXPORT bool requestAudioDeviceIdleStop()
  {
    // Never queue a delayed FFI operation: its "idle" observation could be
    // stale by the time it reaches Player. Admission is synchronous and cheap.
    soloud_fork::InitDeinitTryLock guard;
    if (!guard.ownsLock() || !gPlayer)
      return false;
    return gPlayer->requestAudioDeviceIdleStop();
  }

  /////////////////////////////////////////
  /// Native capture (src/capture/)
  /////////////////////////////////////////

  FFI_PLUGIN_EXPORT void listCaptureDevices(char **devicesName, int **deviceId,
                                            int **isDefault, int *n_devices)
  {
    std::vector<CaptureDevice> d = Player::listCaptureDevices();

    int numDevices = 0;
    for (int i = 0; i < (int)d.size(); i++)
    {
      bool hasSpecialChar = false;
      const size_t nameLength = strlen(d[i].name);
      for (int n = 0; n < 5 && n < (int)nameLength; n++)
      {
        if (d[i].name[n] < 0x20 && d[i].name[n] >= 0)
          hasSpecialChar = true;
      }
      if (nameLength == 0 || hasSpecialChar)
      {
        free(d[i].name);
        continue;
      }

      devicesName[numDevices] = strdup(d[i].name);
      isDefault[numDevices] = (int *)malloc(sizeof(int));
      *isDefault[numDevices] = d[i].isDefault;
      deviceId[numDevices] = (int *)malloc(sizeof(int));
      *deviceId[numDevices] = d[i].id;
      free(d[i].name);
      numDevices++;
    }
    *n_devices = numDevices;
  }

  FFI_PLUGIN_EXPORT void freeListCaptureDevices(
      char **devicesName, int **deviceId, int **isDefault, int n_devices)
  {
    for (int i = 0; i < n_devices; i++)
    {
      free(deviceId[i]);
      free(isDefault[i]);
      free(devicesName[i]);
    }
  }

  FFI_PLUGIN_EXPORT enum PlayerErrors
  startCapture(char *path, unsigned int sampleRate, unsigned int channels,
               unsigned int bufferSizeFrames, float inputGainDb,
               int captureDeviceID, char *mirrorPath,
               unsigned int mirrorFormat, unsigned int mirrorBitsPerSample,
               unsigned int *actualSampleRate, unsigned int *actualChannels,
               uint64_t *sessionStartHostTimeNanos,
               uint64_t *captureStartHostTimeNanos,
               unsigned int *actualMirrorFormat, unsigned int *mirrorActive)
  {
    if (!gPlayer)
      return backendNotInited;
    if (path == nullptr || actualSampleRate == nullptr ||
        actualChannels == nullptr || sessionStartHostTimeNanos == nullptr ||
        captureStartHostTimeNanos == nullptr || actualMirrorFormat == nullptr ||
        mirrorActive == nullptr)
      return nullPointer;

    CaptureStartInfo info;
    PlayerErrors result = gPlayer->startCapture(
        std::string(path), sampleRate, channels, bufferSizeFrames, inputGainDb,
        captureDeviceID,
        mirrorPath == nullptr ? std::string() : std::string(mirrorPath),
        mirrorFormat, mirrorBitsPerSample, &info);
    if (result == noError)
    {
      *actualSampleRate = info.sampleRate;
      *actualChannels = info.channels;
      *sessionStartHostTimeNanos = info.sessionStartHostTimeNanos;
      *captureStartHostTimeNanos = info.captureStartHostTimeNanos;
      *actualMirrorFormat = info.mirrorFormat;
      *mirrorActive = info.mirrorActive ? 1 : 0;
    }
    return result;
  }

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
      unsigned int *mirrorActive)
  {
    if (!gPlayer)
      return backendNotInited;
    if (path == nullptr || handle == nullptr || actualSampleRate == nullptr ||
        actualChannels == nullptr || sessionStartHostTimeNanos == nullptr ||
        captureStartHostTimeNanos == nullptr ||
        playbackStartHostTimeNanos == nullptr ||
        actualMirrorFormat == nullptr || mirrorActive == nullptr)
      return nullPointer;

    CapturePlaybackStartInfo info;
    PlayerErrors result = gPlayer->startCaptureAndPlay(
        std::string(path), soundHash, busId, sampleRate, channels,
        bufferSizeFrames, volume, pan, startAtSeconds, looping, loopingStartAt,
        inputGainDb, captureDeviceID,
        mirrorPath == nullptr ? std::string() : std::string(mirrorPath),
        mirrorFormat, mirrorBitsPerSample, &info);
    if (result == noError)
    {
      *handle = info.handle;
      *actualSampleRate = info.sampleRate;
      *actualChannels = info.channels;
      *sessionStartHostTimeNanos = info.sessionStartHostTimeNanos;
      *captureStartHostTimeNanos = info.captureStartHostTimeNanos;
      *playbackStartHostTimeNanos = info.playbackStartHostTimeNanos;
      *actualMirrorFormat = info.mirrorFormat;
      *mirrorActive = info.mirrorActive ? 1 : 0;
    }
    return result;
  }

  FFI_PLUGIN_EXPORT enum PlayerErrors
  stopCapture(unsigned int *sampleRate, unsigned int *channels,
              uint64_t *frameCount, uint64_t *sessionStartHostTimeNanos,
              uint64_t *captureStartHostTimeNanos,
              uint64_t *firstInputBufferHostTimeNanos,
              uint64_t *firstInputBufferFrameIndex,
              uint64_t *captureStopHostTimeNanos, unsigned int *mirrorFormat,
              unsigned int *mirrorSucceeded, uint64_t *mirrorFrameCount,
              uint64_t *writerOverflowFrames, uint64_t *writerSilenceFrames,
              unsigned int *writerFailed)
  {
    if (!gPlayer)
      return backendNotInited;
    if (sampleRate == nullptr || channels == nullptr || frameCount == nullptr ||
        sessionStartHostTimeNanos == nullptr ||
        captureStartHostTimeNanos == nullptr ||
        firstInputBufferHostTimeNanos == nullptr ||
        firstInputBufferFrameIndex == nullptr ||
        captureStopHostTimeNanos == nullptr || mirrorFormat == nullptr ||
        mirrorSucceeded == nullptr || mirrorFrameCount == nullptr ||
        writerOverflowFrames == nullptr || writerSilenceFrames == nullptr ||
        writerFailed == nullptr)
      return nullPointer;

    CaptureStopInfo info;
    PlayerErrors result = gPlayer->stopCapture(&info);
    if (result == noError)
    {
      *sampleRate = info.sampleRate;
      *channels = info.channels;
      *frameCount = info.frameCount;
      *sessionStartHostTimeNanos = info.sessionStartHostTimeNanos;
      *captureStartHostTimeNanos = info.captureStartHostTimeNanos;
      *firstInputBufferHostTimeNanos = info.firstInputBufferHostTimeNanos;
      *firstInputBufferFrameIndex = info.firstInputBufferFrameIndex;
      *captureStopHostTimeNanos = info.captureStopHostTimeNanos;
      *mirrorFormat = info.mirrorFormat;
      *mirrorSucceeded = info.mirrorSucceeded ? 1 : 0;
      *mirrorFrameCount = info.mirrorFrameCount;
      *writerOverflowFrames = info.writerOverflowFrames;
      *writerSilenceFrames = info.writerSilenceFrames;
      *writerFailed = info.writerFailed ? 1 : 0;
    }
    return result;
  }

  FFI_PLUGIN_EXPORT enum PlayerErrors cancelCapture()
  {
    if (!gPlayer)
      return backendNotInited;
    return gPlayer->cancelCapture();
  }

  FFI_PLUGIN_EXPORT int isCaptureRecording()
  {
    if (!gPlayer)
      return 0;
    return gPlayer->isCaptureRecording() ? 1 : 0;
  }

  FFI_PLUGIN_EXPORT enum PlayerErrors
  getCaptureClockSnapshot(uint64_t *hostTimeNanos,
                          uint64_t *sessionStartHostTimeNanos,
                          unsigned int *sampleRate,
                          uint64_t *inputDeviceFrame)
  {
    if (!gPlayer)
      return backendNotInited;
    if (hostTimeNanos == nullptr || sessionStartHostTimeNanos == nullptr ||
        sampleRate == nullptr || inputDeviceFrame == nullptr)
      return nullPointer;

    CaptureClockInfo info;
    PlayerErrors result = gPlayer->getCaptureClockSnapshot(&info);
    if (result == noError)
    {
      *hostTimeNanos = info.hostTimeNanos;
      *sessionStartHostTimeNanos = info.sessionStartHostTimeNanos;
      *sampleRate = info.sampleRate;
      *inputDeviceFrame = info.inputDeviceFrame;
    }
    return result;
  }

  FFI_PLUGIN_EXPORT enum PlayerErrors getCaptureLevelSnapshot(
      float *currentPeak, float *currentRms, float *peakSinceLastRead,
      float *heldPeak, uint64_t *frameCount)
  {
    if (!gPlayer)
      return backendNotInited;
    if (currentPeak == nullptr || currentRms == nullptr ||
        peakSinceLastRead == nullptr || heldPeak == nullptr ||
        frameCount == nullptr)
      return nullPointer;

    CaptureLevelInfo info;
    PlayerErrors result = gPlayer->getCaptureLevelSnapshot(&info);
    if (result == noError)
    {
      *currentPeak = info.currentPeak;
      *currentRms = info.currentRms;
      *peakSinceLastRead = info.peakSinceLastRead;
      *heldPeak = info.heldPeak;
      *frameCount = info.frameCount;
    }
    return result;
  }

  /////////////////////////////////////////
  /// Voice groups
  /////////////////////////////////////////

  FFI_PLUGIN_EXPORT enum VoiceGroupStartResult scheduleVoiceGroupStartAt(
      unsigned int voiceGroupHandle, unsigned int requiredMainHandle,
      int expectedMemberCount, double engineDeadline)
  {
    if (!gPlayer || !gPlayer->isInited())
      return voiceGroupStartBackendNotInitialized;
    return gPlayer->scheduleVoiceGroupStartAt(
        voiceGroupHandle, requiredMainHandle, expectedMemberCount,
        engineDeadline);
  }

  /////////////////////////////////////////
  /// Plugin audio barrier (plugin_audio_barrier.h)
  /////////////////////////////////////////

#if defined(SOLOUD_ENABLE_VST3) || defined(SOLOUD_ENABLE_CLAP)
  /// Audio-thread barrier for plugin unloading (VST3 and CLAP): returns once
  /// no mix that may have loaded a plugin pointer from a lookup table is
  /// still running. Holds the lifetime lock only for the barrier itself,
  /// never across plugin shutdown (which can close editor windows).
  ///
  /// Deliberately not a PlayerLease: a lease gives up while a teardown is
  /// pending, and a barrier must never give up -- the device may still be
  /// mixing until the teardown actually stops it. Taking the shared lock
  /// unconditionally guarantees one of two outcomes: either this runs before
  /// the teardown and synchronizes with the live mixer, or it waits for the
  /// teardown to finish, by which point the device is stopped and the new
  /// Player is not initialized, so nothing is mixing.
  void soloud_pluginAudioThreadBarrier(void * /*userData*/)
  {
    soloud_fork::PlayerLifetimeSharedLock lifetime;
    if (gPlayer && gPlayer->isInited())
    {
      // Mixing (render worker or device callback) and the post-ring global
      // filters (device callback) each run under their own lock.
      gPlayer->soloud.audioThreadBarrier_internal();
    }
  }
#endif

#if defined(SOLOUD_LIFECYCLE_TEST_HOOKS)
  /////////////////////////////////////////
  /// Test hooks
  /////////////////////////////////////////

  /// Arms (or, with nulls, disarms) the two dispose hooks above.
  FFI_PLUGIN_EXPORT void soloudTestSetDisposeHooks(void (*underLifetimeLock)(),
                                                   void (*pluginShutdown)())
  {
    gDisposeUnderLifetimeLockHook.store(underLifetimeLock,
                                        std::memory_order_release);
    gDisposePluginShutdownHook.store(pluginShutdown, std::memory_order_release);
  }
#endif

#ifdef __cplusplus
}
#endif
