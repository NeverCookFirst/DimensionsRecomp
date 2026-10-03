#pragma once
#include <cstdint>

namespace legodimensions::gpu_native {
struct PlacementBufferInfo {
  uint32_t address = 0;
  uint32_t size = 0;
  uint32_t index_format = 0;  // Native convention: 1 = 16-bit, 2 = 32-bit.
};

// TU23 asset headers observed in game.log; matches re:Blue's physical-buffer
// adoption. These are initialized fetch headers, not XGSet*BufferHeader inputs.
constexpr PlacementBufferInfo DecodePlacementBuffer(uint32_t common,
    uint32_t fetch_lo, uint32_t fetch_hi, bool index) {
  if ((common & 31u) != (index ? 2u : 1u)) return {};
  const uint32_t address = fetch_lo & ~3u;
  const uint32_t size = fetch_hi & 0x03FFFFFCu;
  if (!address || !size || uint64_t(address) + size > 0x100000000ull) return {};
  return {address, size, index ? ((common & 0x80000000u) ? 2u : 1u) : 0u};
}
}
