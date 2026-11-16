#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include "gpu_native/vertex_byte_order.h"

namespace legodimensions::gpu_native {

struct VertexLayoutPlan {
  struct Stream {
    std::array<uint32_t, 64> reversal{};
    uint32_t reversal_count = 0;
    uint64_t minimum_stride = 0;
    bool active = false, widths_valid = true;
    std::span<const uint32_t> Reversal() const { return {reversal.data(), reversal_count}; }
  };
  std::array<Stream, 16> streams{};
  bool valid = true;
};

template<class Declaration, class FormatSize>
VertexLayoutPlan PrepareVertexLayout(const Declaration& declaration, FormatSize format_size,
                                     uint32_t stream_count = 16) {
  VertexLayoutPlan result;
  for (uint32_t e = 0; e < declaration.element_count; ++e) {
    const auto& element = declaration.elements[e];
    if (element.slotIndex >= std::min(stream_count, uint32_t(result.streams.size()))) {
      result.valid = false;
      return result;
    }
    auto& stream = result.streams[element.slotIndex];
    stream.active = true;
    const uint32_t width = format_size(element.format);
    stream.widths_valid &= width != 0;
    stream.minimum_stride = std::max(stream.minimum_stride, uint64_t(element.alignedByteOffset) + width);
    if (e < 64 && (declaration.reversed_byte_elements & (uint64_t{1} << e)))
      stream.reversal[stream.reversal_count++] = element.alignedByteOffset;
  }
  for (uint32_t slot = 0; slot < result.streams.size(); ++slot) {
    auto& stream = result.streams[slot];
    AppendHalfSwapFields(declaration, slot, stream.reversal.data(), stream.reversal_count, uint32_t(stream.reversal.size()));
    auto end = stream.reversal.begin() + stream.reversal_count;
    std::sort(stream.reversal.begin(), end);
    stream.reversal_count = uint32_t(std::unique(stream.reversal.begin(), end) - stream.reversal.begin());
  }
  return result;
}

} // namespace legodimensions::gpu_native
