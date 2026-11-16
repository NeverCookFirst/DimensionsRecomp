#pragma once
// Private bounded, read-only check of TU23 region tags and free lists.
// Call only while the original guest heap lock is held. No repair of metadata.
#include <array>
#include <bit>
#include <cstdint>
#include <span>
#include <unordered_map>
#include <unordered_set>
namespace native_load_heap {
struct Region {uint32_t address{},bytes{};bool external{};};
enum class ShapeError {kNone,kRead,kLimits,kRegion,kLinks,kBlock,kFooter,kOwner,kBin,kMissing,kCounts,kBitmap};
struct ShapeResult {ShapeError error{};uint32_t regions{},blocks{},free_blocks{};explicit operator bool()const{return error==ShapeError::kNone;}};
inline uint32_t BinOffset(uint32_t bytes){
  if(bytes<1024)return 0x11C+(bytes/4)*12;
  const auto exponent=std::bit_width(bytes)-11;
  const auto index=exponent*32+((bytes-(1024u<<exponent))>>(exponent+5));
  return 3536+index*12;
}
template<class Read>
ShapeResult ValidateShape(uint32_t heap,std::span<const Region> known,Read read){
  ShapeResult result{};
  auto fail=[&](ShapeError e){result.error=e;return result;};
  auto word=[&](uint32_t a,uint32_t&v){std::array<uint8_t,4>b{};if(!read(a,b.data(),4))return false;v=uint32_t(b[0])<<24|uint32_t(b[1])<<16|uint32_t(b[2])<<8|b[3];return true;};
  if(!heap||(heap&3)||known.size()>64||uint64_t(heap)+0x3CE0>0x100000000ull)return fail(ShapeError::kLimits);
  uint32_t header_bytes{},id{},node{};
  if(!word(0x848D6614,header_bytes)||!word(heap+12,id)||!word(heap+0x98,node))return fail(ShapeError::kRead);
  if(header_bytes!=4)return fail(ShapeError::kLimits); // Debug allocator layouts require their own proof.
  std::unordered_map<uint32_t,const Region*> allowed;
  for(const auto&r:known){
    if(!r.address||(r.address&3)||r.bytes<64||uint64_t(r.address)+r.bytes>0x100000000ull||!allowed.emplace(r.address,&r).second)return fail(ShapeError::kRegion);
  }
  std::unordered_set<uint32_t> seen;
  std::unordered_map<uint32_t,uint32_t> free_bins;
  std::array<uint32_t,22> large_counts{},large_masks{};
  uint32_t large_top=0;uint64_t free_bytes=0;
  uint32_t previous=0;
  while(node){
    if(result.regions>=known.size()||!seen.insert(node).second)return fail(ShapeError::kLinks);
    const auto it=allowed.find(node);if(it==allowed.end())return fail(ShapeError::kRegion);
    const auto&r=*it->second;std::array<uint32_t,7>w{};
    for(unsigned i=0;i<w.size();++i)if(!word(node+i*4,w[i]))return fail(ShapeError::kRead);
    if(w[0]!=r.address||w[1]!=r.bytes||w[2]!=r.address+32||w[3]!=((r.address+r.bytes)&~3u)-4||w[5]!=previous||((w[6]>>24)!=uint32_t(r.external)))return fail(ShapeError::kRegion);
    uint32_t leading{},trailing{};
    if(!word(node+28,leading)||!word(w[3],trailing))return fail(ShapeError::kRead);
    if(leading!=0x08000000||(trailing&0x7FFFFFFF)!=0x08000000)return fail(ShapeError::kBlock);
    uint32_t at=w[2];
    while(at!=w[3]){
      if(++result.blocks>262144)return fail(ShapeError::kLimits);
      uint32_t tag{},footer{};if(!word(at,tag))return fail(ShapeError::kRead);
      const uint32_t bytes=(tag&0x7FFFFFF)*4;
      if(bytes<12||bytes>w[3]-at)return fail(ShapeError::kBlock);
      if(!word(at+bytes-4,footer))return fail(ShapeError::kRead);
      if((footer&0x7FFFFFF)!=(tag&0x7FFFFFF))return fail(ShapeError::kFooter);
      if(tag&0x78000000){
        const uint32_t owner=footer>>27;
        if(id<30){if(owner!=id+1)return fail(ShapeError::kOwner);}
        else{uint32_t full{};if(owner!=31||bytes<16||!word(at+bytes-8,full)||full!=id)return fail(ShapeError::kOwner);}
      }else{
        if(footer>>27)return fail(ShapeError::kFooter);
        free_bins.emplace(at,heap+BinOffset(bytes));++result.free_blocks;
        free_bytes+=bytes;
        if(bytes>=1024){const auto exponent=std::bit_width(bytes)-11;
          const auto index=(bytes-(1024u<<exponent))>>(exponent+5);
          if(exponent>=22||index>=32)return fail(ShapeError::kLimits);
          ++large_counts[exponent];large_masks[exponent]|=1u<<(31-index);large_top|=1u<<(31-exponent);}
      }
      at+=bytes;
    }
    previous=node;node=w[4];++result.regions;
  }
  if(result.regions!=known.size())return fail(ShapeError::kMissing);
  // Every free block must appear once in its size class, with exact backlinks.
  // Never follow an unrecognized pointer merely because it is readable.
  std::unordered_set<uint32_t> listed;
  auto check_bin=[&](uint32_t sentinel)->bool{
    uint32_t current{},prev=sentinel,previous_size=0;
    if(!word(sentinel+4,current))return false;
    while(current){
      const auto it=free_bins.find(current);
      if(it==free_bins.end()||it->second!=sentinel||!listed.insert(current).second)return false;
      uint32_t next{},back{},tag{};
      if(!word(current,tag)||!word(current+4,next)||!word(current+8,back)||back!=prev)return false;
      const uint32_t size=(tag&0x7FFFFFF)*4;
      if(sentinel>=heap+3536&&size<previous_size)return false;
      previous_size=size;
      prev=current;current=next;
    }
    return true;
  };
  for(unsigned i=0;i<256;++i)if(!check_bin(heap+0x11C+i*12))return fail(ShapeError::kBin);
  for(unsigned i=0;i<22*32;++i)if(!check_bin(heap+3536+i*12))return fail(ShapeError::kBin);
  if(listed.size()!=free_bins.size())return fail(ShapeError::kMissing);
  uint32_t counter{},mask{};
  if(!word(heap+0x2ED8,counter)||counter!=free_bytes||!word(heap+0x2EE8,counter)||counter!=result.free_blocks||
      !word(heap+0x2EF0,counter)||counter!=result.blocks-result.free_blocks)return fail(ShapeError::kCounts);
  if(!word(heap+0xD74,mask)||mask!=large_top)return fail(ShapeError::kBitmap);
  for(unsigned i=0;i<22;++i){
    if(!word(heap+0xD1C+i*4,counter)||counter!=large_counts[i])return fail(ShapeError::kCounts);
    if(!word(heap+0xD78+i*4,mask)||mask!=large_masks[i])return fail(ShapeError::kBitmap);
  }
  return result;
}
} // namespace native_load_heap
