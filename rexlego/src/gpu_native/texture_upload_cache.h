#pragma once

#include <cstdint>
#include <array>
#include <cstring>
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

// A placement texture's allocation address does not affect block decoding.
// Keep layout/sampling semantics and allocation presence in the key, and hash
// ALL new backing bytes before reusing an upload after a pool relocation.
inline TextureUploadSourceKey MakeRelocatableTextureUploadSourceKey(
    std::span<const uint8_t> base, std::span<const uint8_t> mips,
    std::span<const uint8_t> fetch) {
  std::array<uint32_t, 6> layout;
  if (fetch.size() != sizeof(layout)) return MakeTextureUploadSourceKey(base, mips, fetch);
  std::memcpy(layout.data(), fetch.data(), sizeof(layout));
  for (const unsigned word : {1u, 5u}) {
    const bool allocated = (layout[word] >> 12) != 0;
    layout[word] = (layout[word] & 0xFFFu) | (uint32_t(allocated) << 12);
  }
  return MakeTextureUploadSourceKey(base, mips,
      {reinterpret_cast<const uint8_t*>(layout.data()), sizeof(layout)});
}

}  // namespace legodimensions::gpu_native
