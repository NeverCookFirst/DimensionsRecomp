#pragma once
#include <cstdint>

namespace legodimensions::gpu_native {
// Validate the bytes actually read by one attribute, not the entire trailing
// stride: a valid last record need not contain unused padding.
constexpr bool VertexFetchRangeFits(uint32_t minimum, uint32_t maximum,
    int32_t base, uint32_t stride, uint32_t stream_offset,
    uint32_t attribute_offset, uint32_t attribute_bytes, uint32_t buffer_bytes) {
  const int64_t first = int64_t(minimum) + base;
  const int64_t last = int64_t(maximum) + base;
  if (minimum > maximum || first < 0 || last < first || last > UINT32_MAX || !attribute_bytes ||
      attribute_offset > stride || attribute_bytes > stride - attribute_offset)
    return false;
  const uint64_t vertex_offset = uint64_t(last) * stride;
  if (vertex_offset > buffer_bytes) return false;
  const uint64_t end = uint64_t(stream_offset) + vertex_offset +
      attribute_offset + attribute_bytes;
  return end <= buffer_bytes;
}
} // namespace legodimensions::gpu_native
