#pragma once
#include <algorithm>
#include <cstdint>
#include <span>
#if defined(__AVX2__)
#include <immintrin.h>
#endif
#if defined(__SSSE3__) && defined(__SSE4_1__)
#include <smmintrin.h>
#include <tmmintrin.h>
#endif

namespace legodimensions::gpu_native {
struct IndexBounds { uint32_t min = UINT32_MAX, max = 0; };

// Read every index on every use, including direct guest writes. Vectorize the
// big-endian conversion and reduction rather than caching a mutable range.
inline IndexBounds ScanGuestIndexBounds(std::span<const uint8_t> bytes,
                                       uint32_t element_bytes) {
  IndexBounds result;
  if ((element_bytes != 2 && element_bytes != 4) || bytes.size() % element_bytes)
    return result;
  size_t offset = 0;
#if defined(__SSSE3__) && defined(__SSE4_1__)
  const auto swap = element_bytes == 2
      ? _mm_setr_epi8(1,0,3,2,5,4,7,6,9,8,11,10,13,12,15,14)
      : _mm_setr_epi8(3,2,1,0,7,6,5,4,11,10,9,8,15,14,13,12);
  auto low = _mm_set1_epi32(-1), high = _mm_setzero_si128();
#if defined(__AVX2__)
  if (bytes.size() >= 32) {
    const auto wide_swap = _mm256_broadcastsi128_si256(swap);
    auto wide_low = _mm256_set1_epi32(-1), wide_high = _mm256_setzero_si256();
    for (; bytes.size() - offset >= 32; offset += 32) {
      const auto values = _mm256_shuffle_epi8(_mm256_loadu_si256(
          reinterpret_cast<const __m256i*>(bytes.data() + offset)), wide_swap);
      wide_low = element_bytes == 2 ? _mm256_min_epu16(wide_low, values)
                                   : _mm256_min_epu32(wide_low, values);
      wide_high = element_bytes == 2 ? _mm256_max_epu16(wide_high, values)
                                    : _mm256_max_epu32(wide_high, values);
    }
    const auto low_a = _mm256_castsi256_si128(wide_low);
    const auto low_b = _mm256_extracti128_si256(wide_low, 1);
    const auto high_a = _mm256_castsi256_si128(wide_high);
    const auto high_b = _mm256_extracti128_si256(wide_high, 1);
    low = element_bytes == 2 ? _mm_min_epu16(low_a, low_b) : _mm_min_epu32(low_a, low_b);
    high = element_bytes == 2 ? _mm_max_epu16(high_a, high_b) : _mm_max_epu32(high_a, high_b);
  }
#endif
  for (; bytes.size() - offset >= 16; offset += 16) {
    const auto values = _mm_shuffle_epi8(_mm_loadu_si128(
        reinterpret_cast<const __m128i*>(bytes.data() + offset)), swap);
    low = element_bytes == 2 ? _mm_min_epu16(low, values) : _mm_min_epu32(low, values);
    high = element_bytes == 2 ? _mm_max_epu16(high, values) : _mm_max_epu32(high, values);
  }
  if (offset) {
    alignas(16) uint8_t lows[16], highs[16];
    _mm_store_si128(reinterpret_cast<__m128i*>(lows), low);
    _mm_store_si128(reinterpret_cast<__m128i*>(highs), high);
    for (unsigned i = 0; i < 16; i += element_bytes) {
      uint32_t a = 0, b = 0;
      for (unsigned j = 0; j < element_bytes; ++j) {
        a |= uint32_t(lows[i+j]) << (j*8);
        b |= uint32_t(highs[i+j]) << (j*8);
      }
      result.min = std::min(result.min, a); result.max = std::max(result.max, b);
    }
  }
#endif
  for (; offset < bytes.size(); offset += element_bytes) {
    uint32_t value = 0;
    for (unsigned j = 0; j < element_bytes; ++j)
      value = (value << 8) | bytes[offset+j];
    result.min = std::min(result.min, value); result.max = std::max(result.max, value);
  }
  return result;
}
} // namespace legodimensions::gpu_native
