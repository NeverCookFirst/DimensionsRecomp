#pragma once
#if defined(_WIN32)
#include <array>
#include <cstdint>
#include <windows.h>

namespace legodimensions::gpu_native {
struct CrashMemoryWords {
  std::array<uint32_t, 64> words{};
  uint32_t count = 0;
};
// Only used during fatal reporting. Each bounded read is independent of guest
// allocator locks; an inaccessible page preserves the preceding readable words.
inline CrashMemoryWords ReadCrashMemoryWords(const uint8_t* base, uint32_t address,
                                            uint32_t requested = 64) {
  CrashMemoryWords out;
  if (!base || !address) return out;
  const uint32_t limit = requested < out.words.size() ? requested : uint32_t(out.words.size());
  for (uint32_t i = 0; i < limit; ++i) {
    const uint64_t offset = uint64_t(address) + uint64_t(i) * 4;
    if (offset + 4 > 0x100000000ull) break;
    uint32_t raw = 0; SIZE_T copied = 0;
    if (!ReadProcessMemory(GetCurrentProcess(), base + offset, &raw, 4, &copied) || copied != 4) break;
    out.words[out.count++] = __builtin_bswap32(raw);
  }
  return out;
}
} // namespace legodimensions::gpu_native
#endif
