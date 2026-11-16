#pragma once
#include <array>
#include <cstdint>
#include <cstdlib>

namespace legodimensions::gpu_native {
// Diagnostic only. Caller holds the recording lock. The last SetTexture call
// is a reference point, not attribution to the writer of a later difference.
struct TextureFetchSnapshot {
  uint32_t device = 0, guest = 0, caller = 0;
  uint64_t ordinal = 0;
  std::array<uint32_t, 6> fetch{};
};
class TextureFetchHistory {
 public:
  void Record(uint32_t device, uint32_t slot, uint32_t guest, uint32_t caller,
              const std::array<uint32_t, 6>& fetch) {
    if (slot >= slots_.size()) return;
    slots_[slot] = {device, guest, caller, ++ordinal_, fetch};
  }
  const TextureFetchSnapshot* Find(uint32_t device, uint32_t slot) const {
    if (slot >= slots_.size() || !slots_[slot].ordinal || slots_[slot].device != device)
      return nullptr;
    return &slots_[slot];
  }
  static uint32_t ChangedWords(const std::array<uint32_t, 6>& before,
                               const std::array<uint32_t, 6>& after,
                               bool resource_only) {
    // SetTexture resource bits. Mip requests are sampler state, and may change
    // legally after a bind; physical base/mip addresses remain resource fields.
    constexpr uint32_t masks[]{0xFFC003FF,0xFFFFF7FF,0xFFFFFFFF,0x8007FFFF,0,0xFFFFFE00};
    uint32_t changed = 0;
    for (uint32_t i = 0; i < 6; ++i)
      if ((before[i] ^ after[i]) & (resource_only ? masks[i] : ~uint32_t{0})) changed |= 1u << i;
    return changed;
  }
 private:
  std::array<TextureFetchSnapshot,26> slots_{};
  uint64_t ordinal_ = 0;
};
inline TextureFetchHistory g_texture_fetch_history;
inline bool TextureFetchHistoryEnabled() {
  static const bool enabled = [] {const char* p = std::getenv("LEGO_NATIVE_DRAW_TRACE_TRIGGER");return p && *p;}();
  return enabled;
}
} // namespace legodimensions::gpu_native
