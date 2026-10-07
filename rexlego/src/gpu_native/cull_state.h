#pragma once
#include <cstdint>

namespace legodimensions::gpu_native {
// The low three bits are TU23 PA_SU_SC_MODE_CNTL, not PC D3DCULL values.
// Keep the candidate bit in pipeline identity so legacy state stays distinct.
constexpr std::uint32_t kNativeCullCandidate = 1u << 8;
struct NativeCullState {
  std::uint32_t faces = 0;  // 0 none, 1 front, 2 back, 3 both.
  bool clockwise = false;
  bool discard = false;
};
constexpr NativeCullState DecodeNativeCullState(std::uint32_t control,
                                                bool polygonal) {
  const std::uint32_t faces = polygonal ? control & 3u : 0;
  return {faces, (control & 4u) != 0, faces == 3};
}
constexpr std::uint32_t NativeStencilWordOffset(std::uint32_t control,
    std::uint32_t depth_control, bool polygonal) {
  // D3D12 has a single dynamic reference/mask. Like the SDK, use the back
  // values only when two-sided stencil is enabled and only back faces survive.
  return (control & kNativeCullCandidate) && polygonal && (depth_control & 128u) &&
      (control & 3u) == 1u ? 10492u : 10496u;
}
constexpr std::uint32_t CullPolygonOffsetMode(std::uint32_t mode,
    std::uint32_t control, bool polygonal) {
  if ((control & kNativeCullCandidate) && polygonal) {
    if (control & 1u) mode &= ~(1u << 11);
    if (control & 2u) mode &= ~(1u << 12);
  }
  return mode;
}
}  // namespace legodimensions::gpu_native
