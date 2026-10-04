#include "gpu_native/portrait_probe.h"
#include <cassert>
#include <iostream>
#include <unordered_map>
#include <vector>
using namespace legodimensions::gpu_native;
int main() {
  std::unordered_map<uint32_t,uint32_t> words{
      {0x1000+84,0x2000},{0x1000+200,0x3000},
      {0x3000+948,0x12345678},{0x3000+952,0x87654321},
      {0x4000+4,3}, // Captured Sonic object reference count, never a pointer.
      {0x3000+956,0x4000},{0x4000+44,0x5000},
      {0x5000+116,1},{0x5000+120+52,0x10001},
      {0x6000+116,0},{0x6000+120,0x10001}};
  const auto original=words;
  std::vector<uint32_t> reads;
  auto read=[&](uint32_t address,uint32_t& out) {
    reads.push_back(address);
    auto it=words.find(address);if(it==words.end())return false;
    out=it->second;return true;
  };
  auto identity=ReadPortraitIdentity(0x1000,read);
  assert(identity.readable_fields==255 && identity.texture==0x5000+120+52);
  assert(identity.scene==0x2000 && identity.material==0x3000 &&
         identity.texture_object==0x4000 && identity.material_flags==0x12345678);
  assert(words==original);
  for(auto address:reads) assert(address!=0x4000+4 && address!=3+116);
  words[0x4000+44]=0x6000;
  assert(ReadPortraitIdentity(0x1000,read).texture==0x6000+120);
  words[0x6000+116]=0xFFFFFFFF;
  assert(ReadPortraitIdentity(0x1000,read).texture==0);
  words[0x6000+116]=0;
  words.erase(0x4000+44);
  assert(!(ReadPortraitIdentity(0x1000,read).readable_fields&32));
  words[0x3000+956]=0;
  assert(ReadPortraitIdentity(0x1000,read).texture==0);
  words[0x1000+200]=0;
  reads.clear();ReadPortraitIdentity(0x1000,read);assert(reads.size()==2);
  reads.clear();ReadPortraitIdentity(0,read);assert(reads.empty());
  reads.clear();ReadPortraitIdentity(0xFFFFFFF0,read);assert(reads.empty());
  words[0x1000+200]=0xFFFFFFF0;
  reads.clear();ReadPortraitIdentity(0x1000,read);assert(reads.size()==2);
  std::cout<<"PASS: actual portrait identity, relocation, unreadable/null objects, 32-bit bounds, read-only fields\n";
}
