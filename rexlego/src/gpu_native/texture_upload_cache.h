#pragma once

#include <cstdint>
#include <span>
#include <xxhash.h>

namespace legodimensions::gpu_native {

struct TextureUploadSourceKey {
  uint64_t base_hash;
  uint64_t mip_hash;
  uint64_t fetch_hash;
  uint64_t base_size;
  uint64_t mip_size;
  bool operator==(const TextureUploadSourceKey&) const = default;
};

// The caller checks residency BEFORE constructing these spans. Hash all base
// faces and the whole mip tail on every use, not only once per frame: CPU-side
// texture changes must remain visible without an explicit dirty notification.
inline TextureUploadSourceKey MakeTextureUploadSourceKey(
    std::span<const uint8_t> base, std::span<const uint8_t> mips,
    std::span<const uint8_t> fetch) {
  return {XXH3_64bits(base.data(), base.size()),
          XXH3_64bits(mips.data(), mips.size()),
          XXH3_64bits(fetch.data(), fetch.size()), base.size(), mips.size()};
}

}  // namespace legodimensions::gpu_native
