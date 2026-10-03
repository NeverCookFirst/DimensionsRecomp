#pragma once
#include <algorithm>
#include <array>
#include <cstring>
#include <numeric>
#include "gpu_native/vertex_byte_order.h"
#if defined(__SSSE3__)
#include <tmmintrin.h>
#endif

namespace legodimensions::gpu_native {
// Fuse DWORD endian conversion and the packed-field reversal into one
// sequential, write-only upload. Unsupported layouts retain the old path.
inline bool WriteAlignedVertexUpload(uint8_t* destination, const uint8_t* source,
                                    size_t size, const VertexByteOrder& order) {
  if (order.reversed_elements.empty() || order.stride < 4 || order.stride > 256 ||
      order.stride % 4 || order.stream_offset % 4 || order.stream_offset > size)
    return false;
  uint32_t previous = 0;
  bool first = true;
  for (uint32_t field : order.reversed_elements) {
    if (field % 4 || field > order.stride - 4 || (!first && field <= previous)) return false;
    previous = field; first = false;
  }
  const auto packed = [&](size_t offset) {
    if (offset < order.stream_offset) return false;
    const auto field = uint32_t((offset - order.stream_offset) % order.stride);
    return std::binary_search(order.reversed_elements.begin(), order.reversed_elements.end(), field);
  };
  size_t offset = 0;
#if defined(__SSSE3__)
  // The stride and sixteen-byte SIMD chunks repeat after their LCM. Bound
  // this small table to64 chunks; every shuffle stays inside its own DWORD.
  const size_t blocks = order.stride / std::gcd(order.stride, 16u);
  alignas(16) std::array<std::array<uint8_t, 16>, 64> masks{};
  const size_t first_vector = (size_t(order.stream_offset) + 15) / 16 * 16;
  for (size_t b = 0; b < blocks; ++b)
    for (size_t lane = 0; lane < 16; lane += 4) {
      const bool identity = packed(first_vector + b * 16 + lane);
      for (size_t byte = 0; byte < 4; ++byte)
        masks[b][lane + byte] = uint8_t(lane + (identity ? byte : 3 - byte));
    }
  // A non-canonical caller may leave a long unconverted prefix. Handle it
  // before the repeating phase, without applying packed fields too early.
  for (; offset < std::min(first_vector, size / 4 * 4); offset += 4) {
    if (packed(offset)) std::memcpy(destination + offset, source + offset, 4);
    else for (size_t byte = 0; byte < 4; ++byte) destination[offset + byte] = source[offset + 3 - byte];
  }
  size_t block = 0;
  for (; size - offset >= 16; offset += 16) {
    const auto value = _mm_loadu_si128(reinterpret_cast<const __m128i*>(source + offset));
    const auto mask = _mm_load_si128(reinterpret_cast<const __m128i*>(masks[block].data()));
    _mm_storeu_si128(reinterpret_cast<__m128i*>(destination + offset), _mm_shuffle_epi8(value, mask));
    if (++block == blocks) block = 0;
  }
#endif
  for (; size - offset >= 4; offset += 4) {
    if (packed(offset)) std::memcpy(destination + offset, source + offset, 4);
    else for (size_t byte = 0; byte < 4; ++byte) destination[offset + byte] = source[offset + 3 - byte];
  }
  if (offset != size) std::memcpy(destination + offset, source + offset, size - offset);
  return true;
}
} // namespace legodimensions::gpu_native
