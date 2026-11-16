#pragma once
#include "gpu_native/constant_compare.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <span>
#include <utility>

namespace legodimensions::gpu_native {

template<class Key, class Value>
class PreparedValue {
 public:
  template<class Prepare>
  const Value& Resolve(const Key& key, Prepare prepare) {
    if (!valid_ || !(key_ == key)) {
      value_ = prepare();
      key_ = key;
      valid_ = true;
    }
    return value_;
  }

  void Reset() { valid_ = false; value_ = {}; }

 private:
  Key key_{};
  Value value_{};
  bool valid_ = false;
};

template<class Slice>
class PreparedConstantBanks {
 public:
  static constexpr std::array<uint32_t, 3> sizes{4096, 4096, 624};
  static constexpr std::array<uint32_t, 3> offsets{0, 4096, 8192};
  using Packet = std::array<uint8_t, 8816>;

  template<class Convert>
  bool UpdateFloatBank(uint32_t bank, std::span<const uint8_t> guest,
                       uint64_t transform, Convert convert) {
    if (bank >= 2 || guest.size() != sizes[bank]) return false;
    const uint32_t bit = 1u << bank;
    auto* raw = guest_.data() + offsets[bank];
    if ((prepared_ & bit) && transforms_[bank] == transform &&
        EqualConstantBytes(raw, guest.data(), guest.size())) return false;
    std::memcpy(raw, guest.data(), guest.size());
    convert(packet_.data() + offsets[bank], raw, sizes[bank]);
    transforms_[bank] = transform;
    prepared_ |= bit;
    uploaded_ &= ~bit;
    ++prepared_banks_;
    return true;
  }

  void UpdateShared(std::span<const uint8_t> shared) {
    if (shared.size() != sizes[2]) return;
    auto* output = packet_.data() + offsets[2];
    if (!(prepared_ & 4u) || !EqualConstantBytes(output, shared.data(), shared.size())) {
      std::memcpy(output, shared.data(), shared.size());
      prepared_ |= 4u;
      uploaded_ &= ~4u;
      ++prepared_banks_;
    }
  }

  void SetUploadRegisters(uint32_t vertex, uint32_t pixel) {
    const std::array<uint32_t, 3> next{
        Align(std::max(1u, std::min(vertex, 256u)) * 16),
        Align(std::max(1u, std::min(pixel, 256u)) * 16), sizes[2]};
    for (uint32_t i = 0; i < 2; ++i)
      if (next[i] > upload_sizes_[i]) uploaded_ &= ~(1u << i);
    upload_sizes_ = next;
  }

  template<class Allocate>
  bool ResolveUploads(uint64_t serial, Allocate allocate, std::array<Slice, 3>& result) {
    return ResolveUploads(serial, allocate, result,
        [](void* dst, const void* src, uint32_t bytes) { std::memcpy(dst, src, bytes); });
  }

  template<class Allocate, class Copy>
  bool ResolveUploads(uint64_t serial, Allocate allocate, std::array<Slice, 3>& result, Copy copy) {
    if (prepared_ != 7u) return false;
    if (!serial || serial != serial_) {
      serial_ = serial;
      uploaded_ = 0;
    }
    const uint32_t dirty = 7u & ~uploaded_;
    uint32_t bytes = 0;
    for (uint32_t i = 0; i < sizes.size(); ++i)
      if (dirty & (1u << i)) bytes += Align(upload_sizes_[i]);
    Slice allocation{};
    if (bytes) {
      allocation = allocate(bytes);
      if (!allocation) return false;
    }
    uint32_t offset = 0;
    for (uint32_t i = 0; i < sizes.size(); ++i) {
      if (dirty & (1u << i)) {
        auto slice = allocation;
        slice.offset += offset;
        slice.mapped = static_cast<uint8_t*>(allocation.mapped) + offset;
        copy(slice.mapped, packet_.data() + offsets[i], upload_sizes_[i]);
        slices_[i] = slice;
        offset += Align(upload_sizes_[i]);
      }
      result[i] = slices_[i];
    }
    uploaded_ = serial ? 7u : 0u;
    return true;
  }

  Packet& Bytes() { return packet_; }
  const Packet& Bytes() const { return packet_; }
  uint64_t PreparedBanks() const { return prepared_banks_; }
  void Reset() { prepared_ = uploaded_ = 0; serial_ = 0; slices_ = {}; }

 private:
  static constexpr uint32_t Align(uint32_t n) { return (n + 255u) & ~255u; }
  alignas(16) std::array<uint8_t, 8192> guest_{};
  alignas(16) Packet packet_{};
  std::array<uint64_t, 2> transforms_{};
  std::array<Slice, 3> slices_{};
  std::array<uint32_t, 3> upload_sizes_ = sizes;
  uint64_t serial_ = 0, prepared_banks_ = 0;
  uint32_t prepared_ = 0, uploaded_ = 0;
};

} // namespace legodimensions::gpu_native
