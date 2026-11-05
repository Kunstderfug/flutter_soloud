// Adapted from GPhil Native BS.1770 K-weighting.
#pragma once

#include <array>
#include <cmath>
#include <cstddef>

namespace k_weighting {

// ITU-R BS.1770 K-weighting pre-filter designed for one rate with the
// standard analog-matched formulas (as published with libebur128): a high
// shelf followed by the RLB high-pass. Shared by the Classical Leveler's
// real-time detector (ticket 404) and prepared loudness envelopes (ticket
// 405) so both measure the same loudness.
struct Biquad {
  double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
};

struct Design {
  Biquad shelf;
  Biquad highPass;
};

inline Design design(const double sampleRate) noexcept {
  constexpr double pi = 3.14159265358979323846;
  Design out;
  {
    const double f0 = 1681.974450955533;
    const double gainDb = 3.999843853973347;
    const double q = 0.7071752369554196;
    const double k = std::tan(pi * f0 / sampleRate);
    const double vh = std::pow(10.0, gainDb / 20.0);
    const double vb = std::pow(vh, 0.4996667741545416);
    const double a0 = 1.0 + k / q + k * k;
    out.shelf = Biquad{(vh + vb * k / q + k * k) / a0, 2.0 * (k * k - vh) / a0,
                       (vh - vb * k / q + k * k) / a0, 2.0 * (k * k - 1.0) / a0,
                       (1.0 - k / q + k * k) / a0};
  }
  {
    const double f0 = 38.13547087602444;
    const double q = 0.5003270373238773;
    const double k = std::tan(pi * f0 / sampleRate);
    const double a0 = 1.0 + k / q + k * k;
    out.highPass = Biquad{1.0, -2.0, 1.0, 2.0 * (k * k - 1.0) / a0,
                          (1.0 - k / q + k * k) / a0};
  }
  return out;
}

// Transposed direct form II state for one channel: two values per biquad.
using ChannelState = std::array<double, 4>;

// Filters one sample and returns its K-weighted value.
inline double filter(const Design& d, ChannelState& state, const double x) noexcept {
  const auto shelved = d.shelf.b0 * x + state[0];
  state[0] = d.shelf.b1 * x - d.shelf.a1 * shelved + state[1];
  state[1] = d.shelf.b2 * x - d.shelf.a2 * shelved;
  const auto weighted = d.highPass.b0 * shelved + state[2];
  state[2] = d.highPass.b1 * shelved - d.highPass.a1 * weighted + state[3];
  state[3] = d.highPass.b2 * shelved - d.highPass.a2 * weighted;
  return weighted;
}

// BS.1770 loudness of a channel-summed mean square.
inline double loudness(const double meanSquare) noexcept {
  return meanSquare > 0.0 ? -0.691 + 10.0 * std::log10(meanSquare)
                          : -HUGE_VAL;
}

}  // namespace k_weighting
