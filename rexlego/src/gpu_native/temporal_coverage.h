#pragma once
#include <cstdlib>
#include <cstring>
#include "gpu_native/blend_state.h"

namespace legodimensions::gpu_native {
inline bool TemporalMaterialReactiveEnabled() {
  static const bool enabled = [] {
    const char* value = std::getenv("LEGO_NATIVE_MATERIAL_REACTIVE");
    return !value || std::strcmp(value, "0") != 0;
  }();
  return enabled;
}
// Values are private temporal-bank stage IDs; 0/1 remain the motion VS/PS.
enum class TemporalCoverage : uint32_t {
  kNone=0, kOpaque=2, kAlpha=3, kAddAlpha=4, kAdd=5, kPremultiplied=6
};
constexpr TemporalCoverage TemporalCoverageForBlend(ColorBlendState blend) {
  if (!blend.supported) return TemporalCoverage::kNone;
  if (!blend.enabled) return TemporalCoverage::kOpaque;
  if (blend.operation != 0) return TemporalCoverage::kNone;
  if (blend.source == 6 && blend.destination == 7) return TemporalCoverage::kAlpha;
  if (blend.source == 6 && blend.destination == 1) return TemporalCoverage::kAddAlpha;
  if (blend.source == 1 && blend.destination == 1) return TemporalCoverage::kAdd;
  if (blend.source == 1 && blend.destination == 7) return TemporalCoverage::kPremultiplied;
  return TemporalCoverage::kNone;
}
}  // namespace legodimensions::gpu_native
