#include "../../rexlego/src/gpu_native/texture_volume_upload.h"

#include <cassert>
#include <iostream>
#include <vector>

using legodimensions::gpu_native::CopyTextureVolumeBlocks;

void TestLinearVolume(uint32_t width, uint32_t height) {
  // Test both the observed 64x32x16 LUT and a 16^3 regression whose guest
  // Z slices have 32 padded rows while host upload slices have only 16.
  constexpr uint32_t depth = 16, pitch = 256;
  auto offset = [](uint32_t x, uint32_t y, uint32_t z) -> int64_t {
    return (uint64_t(z) * 32 + y) * pitch + x * 4;
  };
  for (uint32_t endian = 0; endian < 4; ++endian) {
    std::vector<uint8_t> source(size_t(offset(width - 1, height - 1, depth - 1)) + 4, 0xCD);
    std::vector<uint8_t> destination(pitch * height * depth, 0xFE);
    for (uint32_t z = 0; z < depth; ++z)
      for (uint32_t y = 0; y < height; ++y)
        for (uint32_t x = 0; x < width; ++x) {
          const uint8_t rgba[] = {uint8_t(x * 17), uint8_t(y * 17), uint8_t(z * 17), 255};
          for (uint32_t b = 0; b < 4; ++b)
            source[(size_t(offset(x, y, z)) + b) ^ endian] = rgba[b];
        }
    assert(CopyTextureVolumeBlocks(source, destination, width, height, depth, 4, pitch, endian, offset));
    for (uint32_t z = 0; z < depth; ++z)
      for (uint32_t y = 0; y < height; ++y) {
        const size_t row = (z * height + y) * pitch;
        for (uint32_t x = 0; x < width; ++x) {
          assert(destination[row + x * 4] == uint8_t(x * 17));
          assert(destination[row + x * 4 + 1] == uint8_t(y * 17));
          assert(destination[row + x * 4 + 2] == z * 17);
          assert(destination[row + x * 4 + 3] == 255);
        }
        if (width * 4 < pitch)
          assert(destination[row + width * 4] == 0xFE); // Do not copy guest padding.
      }
    auto short_source = std::span<const uint8_t>(source).first(source.size() - 1);
    assert(!CopyTextureVolumeBlocks(short_source, destination, width, height, depth, 4, pitch, endian, offset));
    auto short_dest = std::span<uint8_t>(destination).first(destination.size() - 1);
    assert(!CopyTextureVolumeBlocks(source, short_dest, width, height, depth, 4, pitch, endian, offset));
    assert(!CopyTextureVolumeBlocks(source, destination, width, height, depth, 4, 4, endian, offset));
    assert(!CopyTextureVolumeBlocks(source, destination, width, height, depth, 4, pitch, endian,
                                   [](auto, auto, auto) { return int64_t{-1}; }));
  }
}

int main() {
  // The residency/hash span must cover every byte actually addressed by XOR,
  // including the opposite half of a final odd-width half-float texel.
  for(uint32_t extent=0;extent<257;++extent)for(uint32_t mode=0;mode<4;++mode){
    const auto endian=legodimensions::gpu_native::TextureEndianXor(mode);
    const auto span=legodimensions::gpu_native::TextureEndianExtent(extent,endian);
    assert(span>=extent&&span-extent<=3);
    for(uint32_t at=0;at<extent;++at)assert((at^endian)<span);
  }
  assert(legodimensions::gpu_native::TextureEndianExtent(UINT32_MAX,3)==0);
  assert(legodimensions::gpu_native::TextureEndianExtent(UINT32_MAX-3,3)==UINT32_MAX-3);
  // Compare every address-XOR mode against the bytewise oracle, including
  // unaligned offsets and short extents that must reject without overreading.
  for (uint32_t bytes : {1u, 2u, 4u, 8u, 16u})
    for (uint32_t endian = 0; endian < 4; ++endian)
      for (uint32_t start = 0; start < 8; ++start)
        for (uint32_t length = 0; length < 40; ++length) {
          std::vector<uint8_t> source(length), expected(bytes * 3, 0xFE), actual(expected);
          for (uint32_t i = 0; i < length; ++i) source[i] = uint8_t(i * 37 + 11);
          auto offset = [=](uint32_t x, auto, auto) { return int64_t(start + x * bytes); };
          bool valid = true;
          for (uint32_t x = 0; x < 3 && valid; ++x)
            for (uint32_t b = 0; b < bytes; ++b) {
              const size_t at = (size_t(offset(x, 0, 0)) + b) ^ endian;
              if (at >= source.size()) { valid = false; break; }
              expected[x * bytes + b] = source[at];
            }
          const bool copied = CopyTextureVolumeBlocks(source, actual, 3, 1, 1,
                                                       bytes, bytes * 3, endian, offset);
          assert(copied == valid);
          if (valid) assert(actual == expected);
        }
  TestLinearVolume(16, 16);
  TestLinearVolume(64, 32);
  // A callback can address a packed mip / non-linear block order too. The
  // production callback uses the SDK's GetTiledOffset3D, not a 2D slice tiler.
  std::vector<uint8_t> packed(128, 0), output(8);
  auto packed_offset = [](uint32_t x, uint32_t y, uint32_t z) {
    return int64_t(64 + z + 4 * y + 2 * x);
  };
  for (uint32_t z = 0; z < 2; ++z)
    for (uint32_t y = 0; y < 2; ++y)
      for (uint32_t x = 0; x < 2; ++x)
        packed[size_t(packed_offset(x, y, z))] = uint8_t(z * 4 + y * 2 + x);
  assert(CopyTextureVolumeBlocks(packed, output, 2, 2, 2, 1, 2, 0, packed_offset));
  for (uint32_t i = 0; i < 8; ++i) assert(output[i] == i);
  std::cout << "Volume upload regressions passed\n";
}
