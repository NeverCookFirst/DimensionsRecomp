#pragma once
#include <algorithm>
#include <cstdint>
#include <span>

namespace legodimensions::gpu_native {

// Explicit Xenos state: the SDK adds VGT_INDX_OFFSET, retains 24 bits,
// then clamps to VGT_MIN/MAX_VTX_INDX. The renderer integration must obtain
// the actual title state, rather than infer it from buffer sizes.
struct MultiRateIndexState {
  uint32_t offset = 0;
  uint32_t minimum = 0;
  uint32_t maximum = 0xFFFFFF;
};

inline uint32_t MultiRateOutputVertexCount(uint32_t count, bool quads) {
  if (!quads) return count;
  const uint64_t expanded = uint64_t(count / 4) * 6;
  return count && count % 4 == 0 && expanded <= UINT32_MAX ? uint32_t(expanded) : 0;
}

// Indices are the original big-endian guest bytes, not a potentially resident
// GPU upload. Nonindexed start belongs to the implied vertex sequence; indexed
// start addresses the index buffer. Preserve repetition, order and quad winding.
// Validate all bounds and aliasing before publishing any output value.
inline bool BuildMultiRateVertexOrder(std::span<const uint8_t> indices,
    bool indexed, uint32_t index_bytes, uint32_t start, uint32_t count,
    bool quads, MultiRateIndexState state, std::span<uint32_t> output) {
  const auto vertices = MultiRateOutputVertexCount(count, quads);
  if (!vertices || output.size() < vertices || state.minimum > 0xFFFFFF ||
      state.maximum > 0xFFFFFF) return false;
  if (indexed) {
    if ((index_bytes != 2 && index_bytes != 4) ||
        uint64_t(start) + count > indices.size() / index_bytes) return false;
    const auto in = reinterpret_cast<uintptr_t>(indices.data());
    const auto out = reinterpret_cast<uintptr_t>(output.data());
    const uint64_t out_bytes = uint64_t(vertices) * sizeof(uint32_t);
    if (in <= out ? out - in < indices.size() : in - out < out_bytes) return false;
  }
  constexpr uint32_t corners[] = {0, 1, 2, 0, 2, 3};
  for (uint32_t i = 0; i < vertices; ++i) {
    const uint32_t source = quads ? (i / 6) * 4 + corners[i % 6] : i;
    uint32_t value = start + source;
    if (indexed) {
      const auto* p = indices.data() + (uint64_t(start) + source) * index_bytes;
      value = 0;
      for (uint32_t b = 0; b < index_bytes; ++b) value = (value << 8) | p[b];
    }
    value = (value + state.offset) & 0xFFFFFF;
    output[i] = std::min(state.maximum, std::max(state.minimum, value));
  }
  return true;
}
} // namespace legodimensions::gpu_native
