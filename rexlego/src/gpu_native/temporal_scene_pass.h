#pragma once

#include <cstdint>
#include "gpu_native/render_extent.h"

namespace legodimensions::gpu_native {

// Inspected deferred lighting/fog programs reconstruct the existing scene from
// depth (s0), normals (s2) and albedo (s6). They do not introduce new geometry.
// Aliases have identical linked PS bytes for both alpha specializations.
// tools/gpu-aot/audit_temporal_scene_passes.py records the source/byte audit.
constexpr uint32_t TemporalSceneInputMask(uint64_t shader) {
  switch (shader) {
    case 0x081435C474061F7Eull: case 0xFC2BDE055D199EE8ull:
      // This volume light reconstructs position from s0. Its s6 multiplier
      // may be a constant 4x4 white image (captured intro draws12486/12525),
      // rather than the scene albedo. Only depth determines its geometry.
      return 0x01;
    case 0x0E2FA249CEC37FF0ull: case 0xB2A918B7FD4ECADCull:
    case 0x3DF8FF80E2A08043ull: case 0x32EAE4018791DA38ull:
    case 0x745F43DE42185A41ull: case 0xE85D26243917A70Dull:
    case 0x7DDDE95EB4B5A14Dull: case 0x04FB6886587A4462ull:
    case 0x82DB1B788F68E33Bull: case 0xF4CB6445FC4DF523ull:
    case 0xCEC8AAACB206BA39ull: case 0x4BB762F6258DC7D9ull:
      return 0x45;
    case 0xC700ED5C1AAD294Aull: case 0xFF08B82BB808075Full:
      return 0x01;
    default: return 0;
  }
}

// The caller also requires the full scene viewport and disabled depth testing.
// Check active sampled slots, not stale bindings or an arbitrary large image.
template <typename InputExtent>
bool PreservesTemporalSceneHistory(uint64_t shader, uint32_t sampled,
    NativeRenderExtent target, InputExtent input_extent) {
  const uint32_t required = TemporalSceneInputMask(shader);
  if (!required || (sampled & required) != required ||
      target.logical_width != 1280 || target.logical_height != 720 ||
      !target.width || !target.height) return false;
  for (uint32_t slot = 0; slot < 32; ++slot) {
    if (!(required & (1u << slot))) continue;
    const auto input = input_extent(slot);
    if (input.logical_width != target.logical_width ||
        input.logical_height != target.logical_height ||
        input.width != target.width || input.height != target.height) return false;
  }
  return true;
}

}  // namespace legodimensions::gpu_native
