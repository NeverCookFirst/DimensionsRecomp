#pragma once
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string_view>
#include <unordered_map>

namespace legodimensions::gpu_native {
// DXBC input signatures survive DXIL prelinking. Ignore declaration elements
// that this specialization never reads; stale unused streams are legal.
inline std::optional<bool> ShaderReadsVertexInput(std::span<const uint8_t> bytes,
    std::string_view semantic, uint32_t semantic_index) {
  const auto word = [](const uint8_t* p) { uint32_t v; std::memcpy(&v,p,4); return v; };
  if (bytes.size() < 32 || std::memcmp(bytes.data(),"DXBC",4)) return {};
  const uint32_t count = word(bytes.data()+28);
  if (count > (bytes.size()-32)/4) return {};
  for (uint32_t i=0; i<count; ++i) {
    const uint32_t offset=word(bytes.data()+32+4*i);
    if (offset > bytes.size()-8) return {};
    const bool extended=!std::memcmp(bytes.data()+offset,"ISG1",4);
    if (!extended && std::memcmp(bytes.data()+offset,"ISGN",4)) continue;
    const uint32_t length=word(bytes.data()+offset+4);
    if (length<8 || length>bytes.size()-offset-8) return {};
    const auto* data=bytes.data()+offset+8;
    const uint32_t records=word(data), stride=extended?32:24;
    if (records>(length-8)/stride) return {};
    bool used=false;
    for (uint32_t r=0; r<records; ++r) {
      const auto* entry=data+8+r*stride+(extended?4:0);
      const uint32_t name_offset=word(entry);
      if (name_offset>=length) return {};
      const char* name=reinterpret_cast<const char*>(data+name_offset);
      const void* end=std::memchr(name,0,length-name_offset);
      if (!end) return {};
      if (semantic==std::string_view(name,static_cast<const char*>(end)-name) &&
          word(entry+4)==semantic_index && entry[21]) used=true;
    }
    return used;
  }
  return {};
}
// Shader archives and declaration contents are immutable after publication.
// Buffer lengths/contents are deliberately absent: callers still validate the
// actual resource on every draw. Missing bytecode must never be cached, since
// an additive development pack may publish it later in the same process.
class VertexInputUsageCache {
 public:
  struct Key {
    uint64_t shader=0,declaration=0;
    uint32_t specialization=0,elements=0;
    bool operator==(const Key&) const=default;
  };
  explicit VertexInputUsageCache(size_t limit=4096):limit_(limit){}
  template<class Compute>
  std::optional<uint64_t> Get(Key key,Compute&& compute) {
    const bool cacheable=key.shader && key.declaration && key.elements<=64;
    if(cacheable)if(auto it=entries_.find(key);it!=entries_.end())return it->second;
    const auto result=compute();
    if(cacheable && result && entries_.size()<limit_)entries_.emplace(key,*result);
    return result;
  }
  void Clear(){entries_.clear();}
 private:
  struct Hash {
    size_t operator()(const Key& key)const {
      const uint64_t rotated=(key.declaration<<17)|(key.declaration>>47);
      return size_t(key.shader^rotated^(uint64_t(key.specialization)<<32)^key.elements);
    }
  };
  size_t limit_;
  std::unordered_map<Key,uint64_t,Hash> entries_;
};
} // namespace legodimensions::gpu_native
