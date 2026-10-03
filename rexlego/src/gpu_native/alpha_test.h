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
  // 83FB92B8 stores FLOAT BITS unchanged; no integer /255 or /256 mapping.
  return {(color_control & 8u) != 0, color_control & 7u, reference};
}
}  // namespace legodimensions::gpu_native
