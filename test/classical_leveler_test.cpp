#include "../src/filters/classical_leveler_filter.h"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <vector>

using P = ClassicalLeveler;
constexpr double rate = 48000;
struct Rig {
  ClassicalLevelerInstance filter;
  unsigned long frame = 0;
  void render(unsigned int count, double amplitude = 0.01) {
    while (count) {
      const unsigned int n = std::min(count, 480U);
      std::vector<float> audio(n * 2);
      for (unsigned int i = 0; i < n; ++i) {
        audio[i] = audio[n + i] = amplitude * std::sin(2 * 3.141592653589793 * 1000 * (frame + i) / rate);
      }
      filter.filter(audio.data(), n, n, 2, rate, frame / rate);
      for (unsigned int i = 0; i < n; ++i) {
        assert(std::isfinite(audio[i]));
        assert(audio[i] == audio[n + i]); // stereo linkage
      }
      frame += n;
      count -= n;
    }
  }
  double gain() { return filter.getFilterParameter(P::APPLIED_GAIN_DB); }
  void plan(double now, double upcoming, double lead, bool snap) {
    filter.setFilterParameter(P::PLAN_NOW, now);
    filter.setFilterParameter(P::PLAN_COUNT, 1);
    filter.setFilterParameter(P::PLAN_SNAP, snap ? 1 : 0);
    filter.setFilterParameter(P::PLAN_BINS, upcoming);
    filter.setFilterParameter(P::PLAN_BINS + 21, lead);
    filter.setFilterParameter(P::PLAN_COMMIT, 1);
  }
};
int main() {
  ClassicalLeveler parent;
  assert(parent.getParamCount() == 57);
  {
    Rig rig;
    rig.render(48000);
    assert(rig.gain() == 0); // bypass default
    rig.filter.setFilterParameter(P::WET, 1);
    rig.render(4 * 48000);
    assert(std::abs(rig.gain() - 5) < 0.01); // soft reaches mapped cap
    rig.filter.setFilterParameter(P::WET, 0);
    const double before = rig.gain();
    rig.render(480);
    assert(rig.gain() < before && rig.gain() > 0); // smooth bypass
    rig.render(48000);
    assert(rig.gain() == 0);
  }
  {
    Rig rig;
    rig.filter.setFilterParameter(P::WET, 1);
    rig.plan(-40, -40, 0, true);
    rig.render(480);
    assert(std::abs(rig.gain() - 5) < 0.01); // first frame destination gain
    assert(rig.filter.getFilterParameter(P::LOOK_AHEAD_ACTIVE) == 1);
    // Staging half a new plan does not affect the committed soft plan.
    rig.filter.setFilterParameter(P::PLAN_NOW, -5);
    rig.render(480);
    assert(std::abs(rig.gain() - 5) < 0.01);
    rig.plan(-40, -18, 0.1, false);
    rig.render(480);
    assert(rig.gain() < 5); // falls ahead of crescendo at +100ms
    rig.render(4800);
    assert(rig.filter.getFilterParameter(P::LOOK_AHEAD_ACTIVE) == 0); // expiry
  }
  {
    Rig rig;
    rig.filter.setFilterParameter(P::WET, 1);
    rig.filter.setFilterParameter(P::DOWNWARD_ENABLED, 1);
    rig.plan(-5, -5, 0, true);
    rig.render(480);
    assert(std::abs(rig.gain() - (-5 * (1 - 1 / 1.3))) < 0.001);
    rig.plan(-40, -40, 0, true);
    rig.render(480);
    assert(std::abs(rig.gain() - 5) < 0.01); // seek snaps from negative to soft
    rig.filter.setFilterParameter(P::PLAN_COUNT, 0);
    rig.filter.setFilterParameter(P::PLAN_COMMIT, 1);
    rig.render(480);
    assert(rig.filter.getFilterParameter(P::LOOK_AHEAD_ACTIVE) == 0);
  }
  {
    Rig rig;
    rig.filter.setFilterParameter(P::WET, 1);
    rig.render(4 * 48000, 0);
    assert(rig.gain() == 0); // digital silence never lifted
    rig.filter.setFilterParameter(P::LIFT_PERCENT, NAN);
    assert(rig.filter.getFilterParameter(P::LIFT_PERCENT) == 50);
  }
  std::puts("Classical Leveler DSP tests passed");
}
