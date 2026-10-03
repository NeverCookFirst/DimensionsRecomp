#pragma once
#include <algorithm>
#include <cstdint>
#include <span>

namespace legodimensions::gpu_native {
// Big-endian Xenos instruction stream, not the containing physical section.
// Include every exec clause, even conditional and unreachable clauses. Unknown
// or malformed input keeps all bindings; no predicate is evaluated here.
inline uint32_t ShaderTextureUsage(std::span<const uint8_t> code) {
  constexpr uint32_t all = ~uint32_t{0};
  if (code.empty() || code.size() % 12) return all;
  const auto word = [&](size_t offset) {
    return uint32_t(code[offset]) << 24 | uint32_t(code[offset + 1]) << 16 |
           uint32_t(code[offset + 2]) << 8 | uint32_t(code[offset + 3]);
  };
  size_t cf_end = code.size() / 12;
  bool saw_exec = false;
  uint32_t mask = 0;
  for (size_t pair = 0; pair < cf_end; ++pair) {
    const uint32_t a = word(pair * 12), b = word(pair * 12 + 4), c = word(pair * 12 + 8);
    const uint32_t lo[] = {a, (b >> 16) | (c << 16)};
    const uint32_t hi[] = {b & 0xFFFF, c >> 16};
    for (size_t half = 0; half < 2; ++half) {
      const uint32_t opcode = hi[half] >> 12;
      if (!((opcode >= 1 && opcode <= 6) || opcode == 13 || opcode == 14)) continue;
      const size_t address = lo[half] & 0xFFF;
      const uint32_t count = (lo[half] >> 12) & 7;
      const uint32_t sequence = (lo[half] >> 16) & 0xFFF;
      if (count > 6 || address <= pair || address + count > code.size() / 12) return all;
      cf_end = std::min(cf_end, address);
      saw_exec = true;
      for (uint32_t i = 0; i < count; ++i) {
        if (!((sequence >> (i * 2)) & 1)) continue;
        const uint32_t fetch = word((address + i) * 12);
        const uint32_t fetch_opcode = fetch & 31;
        if (fetch_opcode == 0) continue; // vertex buffer fetch
        if (fetch_opcode != 1 && (fetch_opcode < 16 || fetch_opcode > 19) &&
            (fetch_opcode < 24 || fetch_opcode > 26)) return all;
        // Even texture-unit state operations retain their declared slot.
        mask |= uint32_t{1} << ((fetch >> 20) & 31);
      }
    }
  }
  return saw_exec ? mask : all;
}
} // namespace legodimensions::gpu_native
