"""Execute production host-source materialization and its authority transition."""
import argparse
import os
from pathlib import Path
import subprocess
def body(text, signature):
    start=text.index(signature);end=text.index('{',start)+1;depth=1
    while depth:
        depth+=(text[end]=='{')-(text[end]=='}');end+=1
    return text[start:end]

p=argparse.ArgumentParser(description=__doc__);p.add_argument('output',type=Path)
p.add_argument('--compiler',default='clang++');a=p.parse_args()
root=Path(__file__).resolve().parents[2];a.output.mkdir(parents=True,exist_ok=True)
source=(root/'rexlego/src/gpu_native/textures.cpp').read_text()
production='\n'.join(body(source, signature) for signature in (
    'bool RejectTextureWriteback(',
    'bool PoolCopyHasHostTextureSource(',
    'bool TextureDataCoveredByPoolCopy(',
    'TextureReadbackFormat ResolvedTextureReadbackFormat(',
    'bool PrepareResolvedTextureWriteback(', 'bool EncodeResolvedTextureWriteback(',
    'void PublishResolvedTextureWriteback(', 'bool MaterializeResolvedTextures(',
    'bool MaterializeResolvedTexture(const std::shared_ptr<TextureResource>& resource) {',
    'bool MaterializePoolCopyTextureDependencies(',
    'bool MaterializePoolCopyTextureSource(',
    'bool MaterializePoolCopyTextureDestination('))
