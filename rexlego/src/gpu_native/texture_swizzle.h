#pragma once
#include <cstdint>

namespace legodimensions::gpu_native {

// Xenos expands scalar/two-channel formats before applying fetch.swizzle.
// Desktop R/RG and BC4/BC5 views instead fill missing channels with 0/1.
// Values are six-bit Xenos TextureFormat IDs, not full D3DFORMAT words.
constexpr uint32_t TextureFormatChannelSwizzle(uint32_t format) {
  switch (format) {
    case 2: case 8: case 9:             // 8, 8_A, 8_B
    case 22: case 23:                  // D24 depth sampling
    case 30: case 36: case 58: case 59:  // 16_FLOAT, 32_FLOAT, DXT3A, DXT5A
      return 0;                       // RRRR
    case 10: case 13: case 25:          // 8_8, 16_16_EDRAM, 16_16
    case 31: case 37: case 49:          // 16_16_FLOAT, 32_32_FLOAT, DXN
      return (1u << 3) | (1u << 6) | (1u << 9);  // RGGG
    default:
      return 0x688;                   // RGBA, existing supported four-channel formats
  }
}

constexpr uint32_t NativeTextureSwizzle(uint32_t format, uint32_t guest) {
  const uint32_t channels = TextureFormatChannelSwizzle(format);
  uint32_t result = 0;
  for (uint32_t lane = 0; lane < 4; ++lane) {
    const uint32_t requested = (guest >> (lane * 3)) & 7;
    // Xenos/SDK sanitizes reserved selectors 6/7 to constant zero/one.
    const uint32_t selected = requested < 4 ? (channels >> (requested * 3)) & 7
        : requested & 5;
    result |= selected << (lane * 3);
  }
  return result;
}

}  // namespace legodimensions::gpu_native
