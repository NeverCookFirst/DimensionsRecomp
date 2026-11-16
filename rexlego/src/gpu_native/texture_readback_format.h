#pragma once
#include <cstdint>
#include "gpu_native/texture_endian.h"
namespace legodimensions::gpu_native {
// These host allocations contain raw guest channel bits after resolve packing.
// Numeric interpretation belongs to the separate sampling view, not readback.
enum class TextureReadbackFormat {
  kRgba8, kRg16, kRgba16,
  kR16Float, kRg16Float, kRgba16Float, kR32Float, kRg32Float, kRgba32Float,
  kUnsupported
};
constexpr uint32_t TextureReadbackTexelBytes(TextureReadbackFormat format) {
  switch(format) {
    case TextureReadbackFormat::kR16Float: return 2;
    case TextureReadbackFormat::kRgba8:
    case TextureReadbackFormat::kRg16:
    case TextureReadbackFormat::kRg16Float:
    case TextureReadbackFormat::kR32Float: return 4;
    case TextureReadbackFormat::kRgba16:
    case TextureReadbackFormat::kRgba16Float:
    case TextureReadbackFormat::kRg32Float: return 8;
    case TextureReadbackFormat::kRgba32Float: return 16;
    default: return 0;
  }
}
constexpr uint32_t TextureReadbackTexelLog2(TextureReadbackFormat format) {
  const auto bytes=TextureReadbackTexelBytes(format);
  return bytes==16?4:bytes==8?3:bytes==4?2:bytes==2?1:0;
}
}  // namespace legodimensions::gpu_native
