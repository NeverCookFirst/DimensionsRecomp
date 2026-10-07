#pragma once

#include <cstdint>

namespace legodimensions::gpu_native {

struct IndexDrawWindow {
  uint32_t offset = 0;
  uint32_t length = 0;
  explicit constexpr operator bool() const { return length != 0; }
};

// Six triangle indices replace every four QuadList vertices. The host draw
// count is uint32_t even though buffer allocation sizes use uint64_t.
constexpr uint32_t ExpandedQuadIndexCount(uint32_t count) {
  const uint64_t expanded = uint64_t(count / 4) * 6;
  return count && count % 4 == 0 && expanded <= UINT32_MAX ? uint32_t(expanded) : 0;
}

// This interval is independent of signed base_vertex: it covers index fetches,
// while base_vertex affects the subsequent vertex buffer addresses.
constexpr IndexDrawWindow DrawIndexWindow(uint32_t start, uint32_t count,
                                          uint32_t element_bytes,
                                          uint32_t buffer_length) {
  if (!count || (element_bytes != 2 && element_bytes != 4)) return {};
  const uint64_t offset = uint64_t(start) * element_bytes;
  const uint64_t length = uint64_t(count) * element_bytes;
  if (offset > buffer_length || length > buffer_length - offset) return {};
  return {uint32_t(offset), uint32_t(length)};
}

}  // namespace legodimensions::gpu_native
