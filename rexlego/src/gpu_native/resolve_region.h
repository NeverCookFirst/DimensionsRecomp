#pragma once
#include <algorithm>
#include <cstdint>
#include <optional>

namespace legodimensions::gpu_native {
struct ResolveRect { int32_t left, top, right, bottom; };
struct ResolvePoint { int32_t x, y; };
struct ResolveRegion { uint32_t left, top, width, height, x, y; };

// NULL rect selects the logical extent fitting both padded source and target.
// Explicit rectangles must fit exactly; silently clipping breaks atlas tiles.
inline std::optional<ResolveRegion> CheckedResolveRegion(
    uint32_t sw, uint32_t sh, uint32_t dw, uint32_t dh,
    const ResolveRect* rect = nullptr, const ResolvePoint* point = nullptr) {
  const ResolvePoint p = point ? *point : ResolvePoint{0, 0};
  if (p.x < 0 || p.y < 0 || uint32_t(p.x) >= dw || uint32_t(p.y) >= dh)
    return {};
  const ResolveRect r = rect ? *rect : ResolveRect{0, 0,
      int32_t(std::min(sw, dw - p.x)), int32_t(std::min(sh, dh - p.y))};
  if (r.left < 0 || r.top < 0 || r.right <= r.left || r.bottom <= r.top ||
      uint32_t(r.right) > sw || uint32_t(r.bottom) > sh) return {};
  const uint32_t w = r.right - r.left, h = r.bottom - r.top;
  if (w > dw - p.x || h > dh - p.y) return {};
  return ResolveRegion{uint32_t(r.left), uint32_t(r.top), w, h,
                       uint32_t(p.x), uint32_t(p.y)};
}
}  // namespace legodimensions::gpu_native
