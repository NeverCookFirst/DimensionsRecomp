#pragma once
#include "gpu_native/multi_rate_fetch.h"

namespace legodimensions::gpu_native {
// Per-attribute variants of the current upload's existing conversion rules.
// Different attributes may overlap a guest stream while requiring different
// conversions; applying the union of their fix-ups to one copy is ambiguous.
enum class MultiRateFieldOrder { kDwordOnly, kReverse4, kHalfSwap4, kHalfSwap8 };

// Gather directly from the captured CPU mirror, before GPU-only promotion.
// Each byte reproduces common DWORD conversion followed by this attribute's
// existing field fix-up. No source mutation or resident-buffer CPU map occurs.
inline bool GatherMultiRateGuestChannel(std::span<const uint8_t> source,
    uint32_t stride, uint32_t stream_offset, const MultiRateFetchChannel& channel,
    MultiRateFieldOrder order, std::span<const uint32_t> vertices,
    std::span<uint8_t> output, uint32_t output_stride, bool synthetic_zero=false) {
  const uint32_t converted = order==MultiRateFieldOrder::kDwordOnly ? 0 :
      order==MultiRateFieldOrder::kReverse4 || order==MultiRateFieldOrder::kHalfSwap4 ? 4 :
      order==MultiRateFieldOrder::kHalfSwap8 ? 8 : UINT32_MAX;
  if (!stride || !channel.bytes || channel.bytes>16 || converted>channel.bytes ||
      channel.source_offset>stride || channel.bytes>stride-channel.source_offset ||
      output_stride<channel.bytes || vertices.size()>output.size()/output_stride ||
      channel.kind==MultiRateFetchKind::kUnknown) return false;
  const auto in=reinterpret_cast<uintptr_t>(source.data());
  const auto out=reinterpret_cast<uintptr_t>(output.data());
  const uint64_t output_bytes=uint64_t(vertices.size())*output_stride;
  if (!synthetic_zero && !source.empty() && output_bytes &&
      (in<=out ? out-in<source.size() : in-out<output_bytes)) return false;
  for (const auto vertex:vertices) {
    uint32_t index=0;
    if (!MultiRateFetchIndex(channel.kind,vertex,index)) return false;
    const uint64_t offset=uint64_t(stream_offset)+uint64_t(index)*stride+channel.source_offset;
    if (!synthetic_zero && offset+channel.bytes>source.size()) return false;
  }
  for (size_t i=0;i<vertices.size();++i) {
    auto* destination=output.data()+uint64_t(i)*output_stride;
    std::memset(destination,0,output_stride);
    if (synthetic_zero) continue;
    uint32_t index=0;MultiRateFetchIndex(channel.kind,vertices[i],index);
    const uint64_t field=uint64_t(stream_offset)+uint64_t(index)*stride+channel.source_offset;
    for (uint32_t byte=0;byte<channel.bytes;++byte) {
      const uint32_t local=byte>=converted ? byte :
          order==MultiRateFieldOrder::kReverse4 ? 3-byte : byte^2u;
      uint64_t address=field+local;
      // ByteSwapElements leaves an incomplete final DWORD unchanged.
      if ((address&~uint64_t{3})+4<=source.size()) address^=3;
      destination[byte]=source[address];
    }
  }
  return true;
}
} // namespace legodimensions::gpu_native
