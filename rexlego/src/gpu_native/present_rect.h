#pragma once
#include <algorithm>
#include <cstdint>

namespace legodimensions::gpu_native {
struct PresentRect {
  uint32_t x, y, width, height;
};

// Fit the complete game image, including HUD, into the host window. Integer
// products avoid aspect comparison errors and overflow at large dimensions.
constexpr PresentRect FitPresentRect(uint32_t source_width, uint32_t source_height,
                                     uint32_t window_width, uint32_t window_height) {
  if (!source_width || !source_height || !window_width || !window_height)
    return {0, 0, window_width, window_height};
  uint32_t width = window_width, height = window_height;
  if (uint64_t(window_width) * source_height > uint64_t(window_height) * source_width)
    width = std::max(1u, uint32_t(uint64_t(window_height) * source_width / source_height));
  else
    height = std::max(1u, uint32_t(uint64_t(window_width) * source_height / source_width));
  return {(window_width - width) / 2, (window_height - height) / 2, width, height};
}
} // namespace legodimensions::gpu_native
