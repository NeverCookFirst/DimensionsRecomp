#pragma once
#include <cstdint>

namespace legodimensions::gpu_native {
// Guest BGRA uploads use ZYX in RGB. Native color resolves already contain
// logical RGBA. Preserve the guest alpha selection, including constant one
// used by the title/menu transition, while removing this storage conversion.
constexpr uint32_t NativeColorResolveSwizzle(uint32_t guest_swizzle) {
  if ((guest_swizzle & 0x1FFu) != 0x00Au) return guest_swizzle;
  const uint32_t alpha = (guest_swizzle >> 9) & 7u;
  const uint32_t logical_alpha = alpha == 0 ? 2 : alpha == 2 ? 0 : alpha;
  return (guest_swizzle & ~0xFFFu) | (logical_alpha << 9) | 0x088u;
}
} // namespace legodimensions::gpu_native
