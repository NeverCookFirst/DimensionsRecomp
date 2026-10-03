#include <array>
#include <cassert>
#include "gpu_native/color_resolve_swizzle.h"
using legodimensions::gpu_native::NativeColorResolveSwizzle;
constexpr std::array<unsigned, 4> Apply(unsigned swizzle,
                                       std::array<unsigned, 4> color) {
  std::array<unsigned, 4> output{};
  for (unsigned c = 0; c < 4; ++c) {
    unsigned component = (swizzle >> (3 * c)) & 7;
    output[c] = component < 4 ? color[component] : component == 5 ? 255 : 0;
  }
  return output;
}
int main() {
  static_assert(NativeColorResolveSwizzle(0x60A) == 0x688);
  static_assert(NativeColorResolveSwizzle(0xA0A) == 0xA88);
  static_assert(NativeColorResolveSwizzle(0x80A) == 0x888);
  // The exact transition variant must keep red red and cyan cyan, with opaque
  // alpha. The former raw ZYX1 view would exchange red and blue again.
  assert((Apply(NativeColorResolveSwizzle(0xA0A), {255, 0, 0, 37}) ==
          std::array<unsigned, 4>{255, 0, 0, 255}));
  assert((Apply(NativeColorResolveSwizzle(0xA0A), {0, 255, 255, 37}) ==
          std::array<unsigned, 4>{0, 255, 255, 255}));
  assert(Apply(0xA0A, {255, 0, 0, 37})[2] == 255);
  // Ordinary guest uploads retain their original view; only the distinct
  // resolved view uses this mapping. Other RGB layouts are left untouched.
  for (unsigned alpha = 0; alpha < 8; ++alpha) {
    unsigned guest = (alpha << 9) | 0xA;
    unsigned host = NativeColorResolveSwizzle(guest);
    // Applying the original view to actual BGRA storage must equal applying
    // the resolved view to logical RGBA, including alpha sourced from RGB.
    assert((host & 0x1FF) == 0x88);
    assert(Apply(guest, {53, 31, 17, 79}) == Apply(host, {17, 31, 53, 79}));
  }
  for (unsigned swizzle = 0; swizzle < 4096; ++swizzle)
    if ((swizzle & 0x1FF) != 0xA)
      assert(NativeColorResolveSwizzle(swizzle) == swizzle);
}
