#pragma once

#include <cstdint>
#include <new>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace legodimensions::gpu_native {
// Retained bytes for one upload transaction. The caller settles data producers
// and validates residency in Reader; this is not a metadata/provider bypass.
// Hashing, untile/decode and diagnostics must use these same immutable bytes.
class TextureUploadSource {
 public:
  template<class Reader>
  static std::optional<TextureUploadSource> Capture(uint32_t base_address, uint32_t base_size,
      uint32_t mip_address, uint32_t mip_size, Reader&& read) {
    constexpr uint32_t limit = 128u * 1024 * 1024;
    if (!base_address || !base_size || base_size > limit || mip_size > limit ||
        uint64_t(base_address) + base_size > 0x100000000ull ||
        (mip_size && (!mip_address || uint64_t(mip_address) + mip_size > 0x100000000ull))) return {};
    try {
      TextureUploadSource result;
      result.base_address_ = base_address; result.mip_address_ = mip_address;
      result.base_.resize(base_size); result.mips_.resize(mip_size);
      if (!read(base_address, std::span<uint8_t>(result.base_)) ||
          (mip_size && !read(mip_address, std::span<uint8_t>(result.mips_)))) return {};
      return result;
    } catch (const std::bad_alloc&) { return {}; }
  }

  std::span<const uint8_t> Base() const { return base_; }
  std::span<const uint8_t> Mips() const { return mips_; }
  // The slice address is in the same guest coordinate system as its capture.
  // Keep E/F bias in the reader rather than masking aliases in this lookup.
  std::span<const uint8_t> Slice(bool mips, uint32_t address, uint32_t length) const {
    if (!length || uint64_t(address) + length > 0x100000000ull) return {};
    const auto segment = mips ? std::pair(mip_address_, Mips()) : std::pair(base_address_, Base());
    if (address < segment.first) return {};
    const auto offset = uint64_t(address) - segment.first;
    if (offset <= segment.second.size() && length <= segment.second.size() - offset)
      return segment.second.subspan(size_t(offset), length);
    return {};
  }
 private:
  uint32_t base_address_ = 0, mip_address_ = 0;
  std::vector<uint8_t> base_, mips_;
};
}  // namespace legodimensions::gpu_native
