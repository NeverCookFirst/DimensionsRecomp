// Headless integration of production UNORM24 upload, real SDK layout and native tapes.
#define NOMINMAX
#define XXH_INLINE_ALL
#include "gpu_native/texture_upload_publication.h"
#include "gpu_native/texture_upload_source.h"
#include "gpu_native/texture_upload_cache.h"
#include "gpu_native/texture_depth_upload.h"
#include "gpu_native/texture_swizzle.h"
#include <rex/graphics/pipeline/texture/util.h>
#include <plume_d3d12.h>
#include <d3d12sdklayers.h>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <vector>
using namespace legodimensions::gpu_native;
using u8=uint8_t;using u32=uint32_t;using u64=uint64_t;
#define CHECK(c) do{if(!(c)){std::fprintf(stderr,"line %d: %s\n",__LINE__,#c);std::exit(23);}}while(0)
namespace plume {std::unique_ptr<RenderInterface> CreateD3D12Interface();}
struct CpuMemorySpan{u32 address,length;};
struct CpuMemoryStamp{u64 epoch=0;};
struct GuestSource{std::vector<u8> bytes;u64 epoch=1;u32 reads=0;std::function<void()> after_read;};
GuestSource guest;
bool NativeTextureTimingEnabled(){return false;}
bool CpuMemoryWatchEnabled(){return true;}
bool CpuMemoryUnchanged(CpuMemoryStamp stamp){return stamp.epoch==guest.epoch;}
CpuMemoryStamp WatchCpuMemory(std::span<const CpuMemorySpan>){return{guest.epoch};}
bool ReadableUploadSpan(u32 address,u32 bytes){return address==0x1000&&bytes&&bytes<=guest.bytes.size();}
bool CopyTextureUploadSpan(u32 address,std::span<u8> output){
 if(!ReadableUploadSpan(address,u32(output.size())))return false;
 std::memcpy(output.data(),guest.bytes.data(),output.size());++guest.reads;
 auto mutation=std::exchange(guest.after_read,{});if(mutation)mutation();return true;
}
// Only used to compile the frozen native reference's actual direct-read path.
struct GuestMemory{
 template<typename T>T TranslatePhysical(u32 address){CHECK(address==0x1000);++guest.reads;return reinterpret_cast<T>(guest.bytes.data());}
 template<typename T>T TranslateVirtual(u32 address){return TranslatePhysical<T>(address);}
} guest_memory;
#define REX_KERNEL_MEMORY() (&guest_memory)
bool LongProbeEnabled(){return false;}
template<typename... T>void LongProbeEvent(T&&...){}
struct Timing{double hash_ms=0,source_ms=0;u64 hashed_bytes=0,converted_bytes=0,source_hits=0;}g_upload_timing;
enum class D3DResourceType:u32{kTexture=2};
bool IsDepthFormat(plume::RenderFormat f){return f==plume::RenderFormat::D32_FLOAT_S8_UINT;}
u32 RowPitch(plume::RenderFormat f,u32 width){CHECK(f==plume::RenderFormat::R32_FLOAT);return(width*4+255)&~255u;}
struct TextureResource{
 std::mutex mutex;u32 guest_address=0x2000,width=1,height=1,host_width=0,levels=1,guest_format=22;
 u32 d3d_type=u32(D3DResourceType::kTexture),sampled_descriptor_index=~0u;
 bool surface=false,owns_guest_memory=false,resolved_on_host=false,guest_uploaded=false,sampled_valid=false,upload_source_key_valid=false;
 plume::RenderFormat format=plume::RenderFormat::D32_FLOAT_S8_UINT;
 rex::graphics::xenos::xe_gpu_texture_fetch_t guest_fetch{};
 std::unique_ptr<plume::RenderTexture> texture,sampled_texture;
 std::unique_ptr<plume::RenderTextureView> sampled_view;
 std::weak_ptr<TextureUploadPublication> prepared_upload;
 std::shared_ptr<TextureUploadReplayState> upload_replay;
 TextureUploadSourceKey upload_source_key{};u64 guest_content_hash=0;CpuMemoryStamp cpu_stamp;
 u32 HostWidth()const{return host_width?host_width:width;}u32 HostHeight()const{return height;}
};
TextureUploadPublicationCache g_texture_upload_publications(128ull*1024*1024);
std::vector<std::shared_ptr<void>>* retirement=nullptr;
struct HostDevice{
 static inline plume::RenderDevice* device=nullptr;static inline u32 descriptor=0;
 static plume::RenderDevice* Device(){return device;}
 static u32 RegisterTexture(plume::RenderTexture*,plume::RenderTextureView*){return descriptor++;}
 static void RetireResource(std::shared_ptr<void> p){CHECK(retirement);retirement->push_back(std::move(p));}
};
#include "depth-publication-production.inc"
u32 Pattern(u32 x,u32 y,u32 version){
 // Include exact endpoints and the rounding boundary, with varying stencil.
 if(x==0&&y==0)return 0;if(x==1&&y==0)return 0xFFFFFF;
 if(x==2&&y==0)return 0x7FFFFF;if(x==3&&y==0)return 0x800000;
 return ((x*0x14375u)^(y*0x92E7u)^(version*0x517CDu))&0xFFFFFF;
}
void Encode(TextureResource& r,u32 version){
 namespace tu=rex::graphics::texture_util;const auto& f=r.guest_fetch;
 auto l=tu::GetGuestTextureLayout(f.dimension,f.pitch,r.width,r.height,1,f.tiled,f.format,f.packed_mips,true,0);
 guest.bytes.assign(l.base.level_data_extent_bytes,0xAE);++guest.epoch;
 u32 ox=0,oy=0,oz=0;if(f.packed_mips)tu::GetPackedMipOffset(r.width,r.height,1,f.format,0,ox,oy,oz);
 const u32 lane[4]={0,1,3,2};
 for(u32 y=0;y<r.height;++y)for(u32 x=0;x<r.width;++x){
  const u64 offset=f.tiled?tu::GetTiledOffset2D(x+ox,y+oy,l.base.row_pitch_bytes/4,2):u64(y+oy)*l.base.row_pitch_bytes+u64(x+ox)*4;
  const u32 packed=(Pattern(x,y,version)<<8)|((x+y+version)&255);
  CHECK(offset+4<=guest.bytes.size());
  for(u32 byte=0;byte<4;++byte)guest.bytes[size_t(offset)+(byte^lane[u32(f.endianness)])]=u8(packed>>(byte*8));
 }
}
int main(){
 ID3D12Debug* debug=nullptr;const bool enabled=SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)));
 if(enabled){debug->EnableDebugLayer();debug->Release();}
 auto api=plume::CreateD3D12Interface();auto device=api->createDevice();CHECK(device);HostDevice::device=device.get();
 auto* raw=static_cast<plume::D3D12Device*>(device.get())->d3d;
 auto queue=device->createCommandQueue(plume::RenderCommandListType::DIRECT);
 auto commands=queue->createCommandList();auto fence=device->createCommandFence();
 unsigned images=0,pixels=0,cache_hits=0,retained_mutations=0;
 auto initialize=[&](TextureResource& r,u32 width,u32 height,bool tiled,u32 endian,bool packed){
  namespace x=rex::graphics::xenos;r.width=width;r.height=height;
  auto& f=r.guest_fetch;f.dimension=x::DataDimension::k2DOrStacked;f.format=x::TextureFormat::k_24_8;
  f.base_address=1;f.pitch=((width*4+255)&~255u)/128;f.tiled=tiled;f.endianness=x::Endian(endian);f.packed_mips=packed;
  f.swizzle=0xB48;f.size_2d.width=width-1;f.size_2d.height=height-1;
  plume::RenderTextureDesc d;d.dimension=plume::RenderTextureDimension::TEXTURE_2D;
  d.width=width;d.height=height;d.depth=d.arraySize=d.mipLevels=1;d.format=plume::RenderFormat::R32_FLOAT;
  d.committed=true;d.multisampling.sampleCount=plume::RenderSampleCount::COUNT_1;
  // The source depth render attachment is not consumed by a guest CPU upload;
  // the production mirror allocation/view/copy below are real D3D12 objects.
  r.texture=device->createTexture(d);CHECK(r.texture);
 };
 auto read=[&](TextureResource& r,NativeDrawCommands* tape,u32 version,unsigned layers){
  CHECK(r.sampled_texture);auto* tex=static_cast<plume::D3D12Texture*>(r.sampled_texture.get());
  const auto description=tex->d3d->GetDesc();D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};UINT64 bytes=0;
  raw->GetCopyableFootprints(&description,0,1,0,&footprint,nullptr,nullptr,&bytes);
  auto buffer=device->createBuffer(plume::RenderBufferDesc::ReadbackBuffer(bytes));CHECK(buffer);
  commands->begin();ResetNativeReplayState(commands.get());
  auto middle=CreateNativeDrawCommands(),outer=CreateNativeDrawCommands();
  if(layers==0)tape->Replay(commands.get());
  if(layers==1){tape->Replay(middle.get());middle->Replay(commands.get());}
  if(layers==2){tape->Replay(outer.get());outer->Replay(middle.get());middle->Replay(commands.get());}
  commands->barriers(plume::RenderBarrierStage::COPY,{r.sampled_texture.get(),plume::RenderTextureLayout::COPY_SOURCE});
  D3D12_TEXTURE_COPY_LOCATION src{},dst{};src.pResource=tex->d3d;src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  dst.pResource=static_cast<plume::D3D12Buffer*>(buffer.get())->d3d;dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;dst.PlacedFootprint=footprint;
  static_cast<plume::D3D12CommandList*>(commands.get())->d3d->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
  commands->barriers(plume::RenderBarrierStage::GRAPHICS,{r.sampled_texture.get(),plume::RenderTextureLayout::SHADER_READ});
  commands->end();queue->executeCommandLists(commands.get(),fence.get());queue->waitForCommandFence(fence.get());
  auto* data=static_cast<const u8*>(buffer->map());CHECK(data);
  for(u32 y=0;y<r.height;++y)for(u32 x=0;x<r.width;++x){float actual=0;std::memcpy(&actual,data+footprint.Offset+u64(y)*footprint.Footprint.RowPitch+x*4,4);
   const double expected=double(Pattern(x,y,version))/16777215.0;
   const bool matches=std::isfinite(actual)&&std::abs(double(actual)-expected)<=0.5/16777216.0+1e-12;
   if(!matches)std::fprintf(stderr,"depth mismatch image=%u size=%ux%u tiled=%u endian=%u packed=%u version=%u tape_depth=%u pixel=%u,%u actual=%.9g bits=%08X expected=%.17g\n",
       images,r.width,r.height,u32(r.guest_fetch.tiled),u32(r.guest_fetch.endianness),u32(r.guest_fetch.packed_mips),version,layers,x,y,actual,std::bit_cast<u32>(actual),expected);
   CHECK(matches);++pixels;}
  buffer->unmap();++images;tape->Reset();
 };
 for(bool tiled:{false,true})for(u32 endian=0;endian<4;++endian)
 for(const auto shape:std::array<std::array<u32,3>,5>{{{37,19,0},{9,7,1},{1,1,0},{31,33,0},{1025,3,0}}}){
  TextureResource r;initialize(r,shape[0],shape[1],tiled,endian,shape[2]!=0);
  std::array<std::vector<std::shared_ptr<void>>,3> leases;std::array<std::unique_ptr<NativeDrawCommands>,3> tapes;
  for(auto& t:tapes)t=CreateNativeDrawCommands();
  Encode(r,1);retirement=&leases[2];CHECK(UploadGuestDepthTexture(r,tapes[2].get(),false));
  const auto reads=guest.reads;retirement=&leases[0];CHECK(UploadGuestDepthTexture(r,tapes[0].get(),false));
  CHECK(guest.reads==reads);++cache_hits;
  Encode(r,2);retirement=&leases[1];CHECK(UploadGuestDepthTexture(r,tapes[1].get(),false));
  // Prepare A/B/A out of submission order. Each final consumer must publish
  // its own immutable decoded version, even through nested native tapes.
  read(r,tapes[0].get(),1,0);leases[0].clear();read(r,tapes[1].get(),2,1);leases[1].clear();
  read(r,tapes[2].get(),1,2);leases[2].clear();
  auto race=CreateNativeDrawCommands();std::vector<std::shared_ptr<void>> race_lease;retirement=&race_lease;
  Encode(r,3);guest.after_read=[&]{Encode(r,4);};CHECK(UploadGuestDepthTexture(r,race.get(),true));
  read(r,race.get(),3,2);race_lease.clear();++retained_mutations;
  CHECK(UploadGuestDepthTexture(r,race.get(),false));read(r,race.get(),4,1);race_lease.clear();
  // Eviction must expire reusable staging only after every submitted consumer
  // has reached its fence. A subsequent cache miss decodes current CPU bytes.
  g_texture_upload_publications.Reset();CHECK(UploadGuestDepthTexture(r,race.get(),true));
  read(r,race.get(),4,0);race_lease.clear();
 }
 g_texture_upload_publications.Reset();retirement=nullptr;
 ID3D12InfoQueue* info=nullptr;unsigned errors=0;
 if(SUCCEEDED(raw->QueryInterface(IID_PPV_ARGS(&info)))){
  for(UINT64 i=0;i<info->GetNumStoredMessages();++i){SIZE_T n=0;info->GetMessage(i,nullptr,&n);std::vector<u8> b(n);auto* m=reinterpret_cast<D3D12_MESSAGE*>(b.data());info->GetMessage(i,m,&n);
   if(m->Severity<=D3D12_MESSAGE_SEVERITY_ERROR){++errors;std::fprintf(stderr,"D3D12 %u: %s\n",m->ID,m->pDescription);}}
  info->Release();
 }
 CHECK(!errors&&raw->GetDeviceRemovedReason()==S_OK);
 std::printf("PASS %u depth GPU readbacks, %u pixel comparisons, %u watch-cache hits and %u retained-source mutations; real SDK linear/tiled/packed layout, four endians, A/B/A and three native tape depths; debug_layer=%u\n",images,pixels,cache_hits,retained_mutations,unsigned(enabled));
}
