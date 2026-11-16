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

// A field with this bit holds two 16-bit components (8in16 guest data) whose
// shader input has no swapFloats() fix-up (COLOR, BLENDINDICES, ...): after
// the DWORD swap its halves arrive exchanged, so the upload swaps them back.
// Fields without it are TT UBYTE4[N] WZYX dwords (4-byte reversal).
constexpr uint32_t kHalfSwapField = 0x80000000u;
constexpr uint32_t VertexFieldOffset(uint32_t field) { return field & ~kHalfSwapField; }

// Input bytes have already undergone the common guest DWORD endian swap.
// The TT UBYTE4[N] declaration swizzle WZYX then reverses the four components.
// Only the upload copy is changed; floats, other fields and guest RAM stay intact.
constexpr bool ApplyVertexByteOrder(uint8_t* data, size_t size,
                                    const VertexByteOrder& order) {
  if (order.reversed_elements.empty()) return true;
  if (order.stride < 4 || order.stream_offset > size) return false;
  for (uint32_t field : order.reversed_elements)
    if (VertexFieldOffset(field) > order.stride - 4) return false;
  for (uint64_t base = order.stream_offset; base < size; base += order.stride) {
    for (uint32_t field : order.reversed_elements) {
      const uint64_t offset = base + VertexFieldOffset(field);
      if (offset + 4 > size) continue;
      const uint8_t a = data[offset], b = data[offset + 1];
      if (field & kHalfSwapField) {
        data[offset] = data[offset + 2];
        data[offset + 1] = data[offset + 3];
        data[offset + 2] = a;
        data[offset + 3] = b;
        continue;
      }
      data[offset] = data[offset + 3];
      data[offset + 1] = data[offset + 2];
      data[offset + 2] = b;
      data[offset + 3] = a;
    }
  }
  return true;
}

// Adds the half-swapped dwords of `slot` to a draw's conversion list. Fixture
// declarations without the mask compile this to nothing.
template <class Declaration>
inline void AppendHalfSwapFields(const Declaration& declaration, uint32_t slot,
                                 uint32_t* storage, uint32_t& count, uint32_t capacity) {
  if constexpr (requires { declaration.half_swapped_elements; }) {
    const uint32_t elements = declaration.element_count < 64 ? declaration.element_count : 64;
    for (uint32_t e = 0; e < elements; ++e) {
      if (!((declaration.half_swapped_elements >> e) & 1) ||
          declaration.elements[e].slotIndex != slot) continue;
      const uint32_t offset = declaration.elements[e].alignedByteOffset;
      const uint32_t dwords = ((declaration.half_swapped_wide_elements >> e) & 1) ? 2 : 1;
      for (uint32_t d = 0; d < dwords && count < capacity; ++d)
        storage[count++] = (offset + 4 * d) | kHalfSwapField;
    }
  }
}

}  // namespace legodimensions::gpu_native
