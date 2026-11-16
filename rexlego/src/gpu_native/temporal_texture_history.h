#pragma once
#include <array>
#include <cstdint>

namespace legodimensions::gpu_native {
// Previous constants may use current vertex textures only when their complete
// uploaded contents match the previous draw. GPU-owned/unhashed images have no
// such guarantee. An unobserved required binding must reject history too.
struct TemporalTextureHistory {
  std::array<uint64_t, 32> content{};
  uint32_t required = 0, observed = 0, verified = 0;
  void Observe(uint32_t slot, uint64_t hash, bool valid) {
    if (slot >= content.size() || !(required & (uint32_t{1} << slot))) return;
    const uint32_t bit = uint32_t{1} << slot;
    content[slot] = hash;
    observed |= bit;
    verified = valid ? verified | bit : verified & ~bit;
  }
  bool Complete() const { return (observed & verified & required) == required; }
};
}  // namespace legodimensions::gpu_native
