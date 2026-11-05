#include "classical_leveler_filter.h"
#include <algorithm>
#include <cmath>

namespace {
constexpr float defaults[] = {0, 50, -30, -60, 4, 500, 0, -10, 1.3f};
constexpr float minimums[] = {0, 0, -45, -80, 1, 100, 0, -12, 1.1f};
constexpr float maximums[] = {1, 100, -15, -50, 20, 2000, 1, 0, 2};
constexpr const char *names[] = {
    "Wet", "Lift Soft Passages", "Threshold LUFS", "Floor LUFS",
    "Rise Seconds per 10 dB", "Fall Milliseconds per 10 dB",
    "Gentle Downward", "Downward Threshold LUFS", "Downward Ratio",
    "Applied Gain dB", "Look-ahead Active"};
}

int ClassicalLeveler::getParamCount() { return PARAMETER_COUNT; }
const char *ClassicalLeveler::getParamName(unsigned int index) {
  return index <= LOOK_AHEAD_ACTIVE ? names[index] : "Look-ahead Plan";
}
unsigned int ClassicalLeveler::getParamType(unsigned int) { return FLOAT_PARAM; }
float ClassicalLeveler::getParamMin(unsigned int index) {
  return index < APPLIED_GAIN_DB ? minimums[index] : -200;
}
float ClassicalLeveler::getParamMax(unsigned int index) {
  return index < APPLIED_GAIN_DB ? maximums[index] : 100;
}
SoLoud::FilterInstance *ClassicalLeveler::createInstance() {
  return new ClassicalLevelerInstance();
}
ClassicalLevelerInstance::ClassicalLevelerInstance() {
  initParams(ClassicalLeveler::PARAMETER_COUNT);
  std::copy(std::begin(defaults), std::end(defaults), mParam);
}
void ClassicalLevelerInstance::updateParams(SoLoud::time time) {
  // SoLoud's change mask is 32 bits. Transport slots are deliberately not
  // faders and never shift that mask by their larger parameter indices.
  for (unsigned int i = 0; i < ClassicalLeveler::APPLIED_GAIN_DB; ++i) {
    if (mParamFader[i].mActive > 0) {
      const float value = mParamFader[i].get(time);
      if (std::isfinite(value)) {
        mParam[i] = std::clamp(value, minimums[i], maximums[i]);
      }
    }
  }
}
void ClassicalLevelerInstance::fadeFilterParameter(unsigned int index, float to,
    SoLoud::time duration, SoLoud::time start) {
  if (index < ClassicalLeveler::APPLIED_GAIN_DB) {
    SoLoud::FilterInstance::fadeFilterParameter(index, to, duration, start);
  }
}
void ClassicalLevelerInstance::oscillateFilterParameter(unsigned int index,
    float from, float to, SoLoud::time duration, SoLoud::time start) {
  if (index < ClassicalLeveler::APPLIED_GAIN_DB) {
    SoLoud::FilterInstance::oscillateFilterParameter(index, from, to, duration, start);
  }
}
void ClassicalLevelerInstance::setFilterParameter(unsigned int index, float value) {
  using P = ClassicalLeveler;
  if (index >= P::PARAMETER_COUNT || !std::isfinite(value)) { return; }
  if (index < P::APPLIED_GAIN_DB) {
    if (value < minimums[index] || value > maximums[index]) { return; }
    if (index == P::DOWNWARD_ENABLED && value != 0 && value != 1) { return; }
    mParamFader[index].mActive = 0;
    mParam[index] = value;
    return;
  }
  if (index <= P::LOOK_AHEAD_ACTIVE) { return; } // read-only telemetry
  if (index != P::PLAN_COMMIT) {
    if (index == P::PLAN_COUNT && (value < 0 || value > P::maximumBins || std::floor(value) != value)) { return; }
    if (index >= P::PLAN_BINS + P::maximumBins && value < 0) { return; }
    mParam[index] = value;
    return;
  }
  // Commit is one parameter call under the engine audio mutex. A render
  // between any staging writes continues using the previous complete plan.
  plan_ = {};
  plan_.binCount = static_cast<unsigned int>(mParam[P::PLAN_COUNT]);
  plan_.valid = plan_.binCount > 0;
  plan_.nowLufs = mParam[P::PLAN_NOW];
  plan_.snap = mParam[P::PLAN_SNAP] == 1;
  for (unsigned int i = 0; i < plan_.binCount; ++i) {
    plan_.lufs[i] = mParam[P::PLAN_BINS + i];
    // Keep seconds until the callback knows the active sample rate.
    plan_.leadFrames[i] = mParam[P::PLAN_BINS + P::maximumBins + i];
  }
  planAgeSeconds_ = 0;
}
void ClassicalLevelerInstance::filter(float *buffer, unsigned int frames,
    unsigned int stride, unsigned int channels, float sampleRate, SoLoud::time time) {
  using P = ClassicalLeveler;
  if (!(sampleRate > 0) || channels == 0 || channels > 2) {
    mParam[P::APPLIED_GAIN_DB] = 0;
    mParam[P::LOOK_AHEAD_ACTIVE] = 0;
    return;
  }
  if (!processor_ || sampleRate != sampleRate_ || channels != channels_) {
    processor_.emplace(sampleRate, channels);
    sampleRate_ = sampleRate;
    channels_ = channels;
  }
  updateParams(time);
  classical_leveler::Settings settings;
  settings.enabled = mParam[P::WET] > 0;
  settings.liftPercent = mParam[P::LIFT_PERCENT];
  settings.thresholdLufs = mParam[P::THRESHOLD_LUFS];
  settings.floorLufs = mParam[P::FLOOR_LUFS];
  settings.riseTimeS = mParam[P::RISE_TIME_S];
  settings.fallTimeMs = mParam[P::FALL_TIME_MS];
  settings.downwardEnabled = mParam[P::DOWNWARD_ENABLED] == 1;
  settings.downwardThresholdLufs = mParam[P::DOWNWARD_THRESHOLD_LUFS];
  settings.downwardRatio = mParam[P::DOWNWARD_RATIO];
  processor_->adopt(settings);
  auto blockPlan = plan_;
  blockPlan.valid = plan_.valid && planAgeSeconds_ + 0.5 / sampleRate < 0.1;
  for (unsigned int i = 0; i < blockPlan.binCount; ++i) {
    blockPlan.leadFrames[i] = std::max(0.0, plan_.leadFrames[i] - planAgeSeconds_) * sampleRate;
  }
  processor_->beginBlock(&blockPlan);
  plan_.snap = false;
  double gainDb = 0;
  for (unsigned int frame = 0; frame < frames; ++frame) {
    std::array<float, 2> samples{};
    for (unsigned int ch = 0; ch < channels; ++ch) { samples[ch] = buffer[ch * stride + frame]; }
    auto processed = samples;
    gainDb = processor_->process(processed);
    // During disable the processor performs its own smooth return to unity.
    const double wet = settings.enabled ? mParam[P::WET] : 1.0;
    for (unsigned int ch = 0; ch < channels; ++ch) {
      buffer[ch * stride + frame] = static_cast<float>(samples[ch] + wet * (processed[ch] - samples[ch]));
    }
  }
  planAgeSeconds_ += static_cast<double>(frames) / sampleRate;
  const double wet = settings.enabled ? mParam[P::WET] : 1.0;
  mParam[P::APPLIED_GAIN_DB] = static_cast<float>(20 * std::log10(1 + wet * (std::pow(10.0, gainDb / 20) - 1)));
  mParam[P::LOOK_AHEAD_ACTIVE] = processor_->lookAheadActive() ? 1 : 0;
}
