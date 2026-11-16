#pragma once
#include <cstdint>

namespace legodimensions::gpu_native {
struct VertexBufferWindow { uint32_t offset = 0, length = 0; };
// The rebased index is index-min_index; the upload starts at
// stream_offset+(min_index+original_base_vertex)*stride.
constexpr VertexBufferWindow DrawVertexWindow(uint32_t min_index, uint32_t max_index,
    int32_t base_vertex, uint32_t stride, uint32_t stream_offset, uint32_t buffer_length) {
  const int64_t first = int64_t(min_index) + base_vertex;
  const int64_t last = int64_t(max_index) + base_vertex;
  if (!stride || first < 0 || last < first) return {};
  const uint64_t offset = uint64_t(stream_offset) + uint64_t(first) * stride;
  const uint64_t length = uint64_t(last - first + 1) * stride;
  if (offset > buffer_length || length > buffer_length - offset ||
      offset % 4 || length % 4) return {};
  return {uint32_t(offset), uint32_t(length)};
}

// Nonindexed vertices form a contiguous range. Widen before adding count so
// an overflowing range cannot turn into a small apparently valid upload.
constexpr bool NonIndexedVertexRange(uint32_t start, uint32_t count, uint32_t& last) {
  if (!count || uint64_t(start) + count - 1 > UINT32_MAX) return false;
  last = uint32_t(uint64_t(start) + count - 1);
  return true;
}
}
