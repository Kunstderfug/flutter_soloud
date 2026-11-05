// Adapted from GPhil Native Classical Leveler DSP.
#include "classical_leveler_processor.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace classical_leveler {
namespace {

constexpr double decibelsToNatural = 0.11512925464970228420;  // ln(10)/20
// Half-width of the soft knee centred on the threshold (6 dB knee).
constexpr double kneeHalfWidthDb = 3.0;

}  // namespace

LiftMapping liftMapping(const double liftPercent) noexcept {
  return {0.1 * liftPercent, 1.5 + 0.015 * liftPercent};
}

// ---------------------------------------------------------------------------
// Processor.

Processor::Processor(const double sampleRate, const std::uint32_t channelCount) noexcept
    : sampleRate_(sampleRate),
      channelCount_(std::min<std::uint32_t>(channelCount, maximumChannels)),
      binFrames_(static_cast<std::uint32_t>(
          std::max(1.0, std::round(sampleRate * binSeconds)))),
      holdBins_(static_cast<std::uint32_t>(std::round(holdSeconds / binSeconds))),
      weighting_(k_weighting::design(sampleRate)) {
  adopt(Settings{});
  reset();
}

void Processor::reset() noexcept {
  clearDetector();
  phase_ = Phase::follow;
  holdRemaining_ = 0;
  targetDb_ = 0.0;
  gainDb_ = 0.0;
  idle_ = !enabled_;
  clock_ = 0;
  blockFrame_ = 0;
  pending_ = LookAheadPlan{};
  pendingAdopted_ = true;
  planActive_ = false;
  planBins_ = 0;
  ceiling_ = 0.0;
}

void Processor::clearDetector() noexcept {
  filterState_ = {};
  bins_ = {};
  binCursor_ = 0;
  filledBins_ = 0;
  binSum_ = 0.0;
  binFrame_ = 0;
  phase_ = Phase::follow;
  holdRemaining_ = 0;
  targetDb_ = 0.0;
}

void Processor::adopt(const Settings& settings) noexcept {
  const auto mapping = liftMapping(settings.liftPercent);
  enabled_ = settings.enabled;
  maximumLiftDb_ = mapping.maximumLiftDb;
  liftFactor_ = 1.0 - 1.0 / mapping.ratio;
  thresholdLufs_ = settings.thresholdLufs;
  floorLufs_ = settings.floorLufs;
  riseStepDb_ = 10.0 / settings.riseTimeS / sampleRate_;
  fallStepDb_ = 10.0 / (settings.fallTimeMs / 1000.0) / sampleRate_;
  downwardEnabled_ = settings.downwardEnabled;
  downwardThresholdLufs_ = settings.downwardThresholdLufs;
  downwardFactor_ = 1.0 - 1.0 / settings.downwardRatio;
  if (enabled_) idle_ = false;
  // Fall rate and targets may have changed: re-derive the ceiling.
  if (planActive_) ceiling_ = ceilingAt(clock_ + binFrames_);
}

void Processor::beginBlock(const LookAheadPlan* plan) noexcept {
  blockFrame_ = 0;
  if (plan != nullptr) {
    pending_ = *plan;
  } else {
    pending_.valid = false;
    pending_.snap = false;
    pending_.adoptFrame = 0;
  }
  pendingAdopted_ = false;
}

void Processor::adoptPendingPlan() noexcept {
  pendingAdopted_ = true;
  if (!pending_.valid) {
    planActive_ = false;
    return;
  }
  const auto blockStart = clock_ - blockFrame_;
  planNowLufs_ = pending_.nowLufs;
  planBins_ = std::min<std::uint32_t>(pending_.binCount,
                                      static_cast<std::uint32_t>(LookAheadPlan::maximumBins));
  for (std::uint32_t bin = 0; bin < planBins_; ++bin) {
    planLufs_[bin] = pending_.lufs[bin];
    const auto lead = std::max(0.0, pending_.leadFrames[bin]);
    planDeadline_[bin] = blockStart + static_cast<std::uint64_t>(lead);
  }
  planActive_ = true;
  if (pending_.snap && enabled_ && !idle_) {
    // The destination's planned gain from its first frame: the detector's
    // window holds the departed program, so it restarts too.
    clearDetector();
    evaluate(planNowLufs_);
    ceiling_ = ceilingAt(clock_ + binFrames_);
    gainDb_ = std::min(phase_ == Phase::follow ? targetDb_ : gainDb_, ceiling_);
    if (phase_ != Phase::follow) targetDb_ = gainDb_;
    return;
  }
  ceiling_ = ceilingAt(clock_ + binFrames_);
}

