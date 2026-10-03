#include "../../rexlego/src/gpu_native/buffer_header.h"
using namespace legodimensions::gpu_native;
constexpr auto vertex = DecodePlacementBuffer(1, 0xF933E003, 0x100AF002, false);
static_assert(vertex.address == 0xF933E000 && vertex.size == 0xAF000);
constexpr auto index16 = DecodePlacementBuffer(0x20000002, 0xF8823000, 0x1000, true);
static_assert(index16.address == 0xF8823000 && index16.size == 0x1000 && index16.index_format == 1);
constexpr auto index32 = DecodePlacementBuffer(0xA0000002, 0xF8823000, 0x1000, true);
static_assert(index32.index_format == 2);
static_assert(!DecodePlacementBuffer(1, 0xF933E003, 0x100AF002, true).address);
static_assert(!DecodePlacementBuffer(6, 0xF933E003, 0x100AF002, false).address);
static_assert(!DecodePlacementBuffer(1, 3, 0x1000, false).address);
static_assert(!DecodePlacementBuffer(1, 0xF0000000, 2, false).address);
static_assert(!DecodePlacementBuffer(1, 0xFFFFFFF0, 0x1000, false).address);
int main() {}
