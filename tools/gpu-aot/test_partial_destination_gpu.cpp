// GPU pixels -> production materialization -> partial CPU copy -> production
// invalidation. No game code, window, or external shader is needed.
#define NOMINMAX
#include "gpu_native/texture_readback.h"
#include "gpu_native/texture_writeback.h"
#include "gpu_native/pool_copy.h"
#include "gpu_native/resolve_region.h"
#include "gpu_native/memory_watch.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <future>
#include <memory>
#include <mutex>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>
using namespace legodimensions::gpu_native;
using u8=uint8_t;using u32=uint32_t;using u64=uint64_t;
#define CHECK(c) do{if(!(c)){std::fprintf(stderr,"line %d: %s\n",__LINE__,#c);std::exit(23);}}while(0)
#define REXLOG_INFO(...) ((void)0)
#define REXLOG_WARN(...) ((void)0)
#define REXLOG_ERROR(...) ((void)0)
namespace plume {std::unique_ptr<RenderInterface> CreateD3D12Interface();}
namespace rex {constexpr u32 align(u32 a,u32 n){return (a+n-1)&~(n-1);}}
namespace xenos {constexpr u32 kTextureTileWidthHeight=32;}
namespace rex::graphics::xenos {
constexpr u32 kTextureTileWidthHeight=32;
enum class DataDimension{k2DOrStacked=1,k3D=2};enum class TextureFormat{k_8_8_8_8=6,k_16_16=25,k_16_16_16_16=26,k_16_FLOAT=30,k_16_16_FLOAT=31,k_16_16_16_16_FLOAT=32,k_32_FLOAT=36,k_32_32_FLOAT=37,k_32_32_32_32_FLOAT=38};
}
namespace rex::graphics::texture_util {
#include "tile-address.inc"
struct Layout {struct {u32 level_data_extent_bytes=0,row_pitch_bytes=0;}base;u32 mips_total_extent_bytes=0;};
Layout layout;
Layout GetGuestTextureLayout(auto...){return layout;}
void GetPackedMipOffset(auto,auto,auto,auto,auto,u32&x,u32&y,u32&z){x=y=z=0;}
}
struct Fetch {
 rex::graphics::xenos::DataDimension dimension=rex::graphics::xenos::DataDimension::k2DOrStacked;
 rex::graphics::xenos::TextureFormat format=rex::graphics::xenos::TextureFormat::k_8_8_8_8;
 u32 base_address=32,pitch=3,tiled=0,stacked=0,packed_mips=0,endianness=2,swizzle=0x60a,mip_address=0;
 u32 num_format=0,exp_adjust=0,sign_x=0,sign_y=0,sign_z=0,sign_w=0;
};
struct TextureResource {
 TextureWritebackCoverage writeback_coverage;
 std::mutex mutex;bool surface=false,owns_guest_memory=false,resolved_on_host=true,guest_backing_current=false;
 bool guest_uploaded=false,upload_source_key_valid=false,sampled_valid=true;
 u32 guest_address=0x80001000,mirror_address=0,mirror_size=0,width=64,height=32,scale_twice=2,depth=1,levels=1;
 u32 descriptor_index=3,resolved_descriptor_index=4,sampled_descriptor_index=5;
 std::vector<u32> previous_surface_descriptors;
 u32 depth_alias_generation=0,depth_resolve_generation=0;
 CpuMemoryStamp cpu_stamp;
 plume::RenderFormat format=plume::RenderFormat::R8G8B8A8_UNORM;
 Fetch guest_fetch;
 mutable bool layout_cached=false;mutable Fetch layout_fetch{};mutable u32 layout_key[4]{};
 mutable u32 layout_base_bytes=0,layout_mips_bytes=0;
 std::shared_ptr<plume::RenderTexture> texture;
 u32 HostWidth()const{return width*scale_twice/2;}u32 HostHeight()const{return height*scale_twice/2;}
};
struct D3DTexture {u8 data[52];};
struct Memory {
 std::vector<u8> data=std::vector<u8>(1u<<20,0x55);
 template<class T=u8*>T TranslatePhysical(u32 at){return reinterpret_cast<T>(data.data()+at);}
 void SystemHeapFree(u32){CHECK(false);}
}memory;
#define REX_KERNEL_MEMORY() (&::memory)
std::mutex g_textures_mutex;
std::unordered_map<u32,std::shared_ptr<TextureResource>> g_textures;
std::unique_ptr<plume::RenderDevice> device;
std::unique_ptr<plume::RenderCommandQueue> queue;
std::unique_ptr<plume::RenderCommandList> commands;
std::unique_ptr<plume::RenderCommandFence> fence;
bool open=false;u32 readbacks=0,preparation_waits=0,invalidations=0;
bool cpu_write_during_wait=false;u32 cpu_write_address=0;
std::thread::id coordinator;
std::future<void> recorder;
std::vector<std::shared_ptr<void>> retained;
void Finish(){CHECK(open);commands->end();queue->executeCommandLists(commands.get(),fence.get());queue->waitForCommandFence(fence.get());open=false;}
enum class SyncReason{kResource};
struct HostDevice {
 static int LockRecording(){return 0;}
 static auto BackendName(){return std::string_view("D3D12");}
 static auto Device(){return device.get();}
 static auto BeginFrameCommands(){CHECK(open);return commands.get();}
 static bool Synchronize(SyncReason){CHECK(std::this_thread::get_id()!=coordinator);++readbacks;Finish();
  if(cpu_write_during_wait)memory.data[cpu_write_address]^=0x80;return true;}
 template<class T>static void RetireResource(std::shared_ptr<T> r){retained.push_back(std::move(r));}
 static void UnregisterTexture(u32){CHECK(false);}
};
bool legodimensions::gpu_native::PoolCopyPhysicalRange(u32 at,u32 n,PhysicalCopyRange&r){
 if(!n||u64(at)+n>memory.data.size())return false;r={at,n};return true;
}
bool legodimensions::gpu_native::PoolCopyRangeWritable(PhysicalCopyRange r){return u64(r.address)+r.length<=memory.data.size();}
bool legodimensions::gpu_native::PoolCopyRangeReadable(PhysicalCopyRange r){return u64(r.address)+r.length<=memory.data.size();}
// Protection-window behavior is exercised against real SDK mappings by the
// physical-write-scope fixture. This boundary supports transaction failures.
bool physical_window_ok=true;
std::recursive_mutex fixture_transaction;
auto LockPhysicalWriteback(){return std::unique_lock(fixture_transaction);}
auto OpenPhysicalWriteback(Memory*,std::span<const PhysicalCopyRange>){return physical_window_ok?std::make_unique<int>(1):nullptr;}
bool legodimensions::gpu_native::GuestRangeOverlapsPoolCopy(u32 at,u32 n,PhysicalCopyRange r){
 PhysicalCopyRange p;return PoolCopyPhysicalRange(at,n,p)&&CopyRangesOverlap(p,r);
}
bool TexturePlacementHeaderMatches(const TextureResource&){return true;}
#ifndef LEGO_TEST_REAL_MEMORY_WATCH
void legodimensions::gpu_native::InvalidateCpuPhysicalMemory(u32,u32){++invalidations;}
bool legodimensions::gpu_native::CpuMemoryWatchEnabled(){return false;}
CpuMemoryStamp legodimensions::gpu_native::WatchCpuMemory(std::span<const CpuMemorySpan>){return {};}
bool legodimensions::gpu_native::CpuMemoryUnchanged(const CpuMemoryStamp&){return false;}
#endif
void InvalidateFramebufferReferences(u32){CHECK(false);}
std::vector<std::shared_ptr<TextureResource>> PoolCopyTextureCandidates(PhysicalCopyRange){
 std::vector<std::shared_ptr<TextureResource>> r;for(auto&[a,t]:g_textures)r.push_back(t);return r;
}
bool PoolCopyMayTouchTextures(PhysicalCopyRange){return true;}
bool PoolCopyMayTouchBuffers(PhysicalCopyRange){return false;}
bool PoolCopyMayTouchShaders(PhysicalCopyRange){return false;}
u32 InvalidatePoolCopyBuffers(PhysicalCopyRange){return 0;}
u32 InvalidatePoolCopyShaders(PhysicalCopyRange){return 0;}
void WaitAllPreparation(){++preparation_waits;}
bool OnNativeRecorderThread(){return std::this_thread::get_id()!=coordinator;}
void NativeRecorderSubmit(std::function<void()> fn,bool present,bool barrier){
 CHECK(!present&&barrier);recorder=std::async(std::launch::async,std::move(fn));
}
u32 NativeRecorderBarrier(){return 1;}
void NativeRecorderWaitExecuted(u32 n){CHECK(n==1);recorder.get();}
struct PoolCopyBatch {bool active=false,waited=false;std::vector<PhysicalCopyRange>textures,buffers,shaders;}g_pool_batch;
bool BatchAccessible(PhysicalCopyRange r,bool){return u64(r.address)+r.length<=memory.data.size();}
// Full production coverage, readback, ownership transition and coordinator bodies.
#include "partial-destination-production.inc"

