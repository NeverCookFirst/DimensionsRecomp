#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <span>

namespace legodimensions::gpu_native {

// Three immutable CBVs: VS, PS and shared ABI624. Compare cacheable CPU bytes,
// never mapped UPLOAD memory. A process-wide recording serial forbids reuse
// after an arena reset, ring wrap, resize or device recreation. The owner must
// hold the recording lock; this cache does not own or extend GPU lifetimes.
template <typename Slice>
class DrawConstantCache {
 public:
  static constexpr std::array<uint32_t, 3> sizes{4096, 4096, 624};
  static constexpr uint32_t packet_size = 8816;
  static constexpr uint32_t Align(uint32_t size) { return (size + 255) & ~255u; }

  template <typename Allocate>
  bool Resolve(uint64_t serial, std::span<const uint8_t> packet,
               Allocate allocate, std::array<Slice, 3>& result) {
    if (packet.size() != packet_size) return false;
    if (!serial || serial != serial_) {
      serial_ = serial;
      valid_ = 0;
    }
    uint32_t changed = 0, required = 0, source_offset = 0;
    for (uint32_t i = 0; i < sizes.size(); ++i) {
      if (!(valid_ & (1u << i)) ||
          std::memcmp(shadow_.data() + source_offset,
                      packet.data() + source_offset, sizes[i]) != 0) {
        changed |= 1u << i;
        required += Align(sizes[i]);
      }
      source_offset += sizes[i];
    }
    Slice upload{};
    if (required) {
      upload = allocate(required);
      if (!upload) return false; // Never publish a failed allocation.
    }
    source_offset = 0;
    uint32_t upload_offset = 0;
    for (uint32_t i = 0; i < sizes.size(); ++i) {
      if (changed & (1u << i)) {
        auto slice = upload;
        slice.offset += upload_offset;
        slice.mapped = static_cast<uint8_t*>(upload.mapped) + upload_offset;
        std::memcpy(slice.mapped, packet.data() + source_offset, sizes[i]);
        std::memcpy(shadow_.data() + source_offset,
                    packet.data() + source_offset, sizes[i]);
        slices_[i] = slice;
        upload_offset += Align(sizes[i]);
      }
      result[i] = slices_[i];
      source_offset += sizes[i];
    }
    valid_ = serial ? 7u : 0u;
    return true;
  }

 private:
  uint64_t serial_ = 0;
  uint32_t valid_ = 0;
  std::array<uint8_t, packet_size> shadow_{};
  std::array<Slice, 3> slices_{};
};
} // namespace legodimensions::gpu_native
