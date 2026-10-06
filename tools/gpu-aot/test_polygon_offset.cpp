#include "gpu_native/polygon_offset.h"
#include <cassert>
#include <limits>
using namespace legodimensions::gpu_native;
int main() {
  constexpr auto front = 1u << 11, back = 1u << 12, para = 1u << 13;
  assert(DecodePolygonOffset(0, true, 16, 1, 32, 2).depth_bias == 0);
  auto p = DecodePolygonOffset(front | back, true, 16, 1, 32, -1);
  assert(p.depth_bias == 16777215 && std::bit_cast<float>(p.slope_bits) == 1);
  p = DecodePolygonOffset(front | back, true, 0, 0, -32, -1);
  assert(p.depth_bias == -16777215 && std::bit_cast<float>(p.slope_bits) == -2);
  assert(DecodePolygonOffset(front, false, 16, 1, 0, 0).depth_bias == 0);
  assert(DecodePolygonOffset(para, false, 16, 1, 0, 0).depth_bias == 16777215);
  assert(DecodePolygonOffset(front, true, 0, 1.0f / 16777216.0f, 0, 0).depth_bias == 1);
  assert(DecodePolygonOffset(front, true, 0, -1.0f / 16777216.0f, 0, 0).depth_bias == -1);
  p = DecodePolygonOffset(front, true, std::numeric_limits<float>::infinity(),
      std::numeric_limits<float>::quiet_NaN(), 0, 0);
  assert(p.depth_bias == 0 && p.slope_bits == 0);
  assert(DecodePolygonOffset(front, true, 0, std::numeric_limits<float>::max(), 0, 0).depth_bias == 1000000000);
}
