#pragma once
#include <cstdint>
#include <span>
#if defined(_M_X64) || defined(__x86_64__) || defined(__SSE2__)
#include <emmintrin.h>
#endif

namespace legodimensions::gpu_native {
struct TextureWritebackMergeResult {
  bool valid = false;
  bool cpu_modified = false;
};

// Match ReXGlue's memexport retirement: publish each GPU byte only if the
// destination still equals its pre-wait snapshot. Preserve later CPU bytes.
// Equal-value/ABA writes have the same limitation as the reference rule.
inline TextureWritebackMergeResult MergeTextureWriteback(
    std::span<uint8_t> destination, std::span<const uint8_t> gpu,
    std::span<const uint8_t> snapshot) {
  if (destination.size() != gpu.size() || gpu.size() != snapshot.size()) return {};
  bool modified = false;
  std::size_t i = 0;
#if defined(_M_X64) || defined(__x86_64__) || defined(__SSE2__)
  for (; destination.size() - i >= 16; i += 16) {
    const auto current = _mm_loadu_si128(reinterpret_cast<const __m128i*>(destination.data() + i));
    const auto previous = _mm_loadu_si128(reinterpret_cast<const __m128i*>(snapshot.data() + i));
    const auto exported = _mm_loadu_si128(reinterpret_cast<const __m128i*>(gpu.data() + i));
    const auto unchanged = _mm_cmpeq_epi8(current, previous);
    modified |= _mm_movemask_epi8(unchanged) != 0xFFFF;
    const auto merged = _mm_or_si128(_mm_and_si128(unchanged, exported),
                                    _mm_andnot_si128(unchanged, current));
    _mm_storeu_si128(reinterpret_cast<__m128i*>(destination.data() + i), merged);
  }
#endif
  for (; i < destination.size(); ++i) {
    if (destination[i] == snapshot[i]) destination[i] = gpu[i];
    else modified = true;
  }
  return {true, modified};
}
}  // namespace legodimensions::gpu_native
