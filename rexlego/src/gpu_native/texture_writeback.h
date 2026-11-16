#pragma once
#include <cstdint>
#include <span>
#include "gpu_native/texture_writeback_coverage.h"
#include "gpu_native/texture_writeback_merge.h"
#include "gpu_native/texture_readback_format.h"

namespace legodimensions::gpu_native {
// Native scene storage supports uniform half-step scales. These ratios are
// exactly representable by the existing point-resolve shader (including the
// 150% mode). Integer scales also match ReXGlue's half-pixel readback selection.
constexpr bool TextureWritebackScaleSupported(uint32_t width, uint32_t height,
    uint32_t source_width, uint32_t source_height) {
  if (!width || !height || !source_width || !source_height) return false;
  const uint64_t x = uint64_t(source_width) * 2, y = uint64_t(source_height) * 2;
  return x % width == 0 && y % height == 0 && x / width == y / height &&
      x / width >= 2 && x / width <= 8;
}

constexpr uint32_t TextureWritebackSample(uint32_t index, uint32_t source, uint32_t destination) {
  return uint32_t((uint64_t(index) * source + source / 2) / destination);
}

// Inverse of the raw 16/32/64/128-bit texel upload address/endian path. Validate every destination
// before changing any guest byte; padding and bytes outside the image survive.
template<class Offset>
bool WritebackTextureRaw(std::span<const uint8_t> rgba, uint32_t width,
    uint32_t height, uint32_t row_pitch, std::span<uint8_t> guest,
    uint32_t endian_xor, bool swap_red_blue, Offset offset,
    uint32_t source_width, uint32_t source_height,
    const TextureWritebackCoverage* coverage, uint32_t texel_bytes) {
  if (!source_width && !source_height) { source_width = width; source_height = height; }
  if (!TextureWritebackScaleSupported(width,height,source_width,source_height) ||
      (texel_bytes != 2 && texel_bytes != 4 && texel_bytes != 8 && texel_bytes != 16) ||
      (swap_red_blue && texel_bytes != 4) ||
      endian_xor > 3 || uint64_t(source_width)*texel_bytes > row_pitch ||
      uint64_t(row_pitch)*source_height > rgba.size()) return false;
  if (coverage && !coverage->Matches(source_width,source_height)) return false;
  for (uint32_t y=0;y<height;++y) for (uint32_t x=0;x<width;++x) {
    const uint32_t sx=source_width==width ? x : TextureWritebackSample(x,source_width,width);
    const uint32_t sy=source_height==height ? y : TextureWritebackSample(y,source_height,height);
    if (coverage && !coverage->Contains(sx,sy)) continue;
    const int64_t at=offset(x,y);
    if (at<0) return false;
    for (uint32_t c=0;c<texel_bytes;++c)
      if (((uint64_t(at)+c)^endian_xor) >= guest.size()) return false;
  }
  for (uint32_t y=0;y<height;++y) for (uint32_t x=0;x<width;++x) {
    const uint32_t sx=source_width==width ? x : TextureWritebackSample(x,source_width,width);
    const uint32_t sy=source_height==height ? y : TextureWritebackSample(y,source_height,height);
    if (coverage && !coverage->Contains(sx,sy)) continue;
    const uint64_t at=uint64_t(offset(x,y)),src=uint64_t(sy)*row_pitch+uint64_t(sx)*texel_bytes;
    for (uint32_t c=0;c<texel_bytes;++c) {
      const uint32_t channel=swap_red_blue && (c==0 || c==2) ? 2-c : c;
      guest[size_t((at+c)^endian_xor)]=rgba[size_t(src+channel)];
    }
  }
  return true;
}
template<class Offset>
bool WritebackTextureRgba8(std::span<const uint8_t> rgba, uint32_t width,
    uint32_t height, uint32_t row_pitch, std::span<uint8_t> guest,
    uint32_t endian_xor, bool swap_red_blue, Offset offset,
    uint32_t source_width = 0, uint32_t source_height = 0,
    const TextureWritebackCoverage* coverage = nullptr) {
  return WritebackTextureRaw(rgba,width,height,row_pitch,guest,endian_xor,swap_red_blue,
      offset,source_width,source_height,coverage,4);
}
} // namespace legodimensions::gpu_native
