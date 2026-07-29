// Standalone correctness tests for atomic scheduled voice-group starts.
//
// Build and run from the flutter_soloud repository root with:
//
//   ./test/run_voice_group_start_test.sh

#include "soloud.h"
#include "soloud_audiosource.h"
#include "soloud_bus.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <thread>
#include <vector>

namespace {

constexpr unsigned int kSampleRate = 1024;
constexpr unsigned int kRenderFrames = 256;
constexpr unsigned int kSentinelDelay = 41;

int failures = 0;
std::atomic<bool> mixReachedMutexBoundary{false};
std::atomic<bool> releaseMixMutexBoundary{false};

void pauseBeforeMixMutexLock() {
  mixReachedMutexBoundary.store(true, std::memory_order_release);
  while (!releaseMixMutexBoundary.load(std::memory_order_acquire)) {
    std::this_thread::yield();
  }
}

#define EXPECT(condition, format, ...)                                         \
  do {                                                                         \
    if (!(condition)) {                                                        \
      ++failures;                                                              \
      std::fprintf(stderr, "FAIL [%s:%d] " format "\n", __FILE__, __LINE__,   \
                   ##__VA_ARGS__);                                             \
    }                                                                          \
  } while (0)

class ConstantInstance final : public SoLoud::AudioSourceInstance {
public:
  unsigned int getAudio(float *buffer, unsigned int samplesToRead,
                        unsigned int bufferSize) override {
    for (unsigned int channel = 0; channel < mChannels; ++channel) {
      for (unsigned int sample = 0; sample < samplesToRead; ++sample) {
        buffer[channel * bufferSize + sample] = 0.25f;
      }
    }
    return samplesToRead;
  }

  bool hasEnded() override { return false; }
};

class ConstantSource final : public SoLoud::AudioSource {
public:
  ConstantSource() {
    mBaseSamplerate = static_cast<float>(kSampleRate);
    mChannels = 1;
  }

  SoLoud::AudioSourceInstance *createInstance() override {
    return new ConstantInstance();
  }
};

struct Rig {
  Rig() {
    const SoLoud::result result =
        engine.init(SoLoud::Soloud::CLIP_ROUNDOFF,
                    SoLoud::Soloud::NULLDRIVER, kSampleRate, 512, 2);
    EXPECT(result == SoLoud::SO_NO_ERROR,
           "null backend initialization failed: %u", result);
    engine.setMainResampler(SoLoud::Soloud::RESAMPLER_POINT);
  }

  ~Rig() { engine.deinit(); }

  SoLoud::handle pausedVoice(SoLoud::AudioSource &audioSource,
                             unsigned int bus = 0) {
    const SoLoud::handle handle =
        engine.play(audioSource, 1.0f, 0.0f, true, bus);
    EXPECT(handle != 0, "play returned an invalid handle");
    return handle;
  }

  std::vector<float> mix(unsigned int frames = kRenderFrames) {
    std::vector<float> output(frames * 2, 0.0f);
    engine.mix(output.data(), frames);
    return output;
  }

  void expectSilent(const char *context) {
    const std::vector<float> output = mix();
    for (float sample : output) {
      if (std::fabs(sample) > 0.0001f) {
        EXPECT(false, "%s emitted sample %.6f", context, sample);
        break;
      }
    }
  }

  void expectPreparedVoiceUnchanged(SoLoud::handle handle,
                                    const char *context) {
    EXPECT(engine.getPause(handle), "%s was unpaused on failure", context);
    expectSilent(context);
    engine.setPause(handle, false);
    const std::vector<float> output = mix();
    unsigned int onset = kRenderFrames;
    for (unsigned int frame = 0; frame < kRenderFrames; ++frame) {
      if (std::fabs(output[frame * 2]) > 0.0001f ||
          std::fabs(output[frame * 2 + 1]) > 0.0001f) {
        onset = frame;
        break;
      }
    }
    EXPECT(onset == kSentinelDelay,
           "%s delay mutated: onset %u, expected %u", context, onset,
           kSentinelDelay);
  }

  void expectPreparedPairUnchanged(SoLoud::handle main,
                                   SoLoud::handle layer,
                                   const char *context) {
    EXPECT(engine.getPause(main), "%s main was unpaused on failure", context);
    EXPECT(engine.getPause(layer), "%s layer was unpaused on failure", context);
    expectSilent(context);

    engine.setPanAbsolute(main, 1.0f, 0.0f);
    engine.setPanAbsolute(layer, 0.0f, 1.0f);
    engine.setPause(main, false);
    engine.setPause(layer, false);
    const std::vector<float> output = mix();
    unsigned int mainOnset = kRenderFrames;
    unsigned int layerOnset = kRenderFrames;
    for (unsigned int frame = 0; frame < kRenderFrames; ++frame) {
      if (mainOnset == kRenderFrames &&
          std::fabs(output[frame * 2]) > 0.0001f) {
        mainOnset = frame;
      }
      if (layerOnset == kRenderFrames &&
          std::fabs(output[frame * 2 + 1]) > 0.0001f) {
        layerOnset = frame;
      }
    }
    EXPECT(mainOnset == kSentinelDelay,
           "%s main delay mutated: onset %u, expected %u", context, mainOnset,
           kSentinelDelay);
    EXPECT(layerOnset == kSentinelDelay,
           "%s layer delay mutated: onset %u, expected %u", context,
           layerOnset, kSentinelDelay);
  }

  ConstantSource source;
  SoLoud::Soloud engine;
};

void testMixerClockAdvancesOnlyAfterAcquiringAudioMutex() {
  Rig rig;
  const SoLoud::time initialTime = rig.engine.mStreamTime;
  mixReachedMutexBoundary.store(false, std::memory_order_relaxed);
  releaseMixMutexBoundary.store(false, std::memory_order_relaxed);
  rig.engine.mBeforeMixMutexLockCallback = pauseBeforeMixMutexLock;

  rig.engine.lockAudioMutex_internal();
  std::atomic<bool> mixFinished{false};
  std::thread mixThread([&rig, &mixFinished]() {
    rig.mix(64);
    mixFinished.store(true, std::memory_order_release);
  });

  while (!mixReachedMutexBoundary.load(std::memory_order_acquire)) {
    std::this_thread::yield();
  }
  EXPECT(rig.engine.mStreamTime == initialTime,
         "engine clock advanced from %.6f to %.6f before acquiring mutex",
         initialTime, rig.engine.mStreamTime);

  releaseMixMutexBoundary.store(true, std::memory_order_release);
  EXPECT(!mixFinished.load(std::memory_order_acquire),
         "mix completed while the audio mutex was held");
  rig.engine.unlockAudioMutex_internal();
  mixThread.join();

  EXPECT(rig.engine.mStreamTime ==
             initialTime + 64.0 / static_cast<double>(kSampleRate),
         "engine clock did not advance exactly one mixed buffer");
}

void testCommitsAllMembersToOneOnsetSample() {
  Rig rig;
  const SoLoud::handle main = rig.pausedVoice(rig.source);
  const SoLoud::handle layer = rig.pausedVoice(rig.source);
  rig.engine.setPanAbsolute(main, 1.0f, 0.0f);
  rig.engine.setPanAbsolute(layer, 0.0f, 1.0f);

  const SoLoud::handle group = rig.engine.createVoiceGroup();
  rig.engine.addVoiceToGroup(group, main);
  rig.engine.addVoiceToGroup(group, layer);

  constexpr unsigned int expectedDelay = 73;
  const SoLoud::time deadline =
      rig.engine.getEngineTime() +
      static_cast<double>(expectedDelay) / kSampleRate;
  const SoLoud::VoiceGroupStartResult result =
      rig.engine.scheduleVoiceGroupStartAt(group, main, 2, deadline);
  EXPECT(result == SoLoud::VOICE_GROUP_START_SUCCESS,
         "commit returned %d", static_cast<int>(result));

  const std::vector<float> output = rig.mix();
  unsigned int leftOnset = kRenderFrames;
  unsigned int rightOnset = kRenderFrames;
  for (unsigned int frame = 0; frame < kRenderFrames; ++frame) {
    if (leftOnset == kRenderFrames &&
        std::fabs(output[frame * 2]) > 0.0001f) {
      leftOnset = frame;
    }
    if (rightOnset == kRenderFrames &&
        std::fabs(output[frame * 2 + 1]) > 0.0001f) {
      rightOnset = frame;
    }
  }
  if (leftOnset == kRenderFrames) {
    EXPECT(false, "main produced no onset");
  } else {
    EXPECT(leftOnset == expectedDelay,
           "main onset %u (sample %.6f), expected %u", leftOnset,
           output[leftOnset * 2], expectedDelay);
  }
  if (rightOnset == kRenderFrames) {
    EXPECT(false, "layer produced no onset");
  } else {
    EXPECT(rightOnset == expectedDelay,
           "layer onset %u (sample %.6f), expected %u", rightOnset,
           output[rightOnset * 2 + 1], expectedDelay);
  }
}

void testCommitsGroupSpanningMainEngineAndBus() {
  Rig rig;
  SoLoud::Bus bus;
  const SoLoud::handle busHandle = rig.engine.play(bus);
  EXPECT(busHandle != 0, "bus play returned an invalid handle");
  const SoLoud::handle main = rig.pausedVoice(rig.source);
  const SoLoud::handle layer = bus.play(rig.source, 1.0f, 0.0f, true);
  EXPECT(layer != 0, "bus play returned an invalid layer handle");

  const SoLoud::handle group = rig.engine.createVoiceGroup();
  rig.engine.addVoiceToGroup(group, main);
  rig.engine.addVoiceToGroup(group, layer);
  const SoLoud::VoiceGroupStartResult result =
      rig.engine.scheduleVoiceGroupStartAt(
          group, main, 2, rig.engine.getEngineTime() + 0.5);

  EXPECT(result == SoLoud::VOICE_GROUP_START_SUCCESS,
         "cross-bus commit returned %d", static_cast<int>(result));
  EXPECT(!rig.engine.getPause(main), "cross-bus main remained paused");
  EXPECT(!rig.engine.getPause(layer), "cross-bus layer remained paused");
}

void testReachedDeadlineLeavesAllMembersSilentAndUnchanged() {
  Rig rig;
  const SoLoud::handle main = rig.pausedVoice(rig.source);
  const SoLoud::handle layer = rig.pausedVoice(rig.source);
  rig.engine.setDelaySamples(main, kSentinelDelay);
  const SoLoud::handle group = rig.engine.createVoiceGroup();
  rig.engine.addVoiceToGroup(group, main);
  rig.engine.addVoiceToGroup(group, layer);

  const SoLoud::VoiceGroupStartResult result =
      rig.engine.scheduleVoiceGroupStartAt(
          group, main, 2, rig.engine.getEngineTime());
  EXPECT(result == SoLoud::VOICE_GROUP_START_DEADLINE_REACHED,
         "deadline result was %d", static_cast<int>(result));
  EXPECT(rig.engine.getPause(layer), "layer was unpaused on deadline failure");
  rig.engine.stop(layer);
  rig.expectPreparedVoiceUnchanged(main, "deadline failure");
}

void testStoppedRequiredMainLeavesLayerSilentAndUnchanged() {
  Rig rig;
  const SoLoud::handle main = rig.pausedVoice(rig.source);
  const SoLoud::handle layer = rig.pausedVoice(rig.source);
  rig.engine.setDelaySamples(layer, kSentinelDelay);
  const SoLoud::handle group = rig.engine.createVoiceGroup();
  rig.engine.addVoiceToGroup(group, main);
  rig.engine.addVoiceToGroup(group, layer);
  rig.engine.stop(main);

  const SoLoud::VoiceGroupStartResult result =
      rig.engine.scheduleVoiceGroupStartAt(
          group, main, 2, rig.engine.getEngineTime() + 1.0);
  EXPECT(result == SoLoud::VOICE_GROUP_START_INVALID_MAIN,
         "invalid-main result was %d", static_cast<int>(result));
  rig.expectPreparedVoiceUnchanged(layer, "invalid main failure");
}

void testCountMismatchMakesNoMutation() {
  Rig rig;
  const SoLoud::handle main = rig.pausedVoice(rig.source);
  const SoLoud::handle layer = rig.pausedVoice(rig.source);
  rig.engine.setDelaySamples(main, kSentinelDelay);
  rig.engine.setDelaySamples(layer, kSentinelDelay);
  const SoLoud::handle group = rig.engine.createVoiceGroup();
  rig.engine.addVoiceToGroup(group, main);
  rig.engine.addVoiceToGroup(group, layer);

  const SoLoud::VoiceGroupStartResult result =
      rig.engine.scheduleVoiceGroupStartAt(
          group, main, 3, rig.engine.getEngineTime() + 1.0);
  EXPECT(result == SoLoud::VOICE_GROUP_START_MEMBER_COUNT_MISMATCH,
         "count-mismatch result was %d", static_cast<int>(result));
  rig.expectPreparedPairUnchanged(main, layer, "count mismatch failure");
}

void testInvalidMemberMakesNoMutation() {
  Rig rig;
  const SoLoud::handle main = rig.pausedVoice(rig.source);
  const SoLoud::handle layer = rig.pausedVoice(rig.source);
  rig.engine.setDelaySamples(main, kSentinelDelay);
  const SoLoud::handle group = rig.engine.createVoiceGroup();
  rig.engine.addVoiceToGroup(group, main);
  rig.engine.addVoiceToGroup(group, layer);
  rig.engine.stop(layer);

  const SoLoud::VoiceGroupStartResult result =
      rig.engine.scheduleVoiceGroupStartAt(
          group, main, 2, rig.engine.getEngineTime() + 1.0);
  EXPECT(result == SoLoud::VOICE_GROUP_START_INVALID_MEMBER,
         "invalid-member result was %d", static_cast<int>(result));
  rig.expectPreparedVoiceUnchanged(main, "invalid member failure");
}

void testNonPausedMemberMakesNoMutation() {
  Rig rig;
  const SoLoud::handle main = rig.pausedVoice(rig.source);
  const SoLoud::handle layer = rig.pausedVoice(rig.source);
  rig.engine.setDelaySamples(main, kSentinelDelay);
  const SoLoud::handle group = rig.engine.createVoiceGroup();
  rig.engine.addVoiceToGroup(group, main);
  rig.engine.addVoiceToGroup(group, layer);
  rig.engine.setPause(layer, false);

  const SoLoud::VoiceGroupStartResult result =
      rig.engine.scheduleVoiceGroupStartAt(
          group, main, 2, rig.engine.getEngineTime() + 1.0);
  EXPECT(result == SoLoud::VOICE_GROUP_START_MEMBER_NOT_PAUSED,
         "non-paused result was %d", static_cast<int>(result));
  rig.engine.stop(layer);
  rig.expectPreparedVoiceUnchanged(main, "non-paused member failure");
}

void testInvalidInputsAndEmptyGroupAreExplicit() {
  SoLoud::Soloud engine;
  EXPECT(engine.scheduleVoiceGroupStartAt(0, 0, 0, 0.0) ==
             SoLoud::VOICE_GROUP_START_BACKEND_NOT_INITIALIZED,
         "uninitialized engine was not distinguished");

  Rig rig;
  const SoLoud::handle main = rig.pausedVoice(rig.source);
  const SoLoud::handle emptyGroup = rig.engine.createVoiceGroup();
  EXPECT(rig.engine.scheduleVoiceGroupStartAt(
             emptyGroup, main, 1, rig.engine.getEngineTime() + 1.0) ==
             SoLoud::VOICE_GROUP_START_INVALID_GROUP,
         "empty group was not rejected");
  EXPECT(rig.engine.scheduleVoiceGroupStartAt(
             emptyGroup, main, 0, rig.engine.getEngineTime() + 1.0) ==
             SoLoud::VOICE_GROUP_START_INVALID_INPUT,
         "zero expected count was not rejected");
  EXPECT(rig.engine.scheduleVoiceGroupStartAt(
             emptyGroup, main, -1, rig.engine.getEngineTime() + 1.0) ==
             SoLoud::VOICE_GROUP_START_INVALID_INPUT,
         "negative expected count was not rejected");
  EXPECT(rig.engine.scheduleVoiceGroupStartAt(
             emptyGroup, main, VOICE_COUNT + 1,
             rig.engine.getEngineTime() + 1.0) ==
             SoLoud::VOICE_GROUP_START_INVALID_INPUT,
         "impossible expected count was not rejected");
  EXPECT(rig.engine.scheduleVoiceGroupStartAt(
             emptyGroup, main, 1,
             std::numeric_limits<double>::infinity()) ==
             SoLoud::VOICE_GROUP_START_INVALID_INPUT,
         "infinite deadline was not rejected");
}

} // namespace

int main() {
  std::printf("Test: atomic scheduled voice-group start\n");
  testMixerClockAdvancesOnlyAfterAcquiringAudioMutex();
  testCommitsAllMembersToOneOnsetSample();
  testCommitsGroupSpanningMainEngineAndBus();
  testReachedDeadlineLeavesAllMembersSilentAndUnchanged();
  testStoppedRequiredMainLeavesLayerSilentAndUnchanged();
  testCountMismatchMakesNoMutation();
  testInvalidMemberMakesNoMutation();
  testNonPausedMemberMakesNoMutation();
  testInvalidInputsAndEmptyGroupAreExplicit();

  if (failures != 0) {
    std::fprintf(stderr, "%d assertion(s) failed\n", failures);
    return EXIT_FAILURE;
  }
  std::printf("PASS\n");
  return EXIT_SUCCESS;
}
