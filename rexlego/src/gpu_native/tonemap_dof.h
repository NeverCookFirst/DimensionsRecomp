// The game's disabled DoF setting must also disable the final sharp/mip mix.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace legodimensions::gpu_native {
inline bool ApplyDisabledTonemapDof(std::uint64_t pixel_shader_hash, bool disabled,
                                    void* pixel_constants, std::size_t bytes) {
  constexpr std::size_t kMixOffset = 12 * 16;
  if (!disabled || pixel_shader_hash != UINT64_C(0x3A47E5DDE66B42C6) ||
      !pixel_constants || bytes < kMixOffset + 2 * sizeof(float)) return false;
  // HLSL: w=saturate(2*(mip.a*c12.x+c12.y)-1), col=lerp(full,mip.rgb,w).
  // Resolved mipColor1 is UNORM (finite alpha); x=y=0 gives w=0.
  // Preserve c12.z (output alpha threshold), c12.w, exposure, bloom and LUT.
  const float disabled_mix[2] = {0.0f, 0.0f};
  std::memcpy(static_cast<std::uint8_t*>(pixel_constants) + kMixOffset,
              disabled_mix, sizeof(disabled_mix));
  return true;
}
}  // namespace legodimensions::gpu_native
