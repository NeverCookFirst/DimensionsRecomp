#pragma once

#include <cstddef>
#include <cstdint>

namespace legodimensions::gpu_native {

// The source may end at a guest allocation boundary. Never inspect a section
// marker until its four bytes are within the caller's readable span.
inline size_t BoundedShaderContainerByteLength(const void* container,
                                               size_t readable_size) {
  constexpr size_t kHeaderSize = 0x24;
  constexpr size_t kMaximumSectionSize = 4 * 1024 * 1024;
  if (!container || readable_size < kHeaderSize) return 0;
  const auto* bytes = static_cast<const uint8_t*>(container);
  const auto read_word = [&](size_t offset) {
    uint32_t word = 0;
    for (size_t i = 0; i < 4; ++i) word = (word << 8) | bytes[offset + i];
    return word;
  };
  const size_t virtual_size = read_word(4);
  const size_t physical_size = read_word(8);
  if (virtual_size < kHeaderSize || virtual_size > kMaximumSectionSize ||
      physical_size > kMaximumSectionSize || virtual_size > readable_size ||
      physical_size > readable_size - virtual_size) return 0;
  const size_t remaining = readable_size - virtual_size;
  const size_t marker_size = remaining >= 4 && read_word(virtual_size) == physical_size ? 4 : 0;
  if (marker_size > remaining || physical_size > remaining - marker_size) return 0;
  return virtual_size + marker_size + physical_size;
}

}  // namespace legodimensions::gpu_native
