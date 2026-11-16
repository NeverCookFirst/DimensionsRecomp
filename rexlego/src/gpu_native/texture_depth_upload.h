#pragma once

#include <cstring>
#include <span>

#include "depth_alias.h"
#include "texture_volume_upload.h"

namespace legodimensions::gpu_native {

// The SDK DepthUnorm loader first applies fetch endianness, discards the
// low eight stencil bits, then normalizes the remaining UNORM24 depth.
// SourceOffset uses the SDK guest layout, including tiled row padding.
template <typename SourceOffset>
bool CopyTextureDepth24(std::span<const uint8_t> source,
                        std::span<uint8_t> destination,
                        uint32_t width, uint32_t height, uint32_t row_pitch,
                        uint32_t endian_xor, SourceOffset source_offset) {
  if (!CopyTextureVolumeBlocks(source, destination, width, height, 1, 4,
          row_pitch, endian_xor, source_offset)) return false;
  for (uint32_t y = 0; y < height; ++y) {
    for (uint32_t x = 0; x < width; ++x) {
      auto* pixel = destination.data() + uint64_t(y) * row_pitch + uint64_t(x) * 4;
      uint32_t packed;
      std::memcpy(&packed, pixel, sizeof(packed));
      const float depth = UnpackDepth24(packed >> 8);
      std::memcpy(pixel, &depth, sizeof(depth));
    }
  }
  return true;
}

}  // namespace legodimensions::gpu_native
