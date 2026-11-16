"""Execute the production sampler decoder/cache and batch API against scalar."""
import argparse
import os
from pathlib import Path
import subprocess
import sys
sys.path.insert(0,str(Path(__file__).resolve().parent))
from test_frame_ring_transaction import function

p=argparse.ArgumentParser(description=__doc__);p.add_argument('output',type=Path)
p.add_argument('--compiler',default='clang++');p.add_argument('--emit-only',action='store_true');a=p.parse_args()
root=Path(__file__).resolve().parents[2];out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
s=(root/'rexlego/src/gpu_native/device.cpp').read_text()
types=s[s.index('struct SamplerKeyHash {'):s.index('\nstruct State {')]
types=types[:types.index('struct CachedNativeSampler {')]+function(types,'struct CachedNativeSampler')+';'
bodies='\n'.join(function(s,x) for x in ('u32 RegisterSamplerLocked(', 'u32 HostDevice::RegisterSampler(', 'void HostDevice::RegisterSamplers('))
draw=(root/'rexlego/src/gpu_native/draw.cpp').read_text()
bind=draw[draw.index('bool BindConstants('):draw.index('\nplume::RenderPipeline* GetPipeline(')]
assert 'shared.samplers[i] = HostDevice::RegisterSampler(fetch)' not in bind
assert bind.index('MergeTextureFetch(')<bind.index('sampler_fetches[i] = fetch;')<bind.index('HostDevice::RegisterSamplers(')
cpp=out/'samplers.cpp'
cpp.write_text(r'''
#include "gpu_native/sampler_state.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>
#include <chrono>
#include <atomic>
using namespace legodimensions::gpu_native;
using u32=uint32_t;using u64=uint64_t;
#define CHECK(v) do{if(!(v)){std::fprintf(stderr,"failed %d: %s\n",__LINE__,#v);std::exit(23);}}while(0)
#define REXLOG_INFO(...) ((void)0)
#define REXLOG_ERROR(...) ((void)0)
namespace rex::cvar {std::string GetFlagByName(const char*){return "-1";}}
namespace rex::filesystem {std::filesystem::path GetExecutableFolder(){return {};}}
namespace plume {
struct RenderSampler {RenderSamplerDesc desc;};
struct RenderDevice {bool fail=false;unsigned created=0;
 std::unique_ptr<RenderSampler> createSampler(RenderSamplerDesc desc){if(fail)return {}; ++created;return std::make_unique<RenderSampler>(RenderSampler{desc});}};
struct RenderDescriptorSet {std::vector<NativeSamplerKey> keys;
 void setSampler(u32 slot,RenderSampler* s){CHECK(slot==keys.size()+1);keys.push_back(TextureSamplerKey(s->desc));}};
}
''' + types + r'''
constexpr u32 kBindlessSamplerCount=64;
struct State {
 std::unique_ptr<plume::RenderDescriptorSet> sampler_descriptors=std::make_unique<plume::RenderDescriptorSet>();
 std::unique_ptr<plume::RenderDevice> device=std::make_unique<plume::RenderDevice>();
 std::unordered_map<NativeSamplerKey,CachedNativeSampler,SamplerKeyHash> samplers;
 std::array<TextureFetchWords,32> sampler_last_fetches{};
 std::array<u32,32> sampler_last_descriptors{};
 std::array<int32_t,32> sampler_last_overrides{};
 u32 sampler_error_logs=0;
};
struct CountingMutex {std::mutex mutex;uint64_t locks=0;void lock(){mutex.lock();++locks;}void unlock(){mutex.unlock();}};
CountingMutex g_mutex;std::unique_ptr<State> g_state;
std::atomic<u64> g_state_generation{1};
std::atomic<int32_t> g_anisotropic_override{-1};
struct HostDevice {static u32 RegisterSampler(const TextureFetchWords&);
 static void RegisterSamplers(const std::array<TextureFetchWords,32>&,u32,std::array<u32,32>&);};
''' + bodies + r'''
int main(){
 std::array<TextureFetchWords,32> fetches{};std::array<u32,32> batch;
 for(u32 i=0;i<32;++i){fetches[i]={};fetches[i][0]=(i%8)<<10;
  fetches[i][1]=1<<12;fetches[i][2]=63|(63<<13);fetches[i][3]=((i/8)%2)<<19;fetches[i][5]=(1<<9)|(1<<12);}
 batch.fill(0xDEAD);g_mutex.locks=0;
 HostDevice::RegisterSamplers(fetches,0,batch);CHECK(g_mutex.locks==0);
 HostDevice::RegisterSamplers(fetches,5,batch);CHECK(g_mutex.locks==1&&batch[0]==0&&batch[2]==0&&batch[1]==0xDEAD);
 std::mt19937 random(0x83FB58A8);
 // Each independent state must emit the same immutable keys in slot order.
 std::vector<u32> masks;for(unsigned i=0;i<10000;++i)masks.push_back(random());
 std::vector<std::array<u32,32>> reference;
 g_state=std::make_unique<State>();
 for(u32 mask:masks){std::array<u32,32> result;result.fill(0xDEAD);
  for(u32 i=0;i<32;++i)if(mask&(1u<<i))result[i]=HostDevice::RegisterSampler(fetches[i]);reference.push_back(result);}
 auto keys=g_state->sampler_descriptors->keys;
 g_state=std::make_unique<State>();g_mutex.locks=0;
 for(unsigned i=0;i<masks.size();++i){batch.fill(0xDEAD);HostDevice::RegisterSamplers(fetches,masks[i],batch);CHECK(batch==reference[i]);}
 CHECK(g_mutex.locks==masks.size());CHECK(g_state->sampler_descriptors->keys==keys);
 auto stable=batch;auto created=g_state->device->created;
 HostDevice::RegisterSamplers(fetches,~u32{0},batch);CHECK(g_state->device->created==created);
 g_state=std::make_unique<State>();g_state->device->fail=true;
 HostDevice::RegisterSamplers(fetches,3,batch);CHECK(batch[0]==0&&batch[1]==0&&g_state->samplers.empty());
 g_state->device->fail=false;HostDevice::RegisterSamplers(fetches,3,batch);CHECK(batch[0]&&batch[1]);
 // Exhaustion cannot overwrite or recycle an existing in-flight descriptor.
 for(u32 i=0;i<256;++i){auto fetch=fetches[0];fetch[4]=(i<<12);HostDevice::RegisterSampler(fetch);}
 CHECK(g_state->samplers.size()==kBindlessSamplerCount-1);
 auto untouched=g_state->sampler_descriptors->keys;
 auto fetch=fetches[0];fetch[4]=511u<<12;CHECK(HostDevice::RegisterSampler(fetch)==0);
 CHECK(g_state->sampler_descriptors->keys==untouched);
 g_state=std::make_unique<State>();g_state->sampler_descriptors.reset();
 batch.fill(0xDEAD);HostDevice::RegisterSamplers(fetches,3,batch);CHECK(batch[0]==0&&batch[1]==0&&batch[2]==0xDEAD);
 // CPU lookup microbenchmark only, with the actual decoder/cache body.
 g_state=std::make_unique<State>();HostDevice::RegisterSamplers(fetches,~u32{0},batch);
 for(unsigned slots:{4u,8u,32u})for(unsigned trial=0;trial<6;++trial)for(bool batched:{bool(trial%2),!bool(trial%2)}){
  g_mutex.locks=0;auto start=std::chrono::steady_clock::now();
  for(unsigned draw=0;draw<100000;++draw){
   if(batched)HostDevice::RegisterSamplers(fetches,slots==32?~u32{0}:((1u<<slots)-1),batch);
   else for(u32 i=0;i<slots;++i)batch[i]=HostDevice::RegisterSampler(fetches[i]);
  }
  double ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
  std::printf("sampler cpu slots=%u trial=%u batch=%d elapsed_ms=%.3f locks=%llu descriptors=%u/%u\n",slots,trial,batched,ms,
     (unsigned long long)g_mutex.locks,batch[0],batch[31]);
 }
 std::puts("PASS 10000 differential masks, immutable keys, creation retry, exhaustion, null device, unused slots and one lock per draw");
}
''')
if a.emit_only: sys.exit(0)
exe=out/'samplers.exe'
subprocess.run([a.compiler,'-std=c++20','-O2','-DNOMINMAX','-D_CRT_SECURE_NO_WARNINGS','-I'+str(root/'rexlego/src'),'-I'+str(root/'thirdparty/plume'),str(cpp),'-o',str(exe)],check=True,timeout=45)
# This fixture checks batching against scalar; thread-cache behavior is
# independently exercised with setting changes by test_sampler_live_settings.
env=dict(os.environ,LEGO_NATIVE_SAMPLER_THREAD_CACHE='0')
subprocess.run([str(exe)],check=True,timeout=20,env=env)
