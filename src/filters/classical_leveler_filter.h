#pragma once

#include "../soloud/include/soloud.h"
#include "classical_leveler_processor.h"
#include <optional>

// Source-aware, latency-free look-ahead. The app stages bounded source
// loudness bins with output arrival times, then commits them together. The
// audio thread only consumes the committed copy. Existing SoLoud's audio
// mutex serializes parameter writes with rendering.
class ClassicalLeveler : public SoLoud::Filter {
 public:
  enum Parameter {
    WET, LIFT_PERCENT, THRESHOLD_LUFS, FLOOR_LUFS, RISE_TIME_S,
    FALL_TIME_MS, DOWNWARD_ENABLED, DOWNWARD_THRESHOLD_LUFS, DOWNWARD_RATIO,
    APPLIED_GAIN_DB, LOOK_AHEAD_ACTIVE,
    PLAN_NOW, PLAN_COUNT, PLAN_SNAP, PLAN_BINS, // 21 LUFS + 21 lead seconds
    PLAN_COMMIT = PLAN_BINS + 42,
    PARAMETER_COUNT
  };
  static constexpr unsigned int maximumBins = 21;
  int getParamCount() override;
  const char *getParamName(unsigned int index) override;
  unsigned int getParamType(unsigned int index) override;
  float getParamMin(unsigned int index) override;
  float getParamMax(unsigned int index) override;
  SoLoud::FilterInstance *createInstance() override;
};

class ClassicalLevelerInstance : public SoLoud::FilterInstance {
 public:
  ClassicalLevelerInstance();
  void filter(float *buffer, unsigned int frames, unsigned int stride,
              unsigned int channels, float sampleRate, SoLoud::time time) override;
  void setFilterParameter(unsigned int index, float value) override;
  void updateParams(SoLoud::time time) override;
  void fadeFilterParameter(unsigned int index, float to, SoLoud::time duration,
                           SoLoud::time start) override;
  void oscillateFilterParameter(unsigned int index, float from, float to,
                                 SoLoud::time duration, SoLoud::time start) override;
 private:
  std::optional<classical_leveler::Processor> processor_;
  classical_leveler::LookAheadPlan plan_{};
  double sampleRate_ = 0;
  unsigned int channels_ = 0;
  // Advance the plan by rendered frames; stop trusting a stalled UI producer.
  double planAgeSeconds_ = 1;
};
