#pragma once
#include <cstdint>

namespace legodimensions::gpu_native {
// Xenos 32bpp color and depth use opposite 40-sample halves of each 80x16
// EDRAM tile. This path covers equal-pitch, equal-base, single-sample views.
constexpr uint32_t DepthColorAliasX(uint32_t x) {
  return x % 80 < 40 ? x + 40 : x - 40;
}
constexpr float UnpackDepth24(uint32_t depth) {
  // Same UNORM24 conversion as the SDK render-target cache (within half a
  // 24-bit unit, monotonic, with exact 0/1 endpoints).
  return float(depth + (depth >> 23)) * (1.0f / 16777216.0f);
}
struct DepthAliasLayout {
  uint32_t physical_base, width, height, pitch, tiled, endian, levels, dimension;
  bool operator==(const DepthAliasLayout&) const = default;
};
constexpr bool CompatibleDepthAlias(DepthAliasLayout a, DepthAliasLayout b) {
  return a == b && a.physical_base && a.width && a.height &&
      a.levels == 1 && a.dimension == 1;
}
}  // namespace legodimensions::gpu_native
