#pragma once

#include <algorithm>
#include <cstdint>
#include <span>

namespace legodimensions::gpu_native {

struct NativeTileRect { int32_t left, top, right, bottom; };
struct NativeTileExtent { uint32_t width = 0, height = 0; };

// Support a complete, non-overlapping rectangle partition. Reject holes and
// overlaps rather than inventing pixels outside the guest's tiling contract.
inline NativeTileExtent LogicalTileExtent(std::span<const NativeTileRect> tiles) {
  if (tiles.empty() || tiles.size() > 16) return {};
  NativeTileExtent result;
  uint64_t area = 0;
  for (size_t i = 0; i < tiles.size(); ++i) {
    const auto& r = tiles[i];
    if (r.left < 0 || r.top < 0 || r.right <= r.left || r.bottom <= r.top ||
        r.right > 16384 || r.bottom > 16384) return {};
    result.width = std::max(result.width, uint32_t(r.right));
    result.height = std::max(result.height, uint32_t(r.bottom));
    area += uint64_t(r.right - r.left) * (r.bottom - r.top);
    for (size_t j = 0; j < i; ++j) {
      const auto& q = tiles[j];
      if (r.left < q.right && q.left < r.right &&
          r.top < q.bottom && q.top < r.bottom) return {};
    }
  }
  return area == uint64_t(result.width) * result.height ? result : NativeTileExtent{};
}

}  // namespace legodimensions::gpu_native
