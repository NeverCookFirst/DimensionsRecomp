#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#if defined(__AVX2__) || defined(__SSE2__) || defined(_M_X64)
#include <immintrin.h>
#endif

namespace legodimensions::gpu_native {

inline bool EqualConstantBytesSimd(const void* first, const void* second, size_t bytes) {
  auto* a = static_cast<const uint8_t*>(first);
  auto* b = static_cast<const uint8_t*>(second);
#if defined(__AVX2__)
  while (bytes >= 128) {
    auto delta = _mm256_xor_si256(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(a)),
                                _mm256_loadu_si256(reinterpret_cast<const __m256i*>(b)));
    for (unsigned i = 32; i < 128; i += 32)
      delta = _mm256_or_si256(delta, _mm256_xor_si256(
          _mm256_loadu_si256(reinterpret_cast<const __m256i*>(a + i)),
          _mm256_loadu_si256(reinterpret_cast<const __m256i*>(b + i))));
    if (!_mm256_testz_si256(delta, delta)) return false;
    a += 128; b += 128; bytes -= 128;
  }
#endif
#if defined(__AVX2__) || defined(__SSE2__) || defined(_M_X64)
  while (bytes >= 16) {
    const auto equal = _mm_cmpeq_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(a)),
                                     _mm_loadu_si128(reinterpret_cast<const __m128i*>(b)));
    if (_mm_movemask_epi8(equal) != 0xFFFF) return false;
    a += 16; b += 16; bytes -= 16;
  }
#endif
  return !bytes || std::memcmp(a, b, bytes) == 0;
}

inline bool EqualConstantBytes(const void* first, const void* second, size_t bytes) {
  static const bool simd = [] {
    const char* value = std::getenv("LEGO_NATIVE_SIMD_CONSTANT_COMPARE");
    return !value || value[0] != '0';
  }();
  return simd ? EqualConstantBytesSimd(first, second, bytes) : std::memcmp(first, second, bytes) == 0;
}

} // namespace legodimensions::gpu_native