int main(){
 coordinator=std::this_thread::get_id();auto api=plume::CreateD3D12Interface();device=api->createDevice();CHECK(device);
 queue=device->createCommandQueue(plume::RenderCommandListType::DIRECT);commands=queue->createCommandList();fence=device->createCommandFence();
 auto*raw=static_cast<plume::D3D12Device*>(device.get())->d3d;
 u32 cases=0;
 for(u32 raw_format=0;raw_format<3;++raw_format)for(u32 packing=0;packing<2;++packing)
 for(u32 scale:{2u,3u,4u})for(bool tiled:{false,true})for(u32 endian=0;endian<4;++endian)for(bool swap:{false,true})for(bool whole:{false,true})for(bool partial_resolve:{false,true})for(bool later_cpu_write:{false,true}){
  if(!raw_format && packing)continue;
  auto resource=std::make_shared<TextureResource>();auto&f=resource->guest_fetch;
  resource->scale_twice=scale;
  f.tiled=tiled;f.endianness=endian;f.swizzle=swap?(raw_format?0xb48:0x60a):0x688;
  if(raw_format){f.num_format=packing;f.exp_adjust=packing?3:0;}
  if(raw_format==1){resource->format=plume::RenderFormat::R16G16_UNORM;f.format=rex::graphics::xenos::TextureFormat::k_16_16;}
  if(raw_format==2){resource->format=plume::RenderFormat::R16G16B16A16_UNORM;f.format=rex::graphics::xenos::TextureFormat::k_16_16_16_16;}
  const u32 texel_bytes=raw_format==2?8:4;
  const u32 w=resource->width,h=resource->height,pitch=96*texel_bytes,base=f.base_address<<12;
  auto offset=[&](u32 x,u32 y)->u32{return tiled?rex::graphics::texture_util::GetTiledOffset2D(x,y,pitch/texel_bytes,texel_bytes==8?3:2):y*pitch+x*texel_bytes;};
  u32 extent=0;for(u32 y=0;y<h;++y)for(u32 x=0;x<w;++x)extent=std::max(extent,offset(x,y)+texel_bytes);
  extent=(extent+4095)&~4095u;
  rex::graphics::texture_util::layout={{extent,pitch},0};
  memory.data.assign(memory.data.size(),0x55);for(u32 i=0;i<extent;++i)memory.data[4096+i]=u8(i*37+19);
  auto expected=memory.data;
  plume::RenderTextureDesc td;td.dimension=plume::RenderTextureDimension::TEXTURE_2D;td.width=w;td.height=h;
  td.width=resource->HostWidth();td.height=resource->HostHeight();
  td.depth=td.arraySize=td.mipLevels=1;td.format=resource->format;
  resource->texture=device->createTexture(td);CHECK(resource->texture);
  auto*image=static_cast<plume::D3D12Texture*>(resource->texture.get())->d3d;CHECK(image);
  auto desc=image->GetDesc();D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};UINT64 total=0;
  raw->GetCopyableFootprints(&desc,0,1,0,&fp,nullptr,nullptr,&total);
  auto upload=device->createBuffer(plume::RenderBufferDesc::UploadBuffer(total));CHECK(upload);
  auto*mapped=static_cast<u8*>(upload->map());CHECK(mapped);std::memset(mapped,0xee,size_t(total));
  const u32 endian_xor=endian==1?1:endian==2?3:endian==3?2:0;
  if(!whole)for(u32 y=0;y<h;++y)for(u32 x=0;x<w;++x)for(u32 c=0;c<texel_bytes;++c){
   const u32 sx=(x*resource->HostWidth()+resource->HostWidth()/2)/w;
   const u32 sy=(y*resource->HostHeight()+resource->HostHeight()/2)/h;
   if(partial_resolve&&sy>=resource->HostHeight()/2)continue;
   const u8 value=u8(sx*17+sy*29+c*61);
   const u32 channel=!raw_format&&swap&&(c==0||c==2)?2-c:c;
   expected[base+((offset(x,y)+channel)^endian_xor)]=value;
  }
  // Leave the uncovered lower host texels poisoned. They are storage bytes,
  // not guest-visible resolve writes, and must not replace guest memory.
  for(u32 y=0;y<(partial_resolve?resource->HostHeight()/2:resource->HostHeight());++y)for(u32 x=0;x<resource->HostWidth();++x)for(u32 c=0;c<texel_bytes;++c)
   mapped[fp.Offset+size_t(y)*fp.Footprint.RowPitch+x*texel_bytes+c]=u8(x*17+y*29+c*61);
  upload->unmap();commands->begin();open=true;
  commands->barriers(plume::RenderBarrierStage::COPY,plume::RenderTextureBarrier(resource->texture.get(),plume::RenderTextureLayout::COPY_DEST));
  D3D12_TEXTURE_COPY_LOCATION src{},dst{};src.pResource=static_cast<plume::D3D12Buffer*>(upload.get())->d3d;
  src.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;src.PlacedFootprint=fp;dst.pResource=image;dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  static_cast<plume::D3D12CommandList*>(commands.get())->d3d->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
  commands->barriers(plume::RenderBarrierStage::GRAPHICS,plume::RenderTextureBarrier(resource->texture.get(),plume::RenderTextureLayout::SHADER_READ));
  g_textures.clear();g_textures.emplace(resource->guest_address,resource);g_pool_batch={};
  ResolveRegion top{};top.width=resource->HostWidth();top.height=resource->HostHeight()/2;
  RecordResolvedTexture(*resource,top,0,0);
  if(!partial_resolve){auto bottom=top;bottom.y=top.height;RecordResolvedTexture(*resource,bottom,0,0);}
  const u32 destination=whole?base:base+4096,bytes=whole?extent:4096;
  const auto before=readbacks;
  cpu_write_during_wait=later_cpu_write;cpu_write_address=base;
  if(later_cpu_write&&!whole)expected[base]=0x55^0x80;
  std::memmove(expected.data()+destination,expected.data()+4096,bytes);
  PoolCopyRecorded(destination,4096,bytes,{4096,bytes},{destination,bytes});
  FlushPoolCopyBatch();
  CHECK(readbacks==before+(whole?0:1));CHECK(!resource->resolved_on_host&&!resource->sampled_valid);
  for(size_t i=0;i<expected.size();++i)if(memory.data[i]!=expected[i]){
   std::fprintf(stderr,"case%u whole=%u tiled=%u endian=%u swap=%u byte=%zu actual=%u expected=%u\n",cases,whole,tiled,endian,swap,i,memory.data[i],expected[i]);return 23;
  }
  if(open)Finish();retained.clear();++cases;
 }
 std::printf("PASS production GPU destination preservation: %u cases, %u readbacks; RGBA8/RG16/RGBA16 raw bits, tiled/linear/endian/BGRA, untouched pixels, complete overwrite without readback\n",cases,readbacks);
}
