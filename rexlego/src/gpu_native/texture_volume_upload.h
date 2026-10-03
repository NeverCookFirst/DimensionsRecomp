#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace legodimensions::gpu_native {

// Source addressing comes from the guest layout (including 3D tiling and mip
// tail offsets). D3D12 upload slices use the actual block-row count, not the
// guest's tile-aligned Z stride. Keep decoding in cacheable CPU memory.
template <typename SourceOffset>
bool CopyTextureVolumeBlocks(std::span<const uint8_t> source,
                             std::span<uint8_t> destination,
                             uint32_t width_blocks, uint32_t height_blocks,
                             uint32_t depth, uint32_t block_bytes,
                             uint32_t row_pitch, uint32_t endian_xor,
                             SourceOffset source_offset) {
  if (!width_blocks || !height_blocks || !depth || !block_bytes || endian_xor > 3 ||
      uint64_t(width_blocks) * block_bytes > row_pitch) return false;
  const uint64_t slice_pitch = uint64_t(row_pitch) * height_blocks;
  if (slice_pitch > destination.size() / depth) return false;
  for (uint32_t z = 0; z < depth; ++z) {
    for (uint32_t y = 0; y < height_blocks; ++y) {
      for (uint32_t x = 0; x < width_blocks; ++x) {
        const int64_t offset = source_offset(x, y, z);
        if (offset < 0) return false;
        const uint64_t dst = z * slice_pitch + uint64_t(y) * row_pitch +
                             uint64_t(x) * block_bytes;
        for (uint32_t byte = 0; byte < block_bytes; ++byte) {
          const uint64_t src = (uint64_t(offset) + byte) ^ endian_xor;
          if (src >= source.size()) return false;
          destination[size_t(dst + byte)] = source[size_t(src)];
        }
      }
    }
  }
  return true;
}

}  // namespace legodimensions::gpu_native
