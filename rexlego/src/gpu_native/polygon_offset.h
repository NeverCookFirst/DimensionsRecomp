#pragma once
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>

namespace legodimensions::gpu_native {
struct PolygonOffset {
  int32_t depth_bias = 0;
  uint32_t slope_bits = 0;
};

// Native rasterization currently disables culling. Prefer an enabled front
// offset, falling back to the back offset when both front values are zero.
inline PolygonOffset DecodePolygonOffset(uint32_t mode, bool polygonal,
    float front_scale, float front_offset, float back_scale, float back_offset) {
  float scale = 0.0f, offset = 0.0f;
  if (polygonal) {
    if (mode & (1u << 11)) { scale = front_scale; offset = front_offset; }
    if ((mode & (1u << 12)) && scale == 0.0f && offset == 0.0f) {
      scale = back_scale; offset = back_offset;
    }
  } else if (mode & (1u << 13)) {
    scale = front_scale; offset = front_offset;
  }
  PolygonOffset result;
  if (std::isfinite(offset) && offset != 0.0f) {
    const int32_t magnitude = int32_t(std::min(
        std::ceil(std::abs(offset) * 16777215.0f), 1.0e9f));
    result.depth_bias = offset < 0.0f ? -magnitude : magnitude;
  }
  if (std::isfinite(scale) && scale != 0.0f)
    result.slope_bits = std::bit_cast<uint32_t>(scale * (1.0f / 16.0f));
  return result;
}
}  // namespace legodimensions::gpu_native
