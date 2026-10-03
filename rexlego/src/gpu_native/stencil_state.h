#pragma once
#include <cstdint>

namespace legodimensions::gpu_native {
struct StencilFaceState {
  uint32_t comparison = 7, fail = 0, pass = 0, depth_fail = 0;
  bool operator==(const StencilFaceState&) const = default;
};
struct StencilState {
  bool enabled = false;
  StencilFaceState front, back;
  uint32_t reference = 0, read_mask = 255, write_mask = 255;
};
constexpr StencilState DecodeStencilState(uint32_t control, uint32_t ref_mask,
                                         bool has_stencil, bool polygonal) {
  StencilState result;
  result.enabled = has_stencil && (control & 1u);
  if (!result.enabled) return result;
  result.front = {(control >> 8) & 7u, (control >> 11) & 7u,
                  (control >> 14) & 7u, (control >> 17) & 7u};
  result.back = polygonal && (control & 128u)
      ? StencilFaceState{(control >> 20) & 7u, (control >> 23) & 7u,
                         (control >> 26) & 7u, (control >> 29) & 7u}
      : result.front;
  result.reference = ref_mask & 255u;
  result.read_mask = (ref_mask >> 8) & 255u;
  result.write_mask = (ref_mask >> 16) & 255u;
  return result;
}
}  // namespace legodimensions::gpu_native
