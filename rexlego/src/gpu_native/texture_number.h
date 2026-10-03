#pragma once
#include <cmath>
#include <cstdint>

namespace legodimensions::gpu_native {
// Host storage is UNORM16 even when the guest resolves integer values into it.
inline float Unsigned16ResolveScale(int exponent, bool integer) {
  return std::ldexp(integer ? (1.0f / 65535.0f) : 1.0f, exponent);
}
inline float Unsigned16SampleScale(int exponent, bool integer) {
  return std::ldexp(integer ? 65535.0f : 1.0f, exponent);
}
inline int ColorExponentBias(uint32_t color_info) {
  const uint32_t value = (color_info >> 20) & 63;
  return value < 32 ? int(value) : int(value) - 64;
}
inline float ColorOutputScale(uint32_t color_info) {
  const uint32_t format = (color_info >> 16) & 15;
  return std::ldexp(1.0f, ColorExponentBias(color_info) -
      ((format == 4 || format == 5) ? 5 : 0));
}
}  // namespace legodimensions::gpu_native
