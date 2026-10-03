#include "../../rexlego/src/gpu_native/vertex_byte_order.h"
#include <array>

using namespace legodimensions::gpu_native;

constexpr bool TestColor() {
  // Two vertices: untouched float bits followed by the observed logo tint.
  std::array<uint8_t, 16> data{0, 0, 0, 0xBF, 0x1A, 0xFE, 0xFE, 0xFE,
                              0, 0, 0, 0x3F, 0x34, 0xFE, 0xFE, 0xFE};
  const std::array<uint32_t, 1> fields{4};
  return ApplyVertexByteOrder(data.data(), data.size(), {8, 0, fields}) &&
      data == std::array<uint8_t, 16>{0, 0, 0, 0xBF, 0xFE, 0xFE, 0xFE, 0x1A,
                                    0, 0, 0, 0x3F, 0xFE, 0xFE, 0xFE, 0x34};
}

constexpr bool TestOffsetAndTail() {
  std::array<uint8_t, 11> data{9, 9, 1, 2, 3, 4, 5, 6, 7, 8, 9};
  const std::array<uint32_t, 1> fields{0};
  return ApplyVertexByteOrder(data.data(), data.size(), {4, 2, fields}) &&
      data == std::array<uint8_t, 11>{9, 9, 4, 3, 2, 1, 8, 7, 6, 5, 9};
}

constexpr bool TestInvalid() {
  std::array<uint8_t, 4> data{1, 2, 3, 4};
  const std::array<uint32_t, 1> fields{1};
  return !ApplyVertexByteOrder(data.data(), data.size(), {4, 0, fields}) &&
      !ApplyVertexByteOrder(data.data(), data.size(), {0, 0, fields}) &&
      !ApplyVertexByteOrder(data.data(), data.size(), {8, 5, fields}) &&
      ApplyVertexByteOrder(data.data(), data.size(), {}) &&
      data == std::array<uint8_t, 4>{1, 2, 3, 4};
}

static_assert(TestColor());
static_assert(TestOffsetAndTail());
static_assert(TestInvalid());
static_assert(((0x00014C86 >> 10) & 0xFFF) == 0x053); // WZYX.
static_assert((0x00014C86 & 0x300) == 0); // Unsigned, normalized.
static_assert((0x00014E86 & 0x300) == 0x200); // Unsigned, integer.
int main() {}