production=body(source,'enum class TextureWritebackRejection {')+';\n'+body(source,'struct ResolvedTextureWriteback {')+';\n'+production
fixture=r'''
#include "gpu_native/texture_writeback.h"
#include "gpu_native/pool_copy.h"
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string_view>
#include <string>
#include <unordered_map>
#include <vector>
#include <cstdio>
#include <cstdlib>
using u8=uint8_t;using u32=uint32_t;using u64=uint64_t;
using CpuMemoryStamp=u32;
struct CpuMemorySpan {u32 address,length;};
bool CpuMemoryWatchEnabled(){return false;}
CpuMemoryStamp WatchCpuMemory(std::span<const CpuMemorySpan>){return 0;}
int LockPhysicalWriteback(){return 0;}
using namespace legodimensions::gpu_native;
#define REXLOG_INFO(...) ((void)0)
std::vector<std::string> rejection_messages;
template<class... Args>void RecordWarning(const char* reason,Args&&...){rejection_messages.emplace_back(reason);}
template<class... Args>void RecordWarning(u32,Args&&...){}
#define REXLOG_WARN(format,...) RecordWarning(__VA_ARGS__)
#define CHECK(x) do {if(!(x)){fprintf(stderr,"failed %s\n",#x);exit(23);}}while(0)
namespace rex::graphics::xenos {enum class DataDimension{k2DOrStacked=1,k3D=2};enum class TextureFormat{k_8_8_8_8=6,k_16_16=25,k_16_16_16_16=26,k_16_FLOAT=30,k_16_16_FLOAT=31,k_16_16_16_16_FLOAT=32,k_32_FLOAT=36,k_32_32_FLOAT=37,k_32_32_32_32_FLOAT=38};}
struct Fetch {rex::graphics::xenos::DataDimension dimension=rex::graphics::xenos::DataDimension::k2DOrStacked;
 rex::graphics::xenos::TextureFormat format=rex::graphics::xenos::TextureFormat::k_8_8_8_8;
 u32 base_address=1,mip_address=0,pitch=1,tiled=0,stacked=0,packed_mips=0,endianness=2,swizzle=0xa0a;
 u32 num_format=0,exp_adjust=0,sign_x=0,sign_y=0,sign_z=0,sign_w=0;};
namespace plume {enum class RenderFormat{R8G8B8A8_UNORM,R16G16_UNORM,R16G16B16A16_UNORM,R16_FLOAT,R16G16_FLOAT,R16G16B16A16_FLOAT,R32_FLOAT,R32G32_FLOAT,R32G32B32A32_FLOAT,Other};struct RenderTexture{};}
namespace rex::graphics::texture_util {
 struct Layout {struct {u32 level_data_extent_bytes=64,row_pitch_bytes=16;}base;};
 Layout GetGuestTextureLayout(auto...){return {};}
 void GetPackedMipOffset(u32,u32,u32,rex::graphics::xenos::TextureFormat,u32,u32&x,u32&y,u32&z){x=y=z=0;}
 int64_t GetTiledOffset2D(u32 x,u32 y,u32 pitch,u32){return y*pitch*4+x*4;}
}
struct TextureResource {
 TextureWritebackCoverage writeback_coverage;
 TextureResource(){writeback_coverage.Record(4,4,0,0,4,4);}
 std::mutex mutex;bool resolved_on_host=true,owns_guest_memory=false,surface=false,guest_backing_current=false;
 u32 guest_address=9,width=4,height=4,host_width=4,host_height=4,levels=1,depth=1;
 u32 depth_alias_generation=0,depth_resolve_generation=8,cpu_stamp=7;
 bool sampled_valid=true,guest_uploaded=true,upload_source_key_valid=true;
 plume::RenderFormat format=plume::RenderFormat::R8G8B8A8_UNORM;
 Fetch guest_fetch;std::unique_ptr<plume::RenderTexture>texture=std::make_unique<plume::RenderTexture>();
 u32 HostWidth()const{return host_width;}u32 HostHeight()const{return host_height;}
};
std::mutex g_textures_mutex;std::unordered_map<u32,std::shared_ptr<TextureResource>>g_textures;
bool can_read=true,readback_ok=true,wait_ok=true,header_matches=true,cpu_write_during_wait=false,header_change_during_wait=false;
u32 readbacks=0,waits=0,invalidations=0;
std::array<u8,128> guest;std::vector<u8> gpu(64);
struct Memory {template<class T>T TranslatePhysical(u32 at){return reinterpret_cast<T>(guest.data()+at);}}memory;
#define REX_KERNEL_MEMORY() (&::memory)
enum class SyncReason{kResource};
struct HostDevice {
 static int LockRecording(){return 0;}static std::string_view BackendName(){return "D3D12";}
 static int Device(){return 1;}static int BeginFrameCommands(){return 2;}
 static bool Synchronize(SyncReason){++waits;if(cpu_write_during_wait)guest[40]^=1;
  if(header_change_during_wait)header_matches=false;return wait_ok;}
 static void RetireResource(int){}
};
bool TextureDataOverlapsPoolCopy(const TextureResource&,PhysicalCopyRange r){return CopyRangesOverlap({32,64},r);}
std::array<PhysicalCopyRange,2>TextureDataRanges(const TextureResource&r){
 return {PhysicalCopyRange{4096,64},r.levels>1?PhysicalCopyRange{8192,32}:PhysicalCopyRange{}};
}
bool TexturePlacementHeaderMatches(const TextureResource&){return header_matches;}
bool legodimensions::gpu_native::PoolCopyPhysicalRange(u32 at,u32 size,PhysicalCopyRange& r){r={at==4096?32:at==8192?96:at,size};return true;}
bool legodimensions::gpu_native::PoolCopyRangeReadable(PhysicalCopyRange){return can_read;}
auto OpenPhysicalWriteback(Memory*,std::span<const PhysicalCopyRange>){return std::make_unique<int>(1);}
void InvalidateCpuPhysicalMemory(u32 at,u32 size){CHECK(at==32&&size==64);++invalidations;}
struct TextureReadbackRequest {plume::RenderTexture*texture;u32 width,height;TextureReadbackFormat format;};
template<class F,class R>bool ReadbackTexturesRaw(int,int,std::span<const TextureReadbackRequest> requests,std::vector<std::vector<u8>>&out,F finish,R){
 ++readbacks;if(!readback_ok||!finish())return false;
 for(const auto& r:requests){CHECK(r.width*r.height*TextureReadbackTexelBytes(r.format)==gpu.size());out.push_back(gpu);}return true;}
'''+production+r'''
int main(int argc,char** argv){
 for(u32 i=0;i<64;++i)gpu[i]=u8(i+1);guest.fill(0x55);
 auto r=std::make_shared<TextureResource>();g_textures[9]=r;
 auto before=guest;
 if(argc>1&&std::string_view(argv[1])=="rejection-reasons") {
  const auto expect=[&](const char* reason){
   const auto prior=rejection_messages.size();
   CHECK(!MaterializePoolCopyTextureSource({33,1}));
   CHECK(rejection_messages.size()==prior+1&&rejection_messages.back()==reason);
   CHECK(guest==before&&!r->guest_backing_current&&invalidations==0);
  };
  r->levels=2;expect("mip-or-volume");r->levels=1;
  r->depth_alias_generation=1;expect("depth-alias");r->depth_alias_generation=0;
  r->host_width=8;expect("scale");r->host_width=4;
  r->writeback_coverage.Reset();expect("coverage");r->writeback_coverage.Record(4,4,0,0,4,4);
  header_matches=false;expect("placement");header_matches=true;
  can_read=false;expect("read-access");can_read=true;
  readback_ok=false;expect("gpu-readback");readback_ok=true;
  header_change_during_wait=true;expect("placement-after-wait");
  puts("PASS rejection reasons distinguish actual guarded materialization failures without publication");
  return 0;
 }
 // Every rejection class has its own four-message budget. Failure diagnostics
 // cannot change guest bytes or host authority, even after repeated failures.
 for(u32 i=0;i<u32(TextureWritebackRejection::kCount);++i) {
  const auto prior=rejection_messages.size();
  for(u32 repeat=0;repeat<6;++repeat)CHECK(!RejectTextureWriteback(TextureWritebackRejection(i),*r));
  CHECK(rejection_messages.size()==prior+4);
  for(u32 k=1;k<4;++k)CHECK(rejection_messages[prior+k]==rejection_messages[prior]);
  if(prior)CHECK(rejection_messages[prior]!=rejection_messages[prior-1]);
  CHECK(guest==before&&r->resolved_on_host&&!r->guest_backing_current&&r->cpu_stamp==7);
 }
 if(argc>1){CHECK(!MaterializePoolCopyTextureSource({33,1}));
  CHECK(readbacks==0&&guest==before&&r->resolved_on_host);return 0;}
 CHECK(MaterializePoolCopyTextureSource({0,1})&&readbacks==0&&guest==before);
 can_read=false;CHECK(!MaterializePoolCopyTextureSource({33,1})&&readbacks==0&&guest==before);
 can_read=true;r->levels=2;CHECK(!MaterializePoolCopyTextureSource({33,1})&&readbacks==0);
 r->levels=1;r->guest_fetch.stacked=1;CHECK(!MaterializePoolCopyTextureSource({33,1})&&readbacks==0);
 r->guest_fetch.stacked=0;r->host_width=8;CHECK(!MaterializePoolCopyTextureSource({33,1})&&readbacks==0);
 r->host_width=4;header_matches=false;CHECK(!MaterializePoolCopyTextureSource({33,1})&&readbacks==0);
 header_matches=true;readback_ok=false;CHECK(!MaterializePoolCopyTextureSource({33,1}));
 CHECK(r->resolved_on_host&&r->cpu_stamp==7&&guest==before&&invalidations==0);
 readback_ok=true;wait_ok=false;CHECK(!MaterializePoolCopyTextureSource({33,1}));
 CHECK(r->resolved_on_host&&guest==before&&invalidations==0);
 wait_ok=true;header_change_during_wait=true;
 CHECK(!MaterializePoolCopyTextureSource({33,1})&&guest==before&&invalidations==0);
 header_change_during_wait=false;header_matches=true;
 auto other=std::make_shared<TextureResource>();g_textures[10]=other;
 auto attempts=readbacks;CHECK(!MaterializePoolCopyTextureSource({33,1})&&readbacks==attempts&&guest==before);
 g_textures.erase(10);
 CHECK(MaterializePoolCopyTextureSource({33,1}));
 CHECK(r->guest_backing_current&&r->resolved_on_host&&r->sampled_valid&&r->guest_uploaded&&r->upload_source_key_valid);
 CHECK(!r->cpu_stamp&&r->depth_resolve_generation==8&&invalidations==1);
 CHECK(!PoolCopyHasHostTextureSource({33,1}));
 for(u32 y=0;y<4;++y)for(u32 x=0;x<4;++x)for(u32 c=0;c<4;++c)
  CHECK(guest[32+((y*16+x*4+c)^3)]==gpu[y*16+x*4+(c==0||c==2?2-c:c)]);
 for(u32 i=0;i<32;++i)CHECK(guest[i]==0x55&&guest[96+i]==0x55);
 attempts=readbacks;CHECK(MaterializePoolCopyTextureSource({33,1})&&readbacks==attempts);
 // A later GPU resolve starts a new version; it must require another readback.
 r->guest_backing_current=false;r->writeback_coverage.Record(r->HostWidth(),r->HostHeight(),0,0,r->HostWidth(),r->HostHeight());CHECK(PoolCopyHasHostTextureSource({33,1}));
 CHECK(MaterializePoolCopyTextureSource({33,1})&&readbacks==attempts+1);
 // Partial CPU destinations need the untouched GPU pixels first. Canonical
 // physical coverage may differ from the guest allocation address.
 r->guest_backing_current=false;r->writeback_coverage.Record(r->HostWidth(),r->HostHeight(),0,0,r->HostWidth(),r->HostHeight());guest=before;attempts=readbacks;
 CHECK(!TextureDataCoveredByPoolCopy(*r,{32,63}));
 CHECK(TextureDataCoveredByPoolCopy(*r,{32,64}));
 CHECK(MaterializePoolCopyTextureDestination({32,64})&&readbacks==attempts);
 CHECK(!r->guest_backing_current); // whole replacement intentionally skips readback
 CHECK(MaterializePoolCopyTextureDestination({48,16})&&readbacks==attempts+1&&r->guest_backing_current);
 // The preserving materialization lands the entire old image, not just the
 // bytes the incoming copy replaces.
 for(u32 y=0;y<4;++y)for(u32 x=0;x<4;++x)for(u32 c=0;c<4;++c)
  CHECK(guest[32+((y*16+x*4+c)^3)]==gpu[y*16+x*4+(c==0||c==2?2-c:c)]);
 // A whole base level does not cover a separate mip allocation.
 r->guest_backing_current=false;r->writeback_coverage.Record(r->HostWidth(),r->HostHeight(),0,0,r->HostWidth(),r->HostHeight());r->levels=2;
 CHECK(!TextureDataCoveredByPoolCopy(*r,{32,64}));
 CHECK(TextureDataCoveredByPoolCopy(*r,{32,96}));
 CHECK(!MaterializePoolCopyTextureDestination({32,64}));
 CHECK(MaterializePoolCopyTextureDestination({32,96}));
 r->levels=1;
 // Uniform 2x raw storage uses the center of each host block, preserving
 // endian and BGRA packing. A nonuniform scale was rejected above.
 r->host_width=r->host_height=8;r->guest_backing_current=false;r->writeback_coverage.Record(r->HostWidth(),r->HostHeight(),0,0,r->HostWidth(),r->HostHeight());
 gpu.resize(8*8*4);for(u32 i=0;i<gpu.size();++i)gpu[i]=u8(i*13+7);
 CHECK(MaterializePoolCopyTextureSource({33,1}));
 for(u32 y=0;y<4;++y)for(u32 x=0;x<4;++x)for(u32 c=0;c<4;++c)
  CHECK(guest[32+((y*16+x*4+c)^3)]==gpu[((2*y+1)*8+(2*x+1))*4+(c==0||c==2?2-c:c)]);
 // A CPU write during the real publication interval keeps that byte while
 // the other GPU bytes land. Retire the stale host view before later sampling.
 r->guest_backing_current=false;r->writeback_coverage.Record(8,8,0,0,8,8);
 guest=before;cpu_write_during_wait=true;const auto prior_invalidations=invalidations;
 CHECK(MaterializePoolCopyTextureSource({33,1}));
 CHECK(guest[40]==(before[40]^1) && invalidations==prior_invalidations+1);
 CHECK(r->guest_backing_current && !r->resolved_on_host && !r->sampled_valid && !r->guest_uploaded && !r->upload_source_key_valid);
 CHECK(!r->depth_resolve_generation && !PoolCopyHasHostTextureSource({33,1}));
 for(u32 y=0;y<4;++y)for(u32 x=0;x<4;++x)for(u32 c=0;c<4;++c){
  const auto at=32+((y*16+x*4+c)^3);if(at==40)continue;
  CHECK(guest[at]==gpu[((2*y+1)*8+(2*x+1))*4+(c==0||c==2?2-c:c)]);
 }
 puts("PASS production materialization, byte merge and stale host retirement, wait failures, ambiguity and scaled/mip guards");
}
'''
for variant in ('positive','stale-authority','stale-host-after-merge','unbounded-diagnostics','shared-diagnostic-budget'):
    code=fixture
    if variant=='stale-authority':code=code.replace('resource->guest_backing_current=true;','')
    if variant=='stale-host-after-merge':code=code.replace('if (merged.cpu_modified)', 'if (false)')
    if variant=='unbounded-diagnostics':code=code.replace('fetch_add(1, std::memory_order_relaxed) < 4)', 'fetch_add(1, std::memory_order_relaxed) < 5)')
    if variant=='shared-diagnostic-budget':code=code.replace('counts[index].fetch_add', 'counts[0].fetch_add')
    mutant=variant!='positive'
    name=variant+'-pool-writeback'
    cpp=a.output/(name+'.cpp');exe=a.output/(name+'.exe');cpp.write_text(code)
    subprocess.run([a.compiler,'-std=c++20','-O2','-D_CRT_SECURE_NO_WARNINGS','-I'+str(root/'rexlego/src'),str(cpp),'-o',str(exe)],check=True,timeout=45)
    env=dict(os.environ,LEGO_NATIVE_POOL_WRITEBACK='1')
    r=subprocess.run([str(exe.resolve())],env=env,capture_output=True,text=True,timeout=10)
    if r.returncode!=(23 if mutant else 0):raise RuntimeError(r.stdout+r.stderr)
    if not mutant:print(r.stdout.strip())
    if not mutant:
        disabled=subprocess.run([str(exe.resolve()),'disabled'],env=dict(env,LEGO_NATIVE_POOL_WRITEBACK='0'),capture_output=True,text=True,timeout=10)
        disabled.check_returncode()
        diagnostics=subprocess.run([str(exe.resolve()),'rejection-reasons'],env=env,capture_output=True,text=True,timeout=10)
        if diagnostics.returncode:raise RuntimeError(diagnostics.stdout+diagnostics.stderr)
        print(diagnostics.stdout.strip())
