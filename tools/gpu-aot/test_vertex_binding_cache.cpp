#include "gpu_native/vertex_binding_cache.h"
#include <cstdio>
#include <cstdlib>
#include <utility>
#include <vector>
using namespace legodimensions::gpu_native;
#define CHECK(x) do{if(!(x)){std::fprintf(stderr,"failed line %d: %s\n",__LINE__,#x);std::exit(23);}}while(0)
int main() {
  VertexBindingCache cache;
  std::array<plume::RenderVertexBufferView,16> views{};
  std::array<plume::RenderInputSlot,16> slots{};
  for(unsigned i=0;i<16;++i)slots[i]=plume::RenderInputSlot(i,16);
  int commands=0,other_commands=0;
  std::vector<std::pair<unsigned,unsigned>> emitted;
  auto bind=[&](const void* command,unsigned serial){
    emitted.clear();cache.Bind(command,serial,views,slots,[&](unsigned first,unsigned count){emitted.emplace_back(first,count);});
  };
  bind(&commands,1);CHECK(emitted.size()==1 && emitted[0]==std::make_pair(0u,16u));
  bind(&commands,1);CHECK(emitted.empty());
  views[0].size=64;bind(&commands,1);CHECK(emitted.size()==1 && emitted[0]==std::make_pair(0u,1u));
  views[0].buffer.offset=16;views[2].size=32;bind(&commands,1);
  CHECK(emitted.size()==2 && emitted[0]==std::make_pair(0u,1u) && emitted[1]==std::make_pair(2u,1u));
  views[1].buffer.ref=reinterpret_cast<const plume::RenderBuffer*>(uintptr_t(16));
  views[2].size=48;bind(&commands,1);CHECK(emitted.size()==1 && emitted[0]==std::make_pair(1u,2u));
  slots[15].stride=32;bind(&commands,1);CHECK(emitted.size()==1 && emitted[0]==std::make_pair(15u,1u));
  bind(&commands,2);CHECK(emitted.size()==1 && emitted[0]==std::make_pair(0u,16u));
  bind(&other_commands,2);CHECK(emitted.size()==1 && emitted[0]==std::make_pair(0u,16u));
  bind(&other_commands,2);CHECK(emitted.empty());
  slots[1].index=0;bind(&other_commands,2);CHECK(emitted.size()==1 && emitted[0]==std::make_pair(0u,16u));
  slots[1].index=1;bind(&other_commands,2);CHECK(emitted.size()==1 && emitted[0]==std::make_pair(0u,16u));
  bind(&other_commands,0);CHECK(emitted.size()==1 && emitted[0]==std::make_pair(0u,16u));
  cache.Reset();bind(&other_commands,2);CHECK(emitted.size()==1 && emitted[0]==std::make_pair(0u,16u));
  std::puts("PASS changed IA ranges, buffer identity/offset/size/stride, new recording and unfamiliar layout");
}
