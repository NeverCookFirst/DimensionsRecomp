#pragma once

#include "gpu_native/pool_copy.h"
#include <array>
#include <algorithm>

namespace legodimensions::gpu_native {
// Immutable capture of header, base and mip storage. Addresses are canonical
// physical bytes, including the SDK's E/F alias bias, never guest aliases.
struct TexturePreparationFootprint {
  std::array<PhysicalCopyRange, 3> ranges{};
  bool unknown = false;
};

// Conservative bounded union for a preparation chunk or batch. Overflow and
// an unreadable/changed capture match every query rather than losing a wait.
struct PreparationPhysicalFilter {
  static constexpr size_t kRanges = 64;
  std::array<PhysicalCopyRange, kRanges> ranges{};
  unsigned count = 0;
  bool unknown = false;

  void Add(PhysicalCopyRange range) {
    if (!range.length || unknown) return;
    if (range.address >= 0x20000000u || range.length > 0x20000000u - range.address) {
      unknown = true; return;
    }
    uint32_t first = range.address, end = first + range.length;
    // Revisit the swapped entry after merging: a union can join a chain.
    for (unsigned i = 0; i < count;) {
      const auto old = ranges[i];
      if (first <= old.address + old.length && old.address <= end) {
        first = std::min(first, old.address); end = std::max(end, old.address + old.length);
        ranges[i] = ranges[--count]; i = 0;
      } else ++i;
    }
    if (count == ranges.size()) { unknown = true; return; }
    ranges[count++] = {first, end - first};
  }
  void Add(const TexturePreparationFootprint& footprint) {
    unknown |= footprint.unknown;
    for (auto range : footprint.ranges) Add(range);
  }
  bool Matches(const PreparationPhysicalFilter& other) const {
    if ((!count && !unknown) || (!other.count && !other.unknown)) return false;
    if (unknown || other.unknown) return true;
    for (unsigned i = 0; i < count; ++i)
      for (unsigned j = 0; j < other.count; ++j)
        if (CopyRangesOverlap(ranges[i], other.ranges[j])) return true;
    return false;
  }
};
}  // namespace legodimensions::gpu_native
