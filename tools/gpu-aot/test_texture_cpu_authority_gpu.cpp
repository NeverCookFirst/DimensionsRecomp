// Real SDK write faults + production publication/fast paths + actual GPU
// re-upload/readback. The upload driver supplies controlled layout/storage.
#include "authority-prefix.inc"
#include "make-image.inc"
#include "gpu_native/texture_volume_upload.h"

struct TextureResourceView {plume::RenderTexture* texture=nullptr;u32 descriptor_index=~0u;};
enum class TexturePreparationResult {kUnavailable,kReady,kUploadFailed};
struct TexturePreparationSnapshot {std::shared_ptr<void> owner;std::array<u32,1+sizeof(Fetch)/4> header{};bool alias_settled=true;};
bool LongProbeEnabled(){return false;}
bool DepthAliasesEnabled(){return false;}
bool IsDepthFormat(plume::RenderFormat){return false;}
bool CurrentDrawPreparation(){return false;}
bool NativeFsr3Enabled(){return false;}
auto FindTexture(u32 at){auto it=g_textures.find(at);return it==g_textures.end()?std::shared_ptr<TextureResource>{}:it->second;}
auto AdoptTexture(u32 at){return FindTexture(at);}
TextureResourceView TextureResourceViewOf(u32,const std::shared_ptr<TextureResource>& resource){
 return resource?TextureResourceView{resource->texture.get(),resource->descriptor_index}:TextureResourceView{};
}
struct UploadTimer{};
plume::RenderCommandList* NativeResourceCommands(plume::RenderCommandList* value){return value;}
TexturePreparationResult PrepareTextureResource(u32,plume::RenderCommandList*,bool,TextureResourceView&);
#include "authority-production.inc"
u32 uploads=0;
bool fail_upload=false;

// The production entry guard decides whether CPU data must be uploaded. This
// bounded driver then uses the same checked block copier with a controlled
// single-level layout, and records actual Plume GPU copies.
TexturePreparationResult PrepareTextureResource(u32 at,plume::RenderCommandList* list,bool hash,TextureResourceView& view){
 auto r=FindTexture(at);if(!r)return TexturePreparationResult::kUnavailable;
 view={};
 if(ProductionUploadEntry(at,r,list,hash)){view=TextureResourceViewOf(at,r);return TexturePreparationResult::kReady;}
 if(fail_upload)return TexturePreparationResult::kUploadFailed;
 ++uploads;
 auto* raw=static_cast<plume::D3D12Device*>(device.get())->d3d;
 auto old=r->texture;
 plume::RenderTextureDesc td;td.dimension=plume::RenderTextureDimension::TEXTURE_2D;
 td.width=r->width;td.height=r->height;td.depth=td.arraySize=td.mipLevels=1;td.format=r->format;
 r->texture=device->createTexture(td);CHECK(r->texture);r->scale_twice=2;
 auto desc=static_cast<plume::D3D12Texture*>(r->texture.get())->d3d->GetDesc();
 D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};UINT64 bytes=0;raw->GetCopyableFootprints(&desc,0,1,0,&fp,nullptr,nullptr,&bytes);
 auto upload=device->createBuffer(plume::RenderBufferDesc::UploadBuffer(bytes));CHECK(upload);
 auto* mapped=static_cast<u8*>(upload->map());CHECK(mapped);std::memset(mapped,0,size_t(bytes));
 const auto& f=r->guest_fetch;const u32 endian=f.endianness==1?1:f.endianness==2?3:f.endianness==3?2:0;
 auto* source=fixture::sdk_memory->TranslatePhysical<const u8*>(f.base_address<<12);
 CHECK(CopyTextureVolumeBlocks({source,32768},{mapped+fp.Offset,size_t(bytes-fp.Offset)},
   r->width,r->height,1,4,fp.Footprint.RowPitch,endian,[&](u32 x,u32 y,u32)->int64_t{
    return f.tiled?rex::graphics::texture_util::GetTiledOffset2D(x,y,128,2):y*512+x*4;
   }));
 upload->unmap();commands->barriers(plume::RenderBarrierStage::COPY,plume::RenderTextureBarrier(r->texture.get(),plume::RenderTextureLayout::COPY_DEST));
 D3D12_TEXTURE_COPY_LOCATION src{},dst{};src.pResource=static_cast<plume::D3D12Buffer*>(upload.get())->d3d;
 src.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;src.PlacedFootprint=fp;
 dst.pResource=static_cast<plume::D3D12Texture*>(r->texture.get())->d3d;dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
 static_cast<plume::D3D12CommandList*>(commands.get())->d3d->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
 commands->barriers(plume::RenderBarrierStage::GRAPHICS,plume::RenderTextureBarrier(r->texture.get(),plume::RenderTextureLayout::SHADER_READ));
 HostDevice::RetireResource(std::move(old));HostDevice::RetireResource(std::shared_ptr<plume::RenderBuffer>(upload.release()));
 r->guest_uploaded=true;view=TextureResourceViewOf(at,r);return TexturePreparationResult::kReady;
}

