#pragma once
#include <cstdint>

namespace legodimensions::gpu_native {
struct ColorBlendState {
  uint32_t source, destination, operation;
  uint32_t alpha_source, alpha_destination, alpha_operation;
  bool enabled, supported;
};
constexpr bool SupportedBlendFactor(uint32_t value) {
  // Constant alpha for RGB needs a distinct host representation, not silently
  // substituting constant RGB. Keep that case explicitly unsupported for now.
  return value <= 1 || (value >= 4 && value <= 13) || value == 16;
}
constexpr ColorBlendState DecodeColorBlend(uint32_t control) {
  ColorBlendState state{control & 31, (control >> 8) & 31, (control >> 5) & 7,
      (control >> 16) & 31, (control >> 24) & 31, (control >> 21) & 7};
  state.enabled = (control & 0x1FFF1FFFu) != 0x00010001u;
  state.supported = state.operation <= 4 && state.alpha_operation <= 4 &&
      SupportedBlendFactor(state.source) && SupportedBlendFactor(state.destination) &&
      SupportedBlendFactor(state.alpha_source) && SupportedBlendFactor(state.alpha_destination);
  return state;
}
}  // namespace legodimensions::gpu_native
