#include "gpu_native/shader_input_usage.h"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <vector>
using namespace legodimensions::gpu_native;
#define CHECK(x) do{if(!(x)){std::fprintf(stderr,"failed line %d: %s\n",__LINE__,#x);std::exit(23);}}while(0)
int main(){
  // Small real-format ISG1 container: POSITION0 is read, TEXCOORD0 is unused.
  std::vector<uint8_t> bytes(160);
  const auto word=[&](size_t at,uint32_t value){std::memcpy(bytes.data()+at,&value,4);};
  std::memcpy(bytes.data(),"DXBC",4);word(28,1);word(32,36);
  std::memcpy(bytes.data()+36,"ISG1",4);word(40,116);word(44,2);
  word(56,72);bytes[77]=15;word(88,82);bytes[109]=0;
  std::memcpy(bytes.data()+116,"POSITION",9);std::memcpy(bytes.data()+126,"TEXCOORD",9);
  CHECK(ShaderReadsVertexInput(bytes,"POSITION",0)==true);
  CHECK(ShaderReadsVertexInput(bytes,"TEXCOORD",0)==false);
  CHECK(ShaderReadsVertexInput(bytes,"COLOR",0)==false);
  VertexInputUsageCache cache(2);unsigned scans=0;
  auto compute=[&]() -> std::optional<uint64_t> {
    ++scans;uint64_t mask=0;unsigned i=0;
    for(auto name:{"POSITION","TEXCOORD","COLOR"}){
      const auto reads=ShaderReadsVertexInput(bytes,name,0);
      if(!reads || *reads)mask|=uint64_t{1}<<i;
      ++i;
    }return mask;
  };
  const VertexInputUsageCache::Key key{11,22,0,3};
  CHECK(cache.Get(key,compute)==1);
  for(unsigned i=0;i<256;++i)CHECK(cache.Get(key,compute)==1);
  CHECK(scans==1);
  // A different specialization/declaration must not borrow another's mask.
  CHECK(cache.Get({11,22,1,3},[] {return std::optional<uint64_t>(7);})==7);
  CHECK(cache.Get({11,23,0,3},[] {return std::optional<uint64_t>(3);})==3);
  CHECK(cache.Get({11,23,0,3},[] {return std::optional<uint64_t>(5);})==5); // bounded cache was full
  cache.Clear();CHECK(cache.Get(key,compute)==1);CHECK(scans==2);
  // A shader absent before an additive pack is published must be retried.
  cache.Clear();unsigned attempts=0;
  auto publish=[&]() -> std::optional<uint64_t>{return ++attempts==1?std::nullopt:std::optional<uint64_t>(4);};
  CHECK(!cache.Get(key,publish));CHECK(cache.Get(key,publish)==4);CHECK(attempts==2);
  CHECK(cache.Get(key,publish)==4);CHECK(attempts==2);
  // Unidentified declarations bypass retention. More than 64 elements may
  // still use the original parser; they never cause a truncated mask lookup.
  CHECK(cache.Get({11,0,0,3},[] {return std::optional<uint64_t>(1);})==1);
  CHECK(cache.Get({11,0,0,3},[] {return std::optional<uint64_t>(2);})==2);
  CHECK(cache.Get({11,22,0,65},[] {return std::optional<uint64_t>(1);})==1);
  CHECK(cache.Get({11,22,0,65},[] {return std::optional<uint64_t>(2);})==2);
  std::puts("PASS real signature masks, immutable specialization/declaration keys, bounded cache, late publication and reset");
}
