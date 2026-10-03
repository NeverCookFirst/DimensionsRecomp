#include <cassert>
#include <cstdint>
#include <limits>
#include "gpu_native/present_rect.h"
using legodimensions::gpu_native::FitPresentRect;

int main() {
  auto r = FitPresentRect(1280, 720, 1920, 1080);
  assert(r.x == 0 && r.y == 0 && r.width == 1920 && r.height == 1080);
  r = FitPresentRect(1280, 720, 720, 1280);
  assert(r.x == 0 && r.y == 437 && r.width == 720 && r.height == 405);
  r = FitPresentRect(1280, 720, 3440, 1440);
  assert(r.x == 440 && r.y == 0 && r.width == 2560 && r.height == 1440);
  r = FitPresentRect(1280, 720, 1001, 1001);
  assert(r.x == 0 && r.y == 219 && r.width == 1001 && r.height == 563);
  r = FitPresentRect(1280, 720, 1, 1);
  assert(r.width == 1 && r.height == 1);
  r = FitPresentRect(1280, 720, 0, 0);
  assert(r.width == 0 && r.height == 0);
  r = FitPresentRect(0, 0, 1920, 1080);
  assert(r.width == 1920 && r.height == 1080);
  // A resize may jump between portrait and landscape. Every fitted rectangle
  // remains centered and bounded, with less than one pixel of rounding error.
  for (uint32_t w = 1; w <= 4096; w += 17) {
    for (uint32_t h = 1; h <= 2160; h += 13) {
      r = FitPresentRect(1280, 720, w, h);
      assert(r.width && r.height && r.x + r.width <= w && r.y + r.height <= h);
      assert(w - r.width - 2 * r.x <= 1 && h - r.height - 2 * r.y <= 1);
      assert(r.width == w || r.height == h);
      if (w >= 2 && h >= 2) {
        const int64_t error = int64_t(r.width) * 720 - int64_t(r.height) * 1280;
        assert(error > -720 && error < 1280);
      }
    }
  }
  const auto maximum = std::numeric_limits<uint32_t>::max();
  r = FitPresentRect(maximum, maximum - 1, maximum - 1, maximum);
  assert(r.width <= maximum - 1 && r.height <= maximum);
}
