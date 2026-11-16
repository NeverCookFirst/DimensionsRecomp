#pragma once
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <span>

namespace legodimensions::gpu_native {
enum class MultiRateFetchKind { kUnknown, kModulo4, kGroup4, kGroup128 };

struct MultiRateFetchSemantic {
  const char* name;
  uint32_t index;
  MultiRateFetchKind kind;
};
inline constexpr std::array<MultiRateFetchSemantic,11> kMultiRateFetchSemantics{{
  {"TANGENT",0,MultiRateFetchKind::kModulo4},
  {"TEXCOORD",3,MultiRateFetchKind::kGroup4},
  {"TANGENT",1,MultiRateFetchKind::kGroup4},
  {"POSITION",0,MultiRateFetchKind::kGroup128},
  {"NORMAL",0,MultiRateFetchKind::kGroup128},
  {"TEXCOORD",0,MultiRateFetchKind::kGroup128},
  {"TEXCOORD",2,MultiRateFetchKind::kGroup128},
  {"TEXCOORD",1,MultiRateFetchKind::kGroup128},
  {"BLENDWEIGHT",0,MultiRateFetchKind::kGroup128},
  {"TEXCOORD",6,MultiRateFetchKind::kGroup128},
  {"TEXCOORD",7,MultiRateFetchKind::kGroup128}
}};

inline uint32_t MultiRateFetchOrdinal(const char* semantic, uint32_t index) {
  for(uint32_t i=0;i<kMultiRateFetchSemantics.size();++i)
    if(index==kMultiRateFetchSemantics[i].index &&
        !std::strcmp(semantic,kMultiRateFetchSemantics[i].name)) return i;
  return uint32_t(kMultiRateFetchSemantics.size());
}

// F77D99A4104EA49B reflection and original fetch operands. Other programs
// require their own proof; this mapping is not a general semantic convention.
inline MultiRateFetchKind MultiRateFetchInput(const char* semantic, uint32_t index) {
  const auto i=MultiRateFetchOrdinal(semantic,index);
  return i<kMultiRateFetchSemantics.size()?kMultiRateFetchSemantics[i].kind:MultiRateFetchKind::kUnknown;
}

// Preserve the original FP32 arithmetic and fetch's floor conversion. Near
// the high end of the 24-bit index domain, the biases may round differently
// from integer division. Reject an unrepresentable/negative result rather
// than silently substituting a different source record.
inline bool MultiRateFetchIndex(MultiRateFetchKind kind, uint32_t vertex, uint32_t& result) {
  if (kind==MultiRateFetchKind::kUnknown || vertex>0xFFFFFFu) return false;
  const float v=float(vertex);
  const float group4=v*0.25f+0.125f;
  float index=0;
  switch(kind) {
    case MultiRateFetchKind::kModulo4: index=v-std::trunc(group4)*4.0f;break;
    case MultiRateFetchKind::kGroup4: index=group4;break;
    case MultiRateFetchKind::kGroup128: index=v*0.0078125f+0.00390625f;break;
    default:return false;
  }
  index=std::floor(index);
  if (!(index>=0.0f && index<4294967296.0f)) return false;
  result=uint32_t(index);
  return true;
}

struct MultiRateFetchChannel {
  uint32_t source_slot=0,source_offset=0,bytes=0;
  MultiRateFetchKind kind=MultiRateFetchKind::kUnknown;
};

// Give each fetched attribute its own host slot. The guest may interleave or
// overlap attributes in one stream while fetching them at different indices;
// gathering whole records or assigning one rate per stream cannot express it.
// Slot 15 remains reserved for declaration inputs this program never reads.
template<class Element,class ByteSize>
bool BuildMultiRateFetchLayout(std::span<const Element> input,std::span<Element> remapped,
    std::span<MultiRateFetchChannel> channels,uint32_t& channel_count,ByteSize byte_size) {
  if(input.size()>64 || remapped.size()<input.size() || channels.size()<11) return false;
  uint32_t seen=0,count=0;
  std::array<MultiRateFetchChannel,11> prepared{};
  for(const auto& element:input) {
    const auto ordinal=MultiRateFetchOrdinal(element.semanticName,element.semanticIndex);
    if(ordinal==kMultiRateFetchSemantics.size()) continue;
    const auto bytes=uint32_t(byte_size(element.format));
    if(element.slotIndex>=16 || !bytes || bytes>16 || (seen&(1u<<ordinal))) return false;
    seen|=1u<<ordinal;
    prepared[count++]={element.slotIndex,element.alignedByteOffset,bytes,kMultiRateFetchSemantics[ordinal].kind};
  }
  if(seen!=((1u<<11)-1)) return false;
  count=0;
  for(size_t i=0;i<input.size();++i) {
    remapped[i]=input[i];
    const auto ordinal=MultiRateFetchOrdinal(input[i].semanticName,input[i].semanticIndex);
    remapped[i].slotIndex=ordinal<kMultiRateFetchSemantics.size()?count++:15;
    remapped[i].alignedByteOffset=0;
  }
  for(uint32_t i=0;i<count;++i)channels[i]=prepared[i];
  channel_count=count;
  return true;
}

// Source is the ordinary native buffer upload after its existing endian and
// declaration-field conversions. Copy only this attribute, never a guessed
// record layout. Validate the full gather before changing any output byte.
inline bool GatherMultiRateFetchChannel(std::span<const uint8_t> source,uint32_t stride,
    uint32_t stream_offset,const MultiRateFetchChannel& channel,std::span<const uint32_t> vertices,
    std::span<uint8_t> output,uint32_t output_stride,bool synthetic_zero=false) {
  if(!stride || !channel.bytes || channel.bytes>16 || output_stride<channel.bytes ||
      channel.source_offset>stride || channel.bytes>stride-channel.source_offset ||
      vertices.size()>output.size()/output_stride) return false;
  if(channel.kind==MultiRateFetchKind::kUnknown) return false;
  const auto source_address=reinterpret_cast<uintptr_t>(source.data());
  const auto output_address=reinterpret_cast<uintptr_t>(output.data());
  const uint64_t output_bytes=uint64_t(vertices.size())*output_stride;
  if(!synthetic_zero && !source.empty() && output_bytes &&
      (source_address<=output_address ? output_address-source_address<source.size()
                                     : source_address-output_address<output_bytes)) return false;
  for(const auto vertex:vertices) {
    uint32_t index=0;
    if(!MultiRateFetchIndex(channel.kind,vertex,index)) return false;
    const uint64_t offset=uint64_t(stream_offset)+uint64_t(index)*stride+channel.source_offset;
    if(!synthetic_zero && offset+channel.bytes>source.size()) return false;
  }
  for(size_t i=0;i<vertices.size();++i) {
    auto* out=output.data()+uint64_t(i)*output_stride;
    std::memset(out,0,output_stride);
    if(!synthetic_zero) {
      uint32_t index=0;MultiRateFetchIndex(channel.kind,vertices[i],index);
      const uint64_t offset=uint64_t(stream_offset)+uint64_t(index)*stride+channel.source_offset;
      std::memcpy(out,source.data()+offset,channel.bytes);
    }
  }
  return true;
}
} // namespace legodimensions::gpu_native
