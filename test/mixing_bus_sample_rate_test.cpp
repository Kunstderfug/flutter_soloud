// Standalone regression test for mixing-bus sample-rate alignment.
//
// Build and run from the flutter_soloud repository root with:
//
//   ./test/run_mixing_bus_sample_rate_test.sh

// The test uses SoLoud's null backend and a deterministic 48 kHz pulse source.
// It exercises BusData, the exact object constructed by Player::createBus().

#include "../src/filters/filters.h"
#include "../src/soloud/include/soloud_bus.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

// BusData owns a Filters object, but this test does not activate filters.
// Defining only the constructor keeps the standalone binary focused on the
// mixing path without linking every plugin filter implementation.
Filters::Filters(SoLoud::Soloud *soloud, ActiveSound *sound, BusData *busData)
    : mSoloud(soloud), mSound(sound), mBusData(busData) {}

namespace {

constexpr unsigned int kSampleRate = 48000;
constexpr unsigned int kPulseIntervalFrames = 4800;
constexpr unsigned int kChunkFrames = 256;
constexpr unsigned int kDurationSeconds = 20;

int failures = 0;

#define EXPECT(condition, format, ...)                                         \
  do {                                                                         \
    if (!(condition)) {                                                        \
      ++failures;                                                              \
      std::fprintf(stderr, "FAIL [%s:%d] " format "\n", __FILE__, __LINE__,   \
                   ##__VA_ARGS__);                                             \
    }                                                                          \
  } while (0)

class PulseInstance final : public SoLoud::AudioSourceInstance {
public:
  unsigned int getAudio(float *buffer, unsigned int samplesToRead,
                        unsigned int bufferSize) override;
  bool hasEnded() override { return false; }

private:
  unsigned long long sourceFrame_ = 0;
};

class PulseSource final : public SoLoud::AudioSource {
public:
  PulseSource() {
    mBaseSamplerate = static_cast<float>(kSampleRate);
    mChannels = 1;
  }

  SoLoud::AudioSourceInstance *createInstance() override {
    return new PulseInstance();
  }
};

unsigned int PulseInstance::getAudio(float *buffer,
                                     unsigned int samplesToRead,
                                     unsigned int /*bufferSize*/) {
  for (unsigned int i = 0; i < samplesToRead; ++i, ++sourceFrame_) {
    buffer[i] = sourceFrame_ % kPulseIntervalFrames == 0 ? 1.0f : 0.0f;
  }
  return samplesToRead;
}

std::vector<unsigned long long> renderPulseFrames(SoLoud::Soloud &engine) {
  std::vector<float> mixed(kChunkFrames * 2);
  std::vector<unsigned long long> peaks;
  unsigned long long outputFrame = 0;

  const unsigned int chunkCount =
      kDurationSeconds * kSampleRate / kChunkFrames;
  for (unsigned int chunk = 0; chunk < chunkCount; ++chunk) {
    engine.mix(mixed.data(), kChunkFrames);
    for (unsigned int frame = 0; frame < kChunkFrames;
         ++frame, ++outputFrame) {
      const float value = std::fabs(mixed[frame * 2]);
      if (value <= 0.005f) {
        continue;
      }
      if (peaks.empty() || outputFrame - peaks.back() > 1000) {
        peaks.push_back(outputFrame);
      }
    }
  }
  return peaks;
}

void testBusUsesEngineSampleRateWithoutDrift() {
  SoLoud::Soloud engine;
  const SoLoud::result initResult = engine.init(
      SoLoud::Soloud::CLIP_ROUNDOFF, SoLoud::Soloud::NULLDRIVER, kSampleRate,
      512, 2);
  EXPECT(initResult == SoLoud::SO_NO_ERROR, "engine init failed: %d",
         initResult);
  if (initResult != SoLoud::SO_NO_ERROR) {
    return;
  }

  BusData busData(1, &engine);
  EXPECT(busData.bus.mBaseSamplerate == static_cast<float>(kSampleRate),
         "bus sample rate %.1f does not match engine sample rate %u",
         busData.bus.mBaseSamplerate, kSampleRate);

  PulseSource source;
  engine.play(busData.bus);
  busData.bus.play(source);
  const std::vector<unsigned long long> peaks = renderPulseFrames(engine);

  EXPECT(peaks.size() >= 190, "expected at least 190 pulses, found %zu",
         peaks.size());
  if (peaks.size() >= 3) {
    const double outputFramesPerPulse =
        static_cast<double>(peaks.back() - peaks[1]) /
        static_cast<double>(peaks.size() - 2);
    const double contentRate =
        static_cast<double>(kPulseIntervalFrames) / outputFramesPerPulse;
    const double errorPpm = std::fabs(contentRate - 1.0) * 1000000.0;
    EXPECT(errorPpm < 100.0,
           "bus content rate %.9f differs by %.1f ppm (%.3f ms over %u s)",
           contentRate, errorPpm,
           (contentRate - 1.0) * kDurationSeconds * 1000.0,
           kDurationSeconds);
  }

  engine.deinit();
}

void testBusCreatedBeforeInitSynchronizesBeforePlayback() {
  SoLoud::Soloud engine;
  BusData busData(2, &engine);

  const SoLoud::result initResult = engine.init(
      SoLoud::Soloud::CLIP_ROUNDOFF, SoLoud::Soloud::NULLDRIVER, kSampleRate,
      512, 2);
  EXPECT(initResult == SoLoud::SO_NO_ERROR, "engine init failed: %d",
         initResult);
  if (initResult != SoLoud::SO_NO_ERROR) {
    return;
  }

  busData.syncSampleRate(&engine);
  EXPECT(busData.bus.mBaseSamplerate == static_cast<float>(kSampleRate),
         "pre-init bus sample rate %.1f was not synchronized to %u",
         busData.bus.mBaseSamplerate, kSampleRate);
  engine.deinit();
}

} // namespace

int main() {
  std::printf("Test: mixing bus follows the engine sample rate\n");
  testBusUsesEngineSampleRateWithoutDrift();
  testBusCreatedBeforeInitSynchronizesBeforePlayback();
  if (failures != 0) {
    std::fprintf(stderr, "%d assertion(s) failed\n", failures);
    return EXIT_FAILURE;
  }
  std::printf("PASS\n");
  return EXIT_SUCCESS;
}
