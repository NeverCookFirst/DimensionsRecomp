#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

#include "gpu_native/pool_copy.h"

namespace legodimensions::gpu_native {

// One bit per 4 KB page of the 512 MB physical space: which pages hold any part of a registry's resources.
class PhysicalPageBitmap {
 public:
  PhysicalPageBitmap() : words_(kPages / 64) {}
  void Clear() { std::fill(words_.begin(), words_.end(), 0); }
  void Mark(PhysicalCopyRange range) {
    if (!range.length) return;
    const uint64_t first = range.address >> 12;
    const uint64_t last = (uint64_t(range.address) + range.length - 1) >> 12;
    for (uint64_t page = first; page <= last && page < kPages; ++page) words_[page >> 6] |= 1ull << (page & 63);
  }
  bool Any(PhysicalCopyRange range) const {
    if (!range.length) return false;
    const uint64_t first = range.address >> 12;
    const uint64_t last = (uint64_t(range.address) + range.length - 1) >> 12;
    for (uint64_t page = first; page <= last && page < kPages; ++page)
      if (words_[page >> 6] & (1ull << (page & 63))) return true;
    return false;
  }

 private:
  static constexpr uint64_t kPages = (512ull << 20) >> 12;
  std::vector<uint64_t> words_;
};

// Rebuilt only when the owning registry's footprint key changes; capture thread only.
struct FootprintIndex {
  PhysicalPageBitmap pages;
  uint64_t key[2] = {~0ull, ~0ull};
  bool Current(uint64_t a, uint64_t b) const { return key[0] == a && key[1] == b; }
};

}  // namespace legodimensions::gpu_native