int main(){
 SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
 char loaded[MAX_PATH]{};CHECK(GetModuleFileNameA(GetModuleHandleA("rexruntime.dll"),loaded,MAX_PATH));
 CHECK(!_stricmp(loaded,std::getenv("LEGO_TEST_RUNTIME_EXPECTED")));
 rex::memory::Memory sdk_memory;CHECK(sdk_memory.Initialize());fixture::sdk_memory=&sdk_memory;
 memory.data={sdk_memory.TranslatePhysical<u8*>(0),1u<<20};
 coordinator=std::this_thread::get_id();auto api=plume::CreateD3D12Interface();device=api->createDevice();CHECK(device);
 queue=device->createCommandQueue(plume::RenderCommandListType::DIRECT);commands=queue->createCommandList();fence=device->createCommandFence();
 rex::graphics::texture_util::layout={{32768,512},0};u32 cases=0;
 const bool watch=CpuMemoryWatchEnabled();
 for(u32 scale:{2u,3u,4u})for(bool tiled:{false,true})for(u32 endian=0;endian<4;++endian)
 for(bool partial:{false,true})for(u32 alias:{0u,1u,2u,3u})for(u32 path=0;path<3;++path){
  g_textures.clear();
  {const PhysicalCopyRange whole{0,u32(memory.data.size())};auto window=OpenPhysicalWriteback(&sdk_memory,{&whole,1});CHECK(window);
   std::fill(memory.data.begin(),memory.data.end(),0x55);InvalidateCpuPhysicalMemory(0,u32(memory.data.size()));}
  auto expected=std::vector<u8>(memory.data.begin(),memory.data.end());
  commands->begin();open=true;auto image=MakeImage(0,0,scale,tiled,endian,partial,expected);
  auto neighbor=MakeImage(1,0,scale,!tiled,endian,!partial,expected);
  auto r=image.resource;const auto gpu=r->texture;const auto before=readbacks;
  std::array owners{r,neighbor.resource};Recorder([&]{CHECK(MaterializeResolvedTextures(owners));});
  CHECK(readbacks==before+1);BytesMatch(expected);
  CHECK(r->resolved_on_host&&r->guest_backing_current);
  CHECK(CpuMemoryUnchanged(r->cpu_stamp)==watch);
  CHECK(CpuMemoryUnchanged(neighbor.resource->cpu_stamp)==watch);
  TexturePreparationSnapshot snapshot;snapshot.owner=r;snapshot.header[0]=r->adopted_header_type;
  std::memcpy(snapshot.header.data()+1,&r->guest_fetch,sizeof(Fetch));
  if(watch){CHECK(PrepareCachedTexture(snapshot).texture==gpu.get());CHECK(r->resolved_on_host&&r->texture==gpu);}
  const u32 x=13,y=partial?5:23,c=2;
  const u32 logical=tiled?rex::graphics::texture_util::GetTiledOffset2D(x,y,128,2):y*512+x*4;
  const u32 mask=endian==1?1:endian==2?3:endian==3?2:0;
  const u32 physical=(r->guest_fetch.base_address<<12)+((logical+c)^mask);
  const u32 address=alias==0?0xA0000000+physical:alias==1?0xC0000000+physical:0xE0000000+physical-4096;
  if(alias==3){const PhysicalCopyRange span{physical,1};auto write=OpenPhysicalWriteback(&sdk_memory,{&span,1});CHECK(write);
   *sdk_memory.TranslatePhysical<u8*>(physical)^=0x80;InvalidateCpuPhysicalMemory(physical,1);
  }else *sdk_memory.TranslateVirtual<volatile u8*>(address)^=0x80;
  expected[physical]^=0x80;
  CHECK(!CpuMemoryUnchanged(r->cpu_stamp));CHECK(CpuMemoryUnchanged(neighbor.resource->cpu_stamp)==watch);
  const auto uploaded=uploads;
  if(path==1){CHECK(!PrepareCachedTexture(snapshot).texture);CHECK(!r->resolved_on_host&&!r->guest_uploaded&&!r->sampled_valid);}
  commands->begin();open=true;
  if(path==0){TextureResourceView view;CHECK(PrepareTextureResource(r->guest_address,commands.get(),false,view)==TexturePreparationResult::kReady);}
  else CHECK(ResolveTextureResource(r->guest_address).texture);
  CHECK(uploads==uploaded+1&&!r->resolved_on_host&&r->guest_uploaded&&r->texture!=gpu);
  const TextureReadbackRequest request{r->texture.get(),r->width,r->height,TextureReadbackFormat::kRgba8};
  std::vector<std::vector<u8>> pixels;
  Recorder([&]{CHECK(ReadbackTexturesRaw(device.get(),commands.get(),{&request,1},pixels,
    []{return HostDevice::Synchronize(SyncReason::kResource);},[](auto buffer){HostDevice::RetireResource(std::move(buffer));}));});
  for(u32 py=0;py<r->height;++py)for(u32 px=0;px<r->width;++px)for(u32 pc=0;pc<4;++pc){
   const u32 at=tiled?rex::graphics::texture_util::GetTiledOffset2D(px,py,128,2):py*512+px*4;
   CHECK(pixels[0][(py*r->width+px)*4+pc]==expected[(r->guest_fetch.base_address<<12)+((at+pc)^mask)]);
  }
  // A failed upload must not escape through the direct-view path as a stale SRV.
  fail_upload=true;CHECK(!ResolveTextureResource(r->guest_address).texture);fail_upload=false;
  if(open)Finish();
  retained.clear();++cases;
 }
 ShutdownCpuMemoryWatch();g_textures.clear();
 std::printf("PASS %u real SDK A/C/E + notified-host CPU-write-to-GPU transactions; watch=%u; upload/cache/direct-view paths, clean neighbor and failed upload\n",cases,watch);
}
