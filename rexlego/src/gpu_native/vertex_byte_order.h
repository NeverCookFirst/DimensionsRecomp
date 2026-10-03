#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace legodimensions::gpu_native {

struct VertexByteOrder {
  uint32_t stride = 0;
  uint32_t stream_offset = 0;
  std::span<const uint32_t> reversed_elements;
};

// Different stream offsets at the same phase fetch the same converted bytes.
// Convert all records at that phase once, rather than a fresh suffix per draw.
constexpr VertexByteOrder CanonicalVertexByteOrder(VertexByteOrder order) {
  if (order.stride >= 4) order.stream_offset %= order.stride;
  return order;
}

// Input bytes have already undergone the common guest DWORD endian swap.
// The TT UBYTE4[N] declaration swizzle WZYX then reverses the four components.
// Only the upload copy is changed; floats, other fields and guest RAM stay intact.
constexpr bool ApplyVertexByteOrder(uint8_t* data, size_t size,
                                    const VertexByteOrder& order) {
  if (order.reversed_elements.empty()) return true;
  if (order.stride < 4 || order.stream_offset > size) return false;
  for (uint32_t field : order.reversed_elements)
    if (field > order.stride - 4) return false;
  for (uint64_t base = order.stream_offset; base < size; base += order.stride) {
    for (uint32_t field : order.reversed_elements) {
      const uint64_t offset = base + field;
      if (offset + 4 > size) continue;
      const uint8_t a = data[offset], b = data[offset + 1];
      data[offset] = data[offset + 3];
      data[offset + 1] = data[offset + 2];
      data[offset + 2] = b;
      data[offset + 3] = a;
    }
  }
  return true;
}

}  // namespace legodimensions::gpu_native