double Processor::ceilingAt(const std::uint64_t clock) const noexcept {
  // The highest gain from which the fall rate still reaches every upcoming
  // audible bin's target by the time that bin arrives.
  auto ceiling = std::numeric_limits<double>::infinity();
  for (std::uint32_t bin = 0; bin < planBins_; ++bin) {
    if (!(planLufs_[bin] >= floorLufs_)) continue;
    const auto remaining =
        planDeadline_[bin] > clock ? static_cast<double>(planDeadline_[bin] - clock) : 0.0;
    ceiling = std::min(ceiling, followTarget(planLufs_[bin]) + fallStepDb_ * remaining);
  }
  return ceiling;
}

bool Processor::detect(const std::array<float, maximumChannels>& frame) noexcept {
  double power = 0.0;
  for (std::uint32_t channel = 0; channel < channelCount_; ++channel) {
    const auto weighted = k_weighting::filter(weighting_, filterState_[channel],
                                              static_cast<double>(frame[channel]));
    power += weighted * weighted;
  }
  binSum_ += power;
  if (++binFrame_ < binFrames_) return false;
  bins_[binCursor_] = binSum_;
  binCursor_ = (binCursor_ + 1) % windowBins;
  filledBins_ = std::min(filledBins_ + 1, windowBins);
  binSum_ = 0.0;
  binFrame_ = 0;
  return true;
}

double Processor::detectorLoudness() const noexcept {
  double sum = 0.0;
  for (const auto bin : bins_) sum += bin;
  return k_weighting::loudness(
      sum / (static_cast<double>(filledBins_) * static_cast<double>(binFrames_)));
}

double Processor::followTarget(const double loudness) const noexcept {
  const auto distance = thresholdLufs_ - loudness;
  double knee = 0.0;
  if (distance >= kneeHalfWidthDb) {
    knee = distance;
  } else if (distance > -kneeHalfWidthDb) {
    const auto x = distance + kneeHalfWidthDb;
    knee = x * x / (4.0 * kneeHalfWidthDb);
  }
  if (downwardEnabled_ && loudness > downwardThresholdLufs_)
    return -(loudness - downwardThresholdLufs_) * downwardFactor_;
  return std::min(maximumLiftDb_, knee * liftFactor_);
}

void Processor::evaluate(const double loudness) noexcept {
  if (!(loudness >= floorLufs_)) {
    // Silence, a general pause or noise: hold the applied gain, then relax.
    if (phase_ == Phase::follow) {
      phase_ = Phase::hold;
      holdRemaining_ = holdBins_;
      targetDb_ = gainDb_;
    } else if (phase_ == Phase::hold && --holdRemaining_ == 0) {
      phase_ = Phase::relax;
      targetDb_ = 0.0;
    }
    return;
  }
  phase_ = Phase::follow;
  targetDb_ = followTarget(loudness);
}

double Processor::process(std::array<float, maximumChannels>& frame) noexcept {
  if (!pendingAdopted_ && blockFrame_ >= pending_.adoptFrame) adoptPendingPlan();
  ++blockFrame_;
  ++clock_;
  if (idle_) return 0.0;
  if (detect(frame)) {
    evaluate(planActive_ ? planNowLufs_ : detectorLoudness());
    if (planActive_) ceiling_ = ceilingAt(clock_ + binFrames_);
  }
  // A hold never returns toward a gain the stage already left: while
  // disabled (or otherwise) the gain moves toward 0 dB, the held target
  // follows it, so a re-enable below the floor keeps the current gain.
  if (phase_ == Phase::hold &&
      (targetDb_ > 0.0 ? gainDb_ < targetDb_ : gainDb_ > targetDb_) &&
      (targetDb_ > 0.0) == (gainDb_ >= 0.0))
    targetDb_ = gainDb_;
  // An upcoming louder bin caps the target; reaching the cap in time takes
  // the fall rate even while a hold relaxes.
  const bool capped = enabled_ && planActive_ && ceiling_ < targetDb_;
  const auto target = !enabled_ ? 0.0 : capped ? ceiling_ : targetDb_;
  // A relaxing hold returns at the slow rise rate in either direction.
  const bool relaxing = enabled_ && phase_ == Phase::relax && !capped;
  const auto up = riseStepDb_;
  const auto down = relaxing ? riseStepDb_ : fallStepDb_;
  // Lift rising (gain moving up) is slow; lift falling is faster.
  gainDb_ += std::clamp(target - gainDb_, -down, up);
  if (!enabled_ && gainDb_ == 0.0) {
    idle_ = true;
    clearDetector();
    return 0.0;
  }
  if (gainDb_ != 0.0) {
    const auto linear = std::exp(gainDb_ * decibelsToNatural);
    for (std::uint32_t channel = 0; channel < channelCount_; ++channel) {
      frame[channel] = static_cast<float>(static_cast<double>(frame[channel]) * linear);
    }
  }
  return gainDb_;
}

}  // namespace classical_leveler
