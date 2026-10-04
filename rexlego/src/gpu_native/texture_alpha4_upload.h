#pragma once
#include <cstdint>
#include <span>

namespace legodimensions::gpu_native {

// DXT3A stores sixteen independent four-bit alpha values in an eight-byte
// block. It is not BC4, which stores endpoints and three-bit palette indices.
// Offset returns a guest block byte address, including tiling/packed-mip offsets.
template <class Offset>
bool CopyTextureAlpha4Blocks(std::span<const uint8_t> source,
                            std::span<uint8_t> destination,
                            uint32_t width, uint32_t height, uint32_t depth,
                            uint32_t pitch, uint32_t endian_xor, Offset offset) {
  if (!width || !height || !depth || pitch < width || endian_xor > 3 ||
      uint64_t(pitch) * height > destination.size() / depth) return false;
  for (uint32_t z = 0; z < depth; ++z) {
    for (uint32_t by = 0; by < (uint64_t(height) + 3) / 4; ++by) {
      for (uint32_t bx = 0; bx < (uint64_t(width) + 3) / 4; ++bx) {
        const int64_t start = offset(bx, by, z);
        if (start < 0) return false;
        uint8_t block[8];
        for (uint32_t byte = 0; byte < 8; ++byte) {
          const uint64_t at = (uint64_t(start) + byte) ^ endian_xor;
          if (at >= source.size()) return false;
          block[byte] = source[size_t(at)];
        }
        for (uint32_t y = 0; y < 4 && uint64_t(by) * 4 + y < height; ++y) {
          for (uint32_t x = 0; x < 4 && uint64_t(bx) * 4 + x < width; ++x) {
            const uint32_t texel = y * 4 + x;
            const uint8_t alpha = (block[texel / 2] >> ((texel & 1) * 4)) & 15;
            const uint64_t at = (uint64_t(z) * height + uint64_t(by) * 4 + y) * pitch + uint64_t(bx) * 4 + x;
            destination[size_t(at)] = alpha * 17;
          }
        }
      }
    }
  }
  return true;
}

}  // namespace legodimensions::gpu_native
