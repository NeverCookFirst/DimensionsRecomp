#include <algorithm>
#include <cassert>
#include <cmath>
#include "../../rexlego/src/gpu_native/texture_number.h"
using namespace legodimensions::gpu_native;
float transfer(float value, int resolve_exp, int sample_exp, bool integer) {
  const float unorm = std::clamp(value * Unsigned16ResolveScale(resolve_exp, integer), 0.0f, 1.0f);
  const float stored = std::round(unorm * 65535.0f) / 65535.0f;
  return stored * Unsigned16SampleScale(sample_exp, integer);
}
int main() {
  assert(ColorOutputScale(5u << 16) == 1.0f / 32);
  assert(ColorOutputScale(4u << 16) == 1.0f / 32);
  assert(ColorOutputScale(7u << 16) == 1);
  assert(ColorExponentBias(59u << 20) == -5);
  assert(ColorOutputScale((5u << 16) | (5u << 20)) == 1);
  assert(ColorOutputScale((7u << 16) | (59u << 20)) == 1.0f / 32);
  // Captured HDR: UINT16 resolve +11, fetch integer -13. Expected /4,
  // retaining the 16-bit quantization, not the former saturate(value*2048).
  for (float value : {0.0f, 0.1f, 1.0f, 16.0f, 30.391f})
    assert(std::abs(transfer(value, 11, -13, true) - value * 0.25f) < 1.0f / 8192.0f);
  assert(transfer(-1, 11, -13, true) == 0);
  assert(std::abs(transfer(100, 11, -13, true) - 65535.0f / 8192.0f) < 0.00001f);
  assert(Unsigned16ResolveScale(0, false) == 1);
  assert(Unsigned16SampleScale(0, false) == 1);
  assert(Unsigned16SampleScale(5, false) == 32); // half-resolution color fetch
  assert(std::abs(transfer(0.5f, 0, 5, false) - 16) < 0.001f);
}
