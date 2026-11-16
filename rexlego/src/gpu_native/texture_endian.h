#pragma once
#include <cstdint>

namespace legodimensions::gpu_native {
constexpr uint32_t TextureEndianXor(uint32_t mode) {
  return mode==1?1:mode==2?3:mode==3?2:0;
}
// Endian XOR can address the other half/byte of the last logical word.
// Include that word in residency checks, hashes, snapshots and invalidation.
// Encoders still change only bytes selected by covered texels.
constexpr uint32_t TextureEndianExtent(uint32_t extent,uint32_t endian_xor) {
  const uint32_t mask=endian_xor>=2?3:endian_xor;
  if(endian_xor>3 || extent>UINT32_MAX-mask) return 0;
  return (extent+mask)&~mask;
}
}  // namespace legodimensions::gpu_native
