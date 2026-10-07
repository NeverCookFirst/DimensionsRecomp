#include "../../rexlego/src/gpu_native/texture_volume_upload.h"

#include <array>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <limits>
#include <span>
#include <vector>

using legodimensions::gpu_native::CopyTextureVolumeBlocks;
using u32 = uint32_t;

namespace {
void TestLinearSlice(u32 width_blocks, u32 height_blocks, u32 block_bytes) {
  const u32 guest_pitch = width_blocks * block_bytes + 32;
  constexpr u32 host_pitch = 256;
  // Packed mip offsets and guest row padding must not reach the host image.
  auto offset = [=](u32 x, u32 y, u32) -> int64_t {
    return uint64_t(y + 1) * guest_pitch + uint64_t(x + 1) * block_bytes;
  };
  for (u32 endian = 0; endian != 4; ++endian) {
    std::vector<uint8_t> source(size_t(offset(width_blocks - 1, height_blocks - 1, 0)) + block_bytes, 0xCD);
    std::vector<uint8_t> destination(host_pitch * height_blocks + 8, 0xFE);
    for (u32 y = 0; y < height_blocks; ++y)
      for (u32 x = 0; x < width_blocks; ++x)
        for (u32 byte = 0; byte < block_bytes; ++byte)
          source[(size_t(offset(x, y, 0)) + byte) ^ endian] = uint8_t(y * 37 + x * 11 + byte);
    auto image = std::span<uint8_t>(destination).first(host_pitch * height_blocks);
    assert(CopyTextureVolumeBlocks(source, image, width_blocks, height_blocks,
                                   1, block_bytes, host_pitch, endian, offset));
    for (u32 y = 0; y < height_blocks; ++y) {
      for (u32 x = 0; x < width_blocks; ++x)
        for (u32 byte = 0; byte < block_bytes; ++byte)
          assert(destination[y * host_pitch + x * block_bytes + byte] == uint8_t(y * 37 + x * 11 + byte));
      for (u32 byte = width_blocks * block_bytes; byte < host_pitch; ++byte)
        assert(destination[y * host_pitch + byte] == 0xFE);
    }
    for (size_t at = image.size(); at < destination.size(); ++at) assert(destination[at] == 0xFE);
    assert(!CopyTextureVolumeBlocks(std::span<const uint8_t>(source).first(source.size() - 1),
        image, width_blocks, height_blocks, 1, block_bytes, host_pitch, endian, offset));
    assert(!CopyTextureVolumeBlocks(source, image.first(image.size() - 1),
        width_blocks, height_blocks, 1, block_bytes, host_pitch, endian, offset));
    assert(!CopyTextureVolumeBlocks(source, image, width_blocks, height_blocks,
        1, block_bytes, width_blocks * block_bytes - 1, endian, offset));
  }
}

void TestNonlinearSlice() {
  // A cube face is independently bounded; a tile/packed-tail offset callback
  // may visit blocks in a different order than destination rows.
  std::vector<uint8_t> cube(6 * 256, 0xCD), destination(16, 0xFE);
  auto face = std::span<uint8_t>(cube).subspan(3 * 256, 96);
  auto offset = [](u32 x, u32 y, u32) -> int64_t {
    return 64 + (x * 2 + y) * 4;
  };
  for (u32 y = 0; y < 2; ++y)
    for (u32 x = 0; x < 2; ++x)
      for (u32 byte = 0; byte < 4; ++byte)
        face[size_t(offset(x, y, 0)) + byte] = uint8_t((y * 2 + x) * 4 + byte);
  assert(CopyTextureVolumeBlocks(face, destination, 2, 2, 1, 4, 8, 0, offset));
  for (u32 byte = 0; byte < 16; ++byte) assert(destination[byte] == byte);
  assert(!CopyTextureVolumeBlocks(face.first(79), destination, 2, 2, 1, 4, 8, 0, offset));
  assert(!CopyTextureVolumeBlocks(face, destination, 2, 2, 1, 4, 8, 4, offset));
  assert(!CopyTextureVolumeBlocks(face, destination, 2, 2, 1, 4, 8, 0,
      [](auto, auto, auto) { return int64_t{-1}; }));
  assert(!CopyTextureVolumeBlocks(face, destination, 2, 2, 1, 4, 8, 0,
      [](auto, auto, auto) { return std::numeric_limits<int64_t>::max(); }));
  // Endian XOR can address bytes beyond an otherwise in-range block offset.
  assert(!CopyTextureVolumeBlocks(face.first(5), destination, 1, 1, 1, 1, 1, 3,
      [](auto, auto, auto) { return int64_t{4}; }));
}
}  // namespace

int main() {
  TestLinearSlice(5, 3, 4);   // Odd-sized uncompressed 2D image.
  TestLinearSlice(2, 2, 8);   // 7x5 texels rounded to compressed blocks.
  TestLinearSlice(2, 2, 16);  // Sixteen-byte compressed blocks.
  TestNonlinearSlice();
  std::cout << "2D/cube bounded-copy regressions passed\n";
}
