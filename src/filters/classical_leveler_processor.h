// Adapted from GPhil Native Classical Leveler DSP.
#pragma once

#include "classical_k_weighting.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

// Classical Leveler (ticket 404): settings model, ranges, defaults, the Lift
// Soft Passages mapping, and the real-time processor the Venue Output Chain
// runs as stage 1 (after the EQ, before the limiter detector).
namespace classical_leveler {

struct Settings {
  bool enabled = false;
  double liftPercent = 50.0;
  double thresholdLufs = -30.0;
  double floorLufs = -60.0;
  double riseTimeS = 4.0;     // seconds per 10 dB of rising lift
  double fallTimeMs = 500.0;  // milliseconds per 10 dB of falling lift
  bool downwardEnabled = false;
  double downwardThresholdLufs = -10.0;
  double downwardRatio = 1.3;
};

inline bool operator==(const Settings& lhs, const Settings& rhs) noexcept {
  return lhs.enabled == rhs.enabled && lhs.liftPercent == rhs.liftPercent &&
         lhs.thresholdLufs == rhs.thresholdLufs &&
         lhs.floorLufs == rhs.floorLufs && lhs.riseTimeS == rhs.riseTimeS &&
         lhs.fallTimeMs == rhs.fallTimeMs &&
         lhs.downwardEnabled == rhs.downwardEnabled &&
         lhs.downwardThresholdLufs == rhs.downwardThresholdLufs &&
         lhs.downwardRatio == rhs.downwardRatio;
}
inline bool operator!=(const Settings& lhs, const Settings& rhs) noexcept {
  return !(lhs == rhs);
}

struct LiftMapping { double maximumLiftDb; double ratio; };
LiftMapping liftMapping(double liftPercent) noexcept;

// A block's look-ahead plan (ticket 405), built by the renderer from the
// prepared loudness envelopes of the audible sources, power-summed at their
// mix gains. Loudness is BS.1770 LUFS of the planned program.
struct LookAheadPlan {
  // 2 s of 100 ms source bins, the bin in progress and slack.
  static constexpr std::size_t maximumBins = 32;
  // False: some audible source has no envelope, so the real-time detector
  // drives the leveler.
  bool valid = false;
  // Forward 400 ms loudness from the block's source position.
  double nowLufs = 0.0;
  std::uint32_t binCount = 0;
  // Per upcoming source bin: its loudness and the block frames until it
  // reaches the leveler (0 for a bin already sounding).
  std::array<double, maximumBins> lufs{};
  std::array<double, maximumBins> leadFrames{};
  // The plan applies from this block frame (a handoff's first incoming
  // frame); earlier frames keep the previous plan.
  std::uint32_t adoptFrame = 0;
  // A seek, preparation or handoff: the gain jumps to the destination's
  // planned gain at adoptFrame instead of ramping.
  bool snap = false;
};

// Real-time processor for one prepared rate. Bounded, allocation-free state
// sized at compile time; every call is audio-thread safe.
//
// Detector: per channel the BS.1770 K-weighting pre-filter (high shelf, then
// RLB high-pass) feeds squared samples into 10 ms bins; the short-term
// loudness L = -0.691 + 10 log10(sum over channels of the mean square over
// the latest 40 bins) is re-evaluated at every bin boundary, a frame-count
// schedule independent of the render block. During warm-up the mean covers
// the filled bins only.
//
// Look-ahead (ticket 405): with a valid plan, the forward planned loudness
// replaces the detector's in the same phase schedule, and the applied gain
// never exceeds a ceiling that lets the fall rate reach every upcoming bin's
// target before that bin arrives, so the lift is already falling ahead of a
// crescendo. The detector keeps running, so a plan that becomes invalid
// falls back to it without interruption.
class Processor final {
 public:
  static constexpr std::size_t maximumChannels = 2;
  static constexpr std::size_t windowBins = 40;  // 400 ms
  static constexpr double binSeconds = 0.01;
  static constexpr double holdSeconds = 2.0;

  Processor(double sampleRate, std::uint32_t channelCount) noexcept;

  // Adopts settings at a block boundary. A newer target supersedes from the
  // current applied gain, never a reset.
  void adopt(const Settings& settings) noexcept;

  // True while the stage is disabled at exactly 0 dB: it passes samples
  // untouched and its detector rests empty.
  [[nodiscard]] bool idle() const noexcept { return idle_; }

  // Starts a block with an optional look-ahead plan (null: none). The plan
  // is copied; it takes effect at its adoptFrame.
  void beginBlock(const LookAheadPlan* plan) noexcept;

  // Processes one frame in place and returns the applied gain in dB.
  double process(std::array<float, maximumChannels>& frame) noexcept;

  // True while a valid look-ahead plan drives the gain: the stage is
  // enabled and every audible source has an envelope. A disabled stage
  // reports no look-ahead even though it keeps adopting plans.
  [[nodiscard]] bool lookAheadActive() const noexcept { return enabled_ && planActive_; }

  void reset() noexcept;

 private:
  // Returns true at a bin boundary.
  bool detect(const std::array<float, maximumChannels>& frame) noexcept;
  [[nodiscard]] double detectorLoudness() const noexcept;
  [[nodiscard]] double followTarget(double loudness) const noexcept;
  void evaluate(double loudness) noexcept;
  void clearDetector() noexcept;
  void adoptPendingPlan() noexcept;
  [[nodiscard]] double ceilingAt(std::uint64_t clock) const noexcept;

  double sampleRate_;
  std::uint32_t channelCount_;
  std::uint32_t binFrames_;
  std::uint32_t holdBins_;
  k_weighting::Design weighting_{};
  std::array<k_weighting::ChannelState, maximumChannels> filterState_{};
  std::array<double, windowBins> bins_{};
  std::size_t binCursor_ = 0;
  std::size_t filledBins_ = 0;
  double binSum_ = 0.0;
  std::uint32_t binFrame_ = 0;

  // Adopted control values.
  bool enabled_ = false;
  double maximumLiftDb_ = 0.0;
  double liftFactor_ = 0.0;  // 1 - 1/ratio
  double thresholdLufs_ = -30.0;
  double floorLufs_ = -60.0;
  double riseStepDb_ = 0.0;  // per sample
  double fallStepDb_ = 0.0;
  bool downwardEnabled_ = false;
  double downwardThresholdLufs_ = -10.0;
  double downwardFactor_ = 0.0;

  enum class Phase { follow, hold, relax };
  Phase phase_ = Phase::follow;
  std::uint32_t holdRemaining_ = 0;
  double targetDb_ = 0.0;
  double gainDb_ = 0.0;
  bool idle_ = true;

  // Look-ahead. clock_ counts processed frames; deadlines are clock values.
  std::uint64_t clock_ = 0;
  std::uint32_t blockFrame_ = 0;
  LookAheadPlan pending_{};
  bool pendingAdopted_ = true;
  bool planActive_ = false;
  double planNowLufs_ = 0.0;
  std::uint32_t planBins_ = 0;
  std::array<double, LookAheadPlan::maximumBins> planLufs_{};
  std::array<std::uint64_t, LookAheadPlan::maximumBins> planDeadline_{};
  double ceiling_ = 0.0;
};

}  // namespace classical_leveler
