#pragma once
#include <cstdint>

namespace legodimensions::gpu_native {
struct NativeAlphaState {
  bool enabled;
  std::uint32_t function;
  float reference;
};
inline NativeAlphaState DecodeNativeAlphaState(std::uint32_t color_control,
                                               float reference) {
  // TU23 83FB7E00 / 83FB82C0: enable bit 3, raw Xenos compare bits 0..2.
  // Reference is the normalized float shadow at +10500. TU23 83FB8260
  // converts the integer ALPHAREF to float and multiplies by 1/255 there.
  // The separate float state written by 83FB92B8 at +10620 is not ALPHAREF.
  return {(color_control & 8u) != 0, color_control & 7u, reference};
}
}  // namespace legodimensions::gpu_native
