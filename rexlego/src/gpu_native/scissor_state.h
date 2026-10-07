#pragma once
#include <algorithm>
#include <cstdint>
#include <cmath>

namespace legodimensions::gpu_native {
constexpr uint32_t kNativeScissorEnableOffset = 12288;
struct NativeScissorRect {
  int32_t left, top, right, bottom;
  bool operator==(const NativeScissorRect&) const = default;
};

inline bool NativeViewportValid(float x, float y, float width, float height,
                                  float min_z, float max_z) {
  // Empty TU23 clips retain their zero dimensions but submit no host draw.
  return std::isfinite(x) && std::isfinite(y) && std::isfinite(width) &&
         std::isfinite(height) && std::isfinite(min_z) && std::isfinite(max_z) &&
         width >= 0.0f && height >= 0.0f && min_z >= 0.0f && min_z <= 1.0f &&
         max_z >= 0.0f && max_z <= 1.0f;
}

// TU23 83FBA068 intersects the requested rectangle with the viewport only
// when 83FBA968's requested enable at +12288 is nonzero. Clamp the effective
// rectangle to host storage and collapse reversed bounds, preserving empties.
inline int32_t NativeViewportInteger(float value) {
  // Match TU23 fctiwz without an out-of-range C++ float-to-integer cast.
  if (std::isnan(value) || double(value) < double(INT32_MIN)) return INT32_MIN;
  if (double(value) >= double(INT32_MAX)) return INT32_MAX;
  return static_cast<int32_t>(value);
}

inline NativeScissorRect DecodeNativeScissor(float x, float y,
    float width, float height, NativeScissorRect requested, bool enabled,
    uint32_t target_width, uint32_t target_height) {
  int64_t left = NativeViewportInteger(x), top = NativeViewportInteger(y),
          right = left + NativeViewportInteger(width),
          bottom = top + NativeViewportInteger(height);
  if (enabled) {
    left = std::max(left, int64_t(requested.left));
    top = std::max(top, int64_t(requested.top));
    right = std::min(right, int64_t(requested.right));
    bottom = std::min(bottom, int64_t(requested.bottom));
  }
  const int64_t max_x = std::min(int64_t(target_width), int64_t(INT32_MAX));
  const int64_t max_y = std::min(int64_t(target_height), int64_t(INT32_MAX));
  left = std::clamp(left, int64_t(0), max_x);
  top = std::clamp(top, int64_t(0), max_y);
  right = std::clamp(right, left, max_x);
  bottom = std::clamp(bottom, top, max_y);
  return {int32_t(left), int32_t(top), int32_t(right), int32_t(bottom)};
}
}  // namespace legodimensions::gpu_native
