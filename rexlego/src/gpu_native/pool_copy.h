#pragma once

#include <cstdint>

namespace legodimensions::gpu_native {

struct PhysicalCopyRange {
  uint32_t address = 0;
  uint32_t length = 0;
};

constexpr bool CopyRangesOverlap(PhysicalCopyRange a, PhysicalCopyRange b) {
  return a.length && b.length && uint64_t(a.address) < uint64_t(b.address) + b.length &&
         uint64_t(b.address) < uint64_t(a.address) + a.length;
}

// The chunk helper emits POINTLIST batches of at most 16384 64-byte records.
struct PoolCopyDraws {
  uint32_t records = 0;
  uint32_t consumed = 0;
  bool Consume(uint32_t primitive, uint32_t start, uint32_t count) {
    if (primitive != 1 || start != consumed || !count || count > 16384 ||
        consumed > records || count > records - consumed) return false;
    consumed += count;
    return true;
  }
};

// Includes the SDK's +0x1000 adjustment for E/F physical aliases.
bool GuestRangeOverlapsPoolCopy(uint32_t address, uint32_t size,
                               PhysicalCopyRange copy);

// Only active inside the original TU23 82BD2868 copy helper, after its bytes
// have been copied by the CPU. Ordinary memexport shaders aren't suppressed.
bool ConsumeCpuPoolCopyDraw(uint32_t primitive, uint32_t start, uint32_t count);

}  // namespace legodimensions::gpu_native
