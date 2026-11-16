#pragma once
#include <cstdint>
#include <optional>
#include "gpu_native/resolve_region.h"
namespace legodimensions::gpu_native {
struct NativeRenderExtent {
  uint32_t logical_width, logical_height, width, height;
  float ScaleX() const { return logical_width ? float(width) / logical_width : 1.0f; }
  float ScaleY() const { return logical_height ? float(height) / logical_height : 1.0f; }
};
inline int32_t NativeRenderEdge(int32_t edge, uint32_t logical, uint32_t physical) {
  return logical ? int32_t(int64_t(edge) * physical / logical) : edge;
}
// Copy/Load resolves require equal physical coverage at both ends. Reject
// mismatched scales rather than copying a cropped or stretched image.
inline std::optional<ResolveRegion> NativePhysicalResolve(
    ResolveRegion r, NativeRenderExtent s, NativeRenderExtent d,
    ResolveRegion* source_area=nullptr) {
  const auto sx = [&](uint32_t x) { return uint32_t(uint64_t(x)*s.width/s.logical_width); };
  const auto sy = [&](uint32_t y) { return uint32_t(uint64_t(y)*s.height/s.logical_height); };
  const auto dx = [&](uint32_t x) { return uint32_t(uint64_t(x)*d.width/d.logical_width); };
  const auto dy = [&](uint32_t y) { return uint32_t(uint64_t(y)*d.height/d.logical_height); };
  if (!s.logical_width || !s.logical_height || !d.logical_width || !d.logical_height) return {};
  const uint32_t w=sx(r.left+r.width)-sx(r.left), h=sy(r.top+r.height)-sy(r.top);
  const uint32_t dw=dx(r.x+r.width)-dx(r.x), dh=dy(r.y+r.height)-dy(r.y);
  if (!w || !h || !dw || !dh) return {};
  if (!source_area && (w!=dw || h!=dh)) return {};
  if (source_area) *source_area={sx(r.left),sy(r.top),w,h,dx(r.x),dy(r.y)};
  return ResolveRegion{sx(r.left),sy(r.top),dw,dh,dx(r.x),dy(r.y)};
}
} // namespace legodimensions::gpu_native
