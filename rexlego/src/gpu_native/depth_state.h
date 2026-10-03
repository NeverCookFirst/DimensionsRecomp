#pragma once
#include <cstdint>

namespace legodimensions::gpu_native {
struct DepthState {
  bool enabled;
  bool write;
  uint32_t comparison;  // Xenos: NEVER=0 ... ALWAYS=7.
};
// TU23 SetDepthStencilSurface re-applies the requested enable bits on every
// bind. A request made while no depth surface is bound must survive rebinding.
constexpr uint32_t RebindDepthStencilControl(uint32_t control, uint32_t depth_request,
                                             uint32_t stencil_request, bool bound) {
  return (control & ~3u) | (bound ? ((depth_request & 1u) << 1) |
                                          (stencil_request & 1u) : 0u);
}
constexpr DepthState DecodeDepthState(uint32_t control, bool has_depth) {
  const bool enabled = has_depth && (control & 2u);
  return {enabled, enabled && (control & 4u), enabled ? ((control >> 4) & 7u) : 7u};
}
static_assert(!DecodeDepthState(0, true).enabled);
static_assert(!DecodeDepthState(0x76, false).enabled);
static_assert(!DecodeDepthState(0x74, true).write);
static_assert(DecodeDepthState(0x32, true).comparison == 3);
static_assert(!DecodeDepthState(0x32, true).write);
static_assert(DecodeDepthState(0x46, true).write);
static_assert(DecodeDepthState(0x46, true).comparison == 4);
static_assert(DecodeDepthState(0xFFFFFF89, true).comparison == 7);
}  // namespace legodimensions::gpu_native
