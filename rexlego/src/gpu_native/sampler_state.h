#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cmath>

#include <plume_render_interface_types.h>

namespace legodimensions::gpu_native {

using TextureFetchWords = std::array<uint32_t, 6>;

// TU23 83FB58A8, before its Xbox fence/reference tracking. Input words are
// host endian; the texture header and the device shadow remain big endian.
inline TextureFetchWords MergeTextureFetch(const TextureFetchWords& old,
                                           const TextureFetchWords& header,
                                           uint8_t requested_min,
                                           uint8_t requested_max) {
  TextureFetchWords result;
  result[0] = (header[0] & ~0x003FFC00u) | (old[0] & 0x003FFC00u);
  const auto physical_word = [](uint32_t value) {
    return (value & 0x1FFFFFFFu) + (((value >> 20) + 512u) & 0x1000u);
  };
  result[1] = (physical_word(header[1]) & ~0x800u) | (old[1] & 0x800u);
  result[2] = header[2];
  result[3] = (header[3] & ~0x7FF80000u) | (old[3] & 0x7FF80000u);
  result[4] = (header[4] & 0x3FCu) | (old[4] & ~0x3FCu);
  const uint32_t mip_min = std::max((header[4] >> 2) & 15u, uint32_t(requested_min));
  const uint32_t mip_max = std::min((header[4] >> 6) & 15u, uint32_t(requested_max));
  result[4] = (result[4] & ~0x3FCu) | ((mip_min & 15u) << 2) | ((mip_max & 15u) << 6);
  result[5] = ((header[5] & 0x1FFFFE00u) +
               (((header[5] >> 20) + 512u) & 0x1000u)) | (old[5] & 0x1FFu);
  return result;
}

inline void ClearTextureFetch(TextureFetchWords& words) { words[0] &= ~3u; }

// Apply only to scene pixel textures, after the resource/header mip merge.
// Vertex animation, depth, UI, point-filtered and basemap-only fetches must
// retain their original sampler. Clamp the signed fixed-point field, never wrap.
inline void ApplyFsrTextureBias(TextureFetchWords& fetch, float bias) {
  if (!std::isfinite(bias) || bias == 0 || ((fetch[5] >> 9) & 3u) != 1 ||
      !((fetch[5] >> 12) & 0x1FFFFu) || ((fetch[3] >> 23) & 3u) == 2 ||
      (!((fetch[3] >> 25) & 7u) && (((fetch[3] >> 19) & 3u) != 1 ||
                                  ((fetch[3] >> 21) & 3u) != 1)) ||
      ((fetch[4] >> 6) & 15u) <= ((fetch[4] >> 2) & 15u)) return;
  int32_t old = int32_t((fetch[4] >> 12) & 1023u);
  if (old >= 512) old -= 1024;
  const int32_t delta = int32_t(std::round(std::clamp(bias,-16.0f,16.0f)*32));
  const int32_t value = std::clamp(old+delta,-512,511);
  fetch[4] = (fetch[4] & ~(1023u << 12)) | ((uint32_t(value) & 1023u) << 12);
}

// Only fields consumed by the desktop sampler are keys. No padding or texture
// addresses: cached descriptors never change until the host device is idle.
struct NativeSamplerKey {
  std::array<uint32_t, 5> words{};
  bool operator==(const NativeSamplerKey&) const = default;
};

inline plume::RenderSamplerDesc DecodeTextureSampler(const TextureFetchWords& fetch) {
  using Address = plume::RenderTextureAddressMode;
  constexpr Address modes[] = {Address::WRAP, Address::MIRROR, Address::CLAMP,
      Address::MIRROR_ONCE, Address::CLAMP, Address::MIRROR_ONCE,
      Address::BORDER, Address::MIRROR_ONCE};
  plume::RenderSamplerDesc desc;
  const uint32_t dimension = (fetch[5] >> 9) & 3u;
  desc.addressU = dimension == 3 ? Address::CLAMP : modes[(fetch[0] >> 10) & 7u];
  desc.addressV = dimension == 0 || dimension == 3 ? Address::CLAMP : modes[(fetch[0] >> 13) & 7u];
  desc.addressW = dimension == 2 ? modes[(fetch[0] >> 16) & 7u] : Address::CLAMP;
  const uint32_t mag = (fetch[3] >> 19) & 3u;
  const uint32_t min = (fetch[3] >> 21) & 3u;
  const uint32_t mip = (fetch[3] >> 23) & 3u;
  const uint32_t aniso = std::min((fetch[3] >> 25) & 7u, 5u);
  desc.anisotropyEnabled = aniso != 0;
  desc.maxAnisotropy = aniso ? 1u << (aniso - 1u) : 1u;
  desc.magFilter = aniso || mag == 1 ? plume::RenderFilter::LINEAR : plume::RenderFilter::NEAREST;
  desc.minFilter = aniso || min == 1 ? plume::RenderFilter::LINEAR : plume::RenderFilter::NEAREST;
  desc.mipmapMode = aniso || mip == 1 ? plume::RenderMipmapMode::LINEAR : plume::RenderMipmapMode::NEAREST;
  // Xenia adds this bias in generated shaders. Native currently emits Sample
  // for implicit LOD, so use the host sampler there. Explicit register LOD is
  // a separate compiler gap; this does not claim to implement it.
  const int32_t bias = int32_t((fetch[4] >> 12) & 0x3FFu);
  desc.mipLODBias = float(bias >= 512 ? bias - 1024 : bias) / 32.0f;
  desc.borderColor = (fetch[5] & 3u) == 1 ? plume::RenderBorderColor::OPAQUE_WHITE
                                         : plume::RenderBorderColor::TRANSPARENT_BLACK;

  uint32_t longest_minus_one;
  if (dimension == 0) longest_minus_one = fetch[2] & 0xFFFFFFu;
  else if (dimension == 2)
    longest_minus_one = std::max({fetch[2] & 0x7FFu, (fetch[2] >> 11) & 0x7FFu, fetch[2] >> 22});
  else longest_minus_one = std::max(fetch[2] & 0x1FFFu, (fetch[2] >> 13) & 0x1FFFu);
  const uint32_t size_max = std::bit_width(longest_minus_one + 1u) - 1u;
  uint32_t min_level = 0, max_level = 0;
  if ((fetch[5] >> 12) & 0x1FFFFu) {
    min_level = std::min((fetch[4] >> 2) & 15u, size_max);
    max_level = std::max(min_level, std::min((fetch[4] >> 6) & 15u, size_max));
    if (max_level && !((fetch[1] >> 12) & 0x1FFFFu)) min_level = std::max(min_level, 1u);
  }
  desc.minLOD = float(min_level);
  desc.maxLOD = float(std::max(min_level, max_level));
  if (mip == 2) desc.maxLOD = desc.minLOD + (aniso ? 0.0f : 0.25f);
  return desc;
}

// Xenia's anisotropic_override (texture_cache.cpp GetSamplerParameters): 0..5
// forces 1x..16x on linear, mipmapped, non-basemap samplers; -1 or 6 keeps the
// fetch constant's value.
inline void ApplyAnisotropicOverride(plume::RenderSamplerDesc& desc,
                                     const TextureFetchWords& fetch, int32_t override_level) {
  if (override_level < 0 || override_level > 5) return;
  const uint32_t mip = (fetch[3] >> 23) & 3u;
  const bool has_mips = ((fetch[5] >> 12) & 0x1FFFFu) != 0 && desc.maxLOD > desc.minLOD;
  const bool min_mag_linear = desc.minFilter == plume::RenderFilter::LINEAR &&
                              desc.magFilter == plume::RenderFilter::LINEAR;
  if (!has_mips || mip == 2 || !min_mag_linear) return;
  desc.anisotropyEnabled = override_level != 0;
  desc.maxAnisotropy = override_level ? 1u << (override_level - 1) : 1u;
}

inline NativeSamplerKey TextureSamplerKey(const plume::RenderSamplerDesc& desc) {
  return {{{uint32_t(desc.addressU) | (uint32_t(desc.addressV) << 3) |
             (uint32_t(desc.addressW) << 6) | (uint32_t(desc.minFilter) << 9) |
             (uint32_t(desc.magFilter) << 11) | (uint32_t(desc.mipmapMode) << 13) |
             (uint32_t(desc.anisotropyEnabled) << 15) | (desc.maxAnisotropy << 16) |
             (uint32_t(desc.borderColor) << 21),
            std::bit_cast<uint32_t>(desc.mipLODBias),
            std::bit_cast<uint32_t>(desc.minLOD),
            std::bit_cast<uint32_t>(desc.maxLOD), 0u}}};
}

}  // namespace legodimensions::gpu_native
