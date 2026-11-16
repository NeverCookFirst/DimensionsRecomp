#include "gpu_native/fps_counter.h"
#include <cassert>
#include <limits>
#include <cstdio>
#include <cstdlib>

#undef assert
#define assert(condition) do { if (!(condition)) { \
  std::fprintf(stderr, "check failed: %s\n", #condition); std::exit(23); } } while (0)

using legodimensions::gpu_native::NativeFpsCounter;

int main() {
  NativeFpsCounter counter;
  double now = 100;
  counter.RecordPresent(now);
  assert(counter.FrameTimeMilliseconds()==0);
  for (unsigned i = 0; i < 300; ++i) counter.RecordPresent(now += 1.0 / 30);
  assert(counter.Value() == 30);
  // A real stall must lower the reading, never be clamped to the target FPS.
  counter.RecordPresent(now += 0.6);
  assert(std::abs(counter.FrameTimeMilliseconds()-600)<0.000001);
  assert(counter.Value() < 20);
  for (unsigned i = 0; i < 120; ++i) counter.RecordPresent(now += 1.0 / 60);
  assert(counter.Value() == 60);
  // Unequal durations: two frames / 100 ms is 20 FPS, rather than averaging
  // instantaneous 100 and 11.1 FPS to a misleading 55.6 FPS.
  counter = {};
  now = 0;
  counter.RecordPresent(now);
  for (unsigned i = 0; i < 100; ++i) {
    counter.RecordPresent(now += 0.01);
    assert(std::abs(counter.FrameTimeMilliseconds()-10)<0.000001);
    counter.RecordPresent(now += 0.09);
    assert(std::abs(counter.FrameTimeMilliseconds()-90)<0.000001);
  }
  // A complete boundary frame may extend the window by up to90ms.
  assert(counter.Value() >= 19 && counter.Value() <= 21);
  const auto displayed = counter.Value();
  counter.RecordPresent(now += 0.01);
  assert(counter.Value() == displayed);  // Digits do not change every frame.
  counter.RecordPresent(std::numeric_limits<double>::quiet_NaN());
  assert(counter.Value() == displayed);
  counter.RecordPresent(0);  // Clock/device restart drops previous history.
  assert(counter.Value() == 0);
  assert(counter.FrameTimeMilliseconds()==0);
  for (unsigned i = 1; i <= 1800; ++i) counter.RecordPresent(i / 600.0);
  assert(counter.Value() == 600);  // Bounded ring wraps safely at high rates.
}
