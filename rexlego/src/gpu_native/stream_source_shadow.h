#pragma once
#include <cstdint>
#include <cstddef>
#include <span>

namespace legodimensions::gpu_native {
// CPU fields written by TU23 83FBA160 and read by 83FBA280. The getter is
// used to save/restore stream 0 around a dynamic vertex-buffer update.
inline bool WriteStreamSourceShadow(std::span<uint8_t> device, uint32_t stream,
    uint32_t buffer, uint32_t fetch_lo, uint32_t fetch_hi, uint32_t offset,
    uint32_t stride, uint64_t dirty) {
  if (device.size() < 12904 + 18 || stream >= 18) return false;
  const auto read = [&](std::size_t at, uint32_t bytes) {
    uint64_t value=0; for(uint32_t i=0;i<bytes;++i)value=(value<<8)|device[at+i];return value;
  };
  const auto write = [&](std::size_t at,uint64_t value,uint32_t bytes) {
    for(uint32_t i=0;i<bytes;++i)device[at+bytes-1-i]=uint8_t(value>>(8*i));
  };
  if (buffer) {
    const uint32_t address = fetch_lo + offset;
    const uint32_t packed = (address & 0x1FFFFFFFu) + (((address >> 20) + 512u) & 0x1000u);
    const uint32_t fetch = (239u-stream)*8u;
    write(fetch,packed,4); write(fetch+4,fetch_hi-offset,4);
    write(24,read(24,8)|dirty,8);
  }
  write(12836+4*stream,buffer,4);
  device[12904+stream]=uint8_t(stride>>2);
  if ((stride>>2) && (stride>>2)!=device[12264+stream])
    write(16,read(16,8)|0x80000u,8);
  return true;
}
} // namespace legodimensions::gpu_native
