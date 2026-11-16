// Real retained upload publication, native tapes and headless D3D12 readbacks.
// Input decoding and the frame-retirement queue are boundaries in this fixture.
#define NOMINMAX
#include "gpu_native/texture_upload_publication.h"
#include <plume_d3d12.h>
#include <d3d12sdklayers.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <vector>
using namespace legodimensions::gpu_native;
using u64=uint64_t;
// The runner instruments only actual viewport emissions in native_recording.cpp.
extern unsigned g_native_test_viewport_binds;
#define CHECK(c) do{if(!(c)){std::fprintf(stderr,"line %d: %s\n",__LINE__,#c);std::exit(23);}}while(0)
namespace plume {std::unique_ptr<RenderInterface> CreateD3D12Interface();}
struct TextureResource {
 std::weak_ptr<TextureUploadPublication> prepared_upload;
 std::shared_ptr<TextureUploadReplayState> upload_replay;
 bool guest_uploaded=false,upload_source_key_valid=false;
 u64 guest_content_hash=0,upload_source_key=0;int cpu_stamp=0;
};
TextureUploadPublicationCache g_texture_upload_publications(128ull*1024*1024);
std::vector<std::shared_ptr<void>>* retirement=nullptr;
struct HostDevice {
 static void RetireResource(std::shared_ptr<void> p){CHECK(retirement);retirement->push_back(std::move(p));}
};
#include "texture-publication-production.inc"
bool CachedDecoded(TextureResource* resource,plume::RenderCommandList* commands,u64 content_hash){
 const u64 source_key=content_hash;const bool source_key_valid=true;int cpu_stamp=int(content_hash);
#include "texture-publication-cache-hit.inc"
 return false;
}
int main(){
 ID3D12Debug* debug=nullptr;const bool debug_enabled=SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)));
 if(debug_enabled){debug->EnableDebugLayer();debug->Release();}
 auto api=plume::CreateD3D12Interface();auto device=api->createDevice();CHECK(device);
 auto* raw=static_cast<plume::D3D12Device*>(device.get())->d3d;
 auto queue=device->createCommandQueue(plume::RenderCommandListType::DIRECT);
 auto commands=queue->createCommandList();auto fence=device->createCommandFence();
 plume::RenderTextureDesc td;td.dimension=plume::RenderTextureDimension::TEXTURE_2D;
 td.width=td.height=td.depth=td.arraySize=td.mipLevels=1;
 td.format=plume::RenderFormat::R32_UINT;td.committed=true;
 td.multisampling.sampleCount=plume::RenderSampleCount::COUNT_1;
 auto target=device->createTexture(td);CHECK(target);
 auto* texture=static_cast<plume::D3D12Texture*>(target.get());CHECK(texture->d3d);
 auto desc=texture->d3d->GetDesc();D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};UINT64 bytes=0;
 raw->GetCopyableFootprints(&desc,0,1,0,&footprint,nullptr,nullptr,&bytes);
 auto readback=device->createBuffer(plume::RenderBufferDesc::ReadbackBuffer(bytes));CHECK(readback);
 unsigned checks=0,cache_hits=0;
 auto payload=[&](uint32_t value){
  std::shared_ptr<plume::RenderBuffer> upload=device->createBuffer(plume::RenderBufferDesc::UploadBuffer(256));CHECK(upload);
  auto* p=upload->map();CHECK(p);std::memset(p,0,256);std::memcpy(p,&value,4);upload->unmap();
  auto copy=CreateNativeDrawCommands();
  copy->barriers(plume::RenderBarrierStage::COPY,{target.get(),plume::RenderTextureLayout::COPY_DEST});
  copy->copyTextureRegion(plume::RenderTextureCopyLocation::Subresource(target.get(),0),
    plume::RenderTextureCopyLocation::PlacedFootprint(upload.get(),plume::RenderFormat::R32_UINT,1,1,1,64,0));
  copy->barriers(plume::RenderBarrierStage::GRAPHICS,{target.get(),plume::RenderTextureLayout::SHADER_READ});
  return std::pair{std::move(copy),std::move(upload)};
 };
 auto prepare=[&](TextureResource& r,uint32_t value,plume::RenderCommandList* record){
  if(CachedDecoded(&r,record,value)){++cache_hits;return;}
  auto [copy,upload]=payload(value);
  PublishPreparedTextureUpload(r,record,std::move(copy),std::move(upload),256);
  r.guest_uploaded=true;r.guest_content_hash=value;r.upload_source_key=value;r.upload_source_key_valid=true;
 };
 auto read=[&](NativeDrawCommands* tape,unsigned layers){
  commands->begin();ResetNativeReplayState(commands.get());
  auto bindings=CreateNativeDrawCommands();const plume::RenderViewport viewport{};
  bindings->setViewports(&viewport,1);bindings->Replay(commands.get());
  const auto before_binds=g_native_test_viewport_binds;
  auto middle=CreateNativeDrawCommands(),outer=CreateNativeDrawCommands();
  if(tape){
   if(layers==0)tape->Replay(commands.get());
   if(layers==1){tape->Replay(middle.get());middle->Replay(commands.get());}
   if(layers==2){tape->Replay(outer.get());outer->Replay(middle.get());middle->Replay(commands.get());}
  }
  bindings->Replay(commands.get());CHECK(g_native_test_viewport_binds==before_binds);
  commands->barriers(plume::RenderBarrierStage::COPY,{target.get(),plume::RenderTextureLayout::COPY_SOURCE});
  D3D12_TEXTURE_COPY_LOCATION src{},dst{};src.pResource=texture->d3d;src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  dst.pResource=static_cast<plume::D3D12Buffer*>(readback.get())->d3d;dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;dst.PlacedFootprint=footprint;
  static_cast<plume::D3D12CommandList*>(commands.get())->d3d->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
  commands->barriers(plume::RenderBarrierStage::GRAPHICS,{target.get(),plume::RenderTextureLayout::SHADER_READ});
  commands->end();if(tape)tape->Reset();middle->Reset();outer->Reset();
  queue->executeCommandLists(commands.get(),fence.get());queue->waitForCommandFence(fence.get());
  uint32_t result=0;auto* p=readback->map();CHECK(p);std::memcpy(&result,p,4);readback->unmap();++checks;return result;
 };
 for(unsigned layers=0;layers<3;++layers){
  std::array<unsigned,3> order{0,1,2};
  do{
   TextureResource r;std::array<std::vector<std::shared_ptr<void>>,3> leases;
   std::array<std::unique_ptr<NativeDrawCommands>,3> tapes;
   for(auto& t:tapes)t=CreateNativeDrawCommands();
   // Establish deterministic old GPU bytes without publishing the new version.
   std::vector<std::shared_ptr<void>> initial;retirement=&initial;
   auto seed=CreateNativeDrawCommands();prepare(r,99,seed.get());CHECK(!r.upload_replay->version);CHECK(read(seed.get(),layers)==99);
   const auto before=r.upload_replay->version;CHECK(before);
   for(unsigned i:order){retirement=&leases[i];prepare(r,i==1?22:11,tapes[i].get());}
   CHECK(r.upload_replay->version==before); // no preparation-time publication
   for(unsigned i=0;i<3;++i){CHECK(read(tapes[i].get(),layers)==(i==1?22u:11u));leases[i].clear();}
  }while(std::next_permutation(order.begin(),order.end()));
  // An abandoned producer tape must not strand an earlier cache-hit consumer.
  TextureResource r;std::vector<std::shared_ptr<void>> later,earlier;auto a=CreateNativeDrawCommands(),b=CreateNativeDrawCommands();
  retirement=&later;prepare(r,33,b.get());b->Reset();later.clear();
  retirement=&earlier;prepare(r,33,a.get());CHECK(read(a.get(),layers)==33);earlier.clear();
  // GPU host writes invalidate the upload version at their own ordered point.
  retirement=&earlier;prepare(r,44,a.get());CHECK(read(a.get(),layers)==44);
  auto [host_copy,host_upload]=payload(55);host_copy->Replay(a.get());
  InvalidateTextureUploadReplay(a.get(),r.upload_replay);
  prepare(r,44,a.get());CHECK(read(a.get(),layers)==44);
  // Eviction expires the weak cache but queued work/fence owners survive it.
  retirement=&earlier;prepare(r,66,a.get());auto weak=r.prepared_upload;
  g_texture_upload_publications.Reset();CHECK(!weak.expired());
  CHECK(read(a.get(),layers)==66);earlier.clear();CHECK(weak.expired());
  prepare(r,66,a.get());CHECK(read(a.get(),layers)==66);earlier.clear();
 }
 // The cache owns no more than its byte budget, and reset releases idle data.
 TextureUploadPublicationCache bounded_cache(256);std::vector<std::shared_ptr<void>> leases;retirement=&leases;
 TextureResource r;auto t=CreateNativeDrawCommands();prepare(r,77,t.get());auto p=r.prepared_upload.lock();CHECK(p);
 bounded_cache.Keep(p);CHECK(bounded_cache.Bytes()==256);bounded_cache.Keep(p);CHECK(bounded_cache.Bytes()==256);
 auto [other_copy,other_upload]=payload(88);
 auto other=std::make_shared<TextureUploadPublication>(std::make_shared<TextureUploadReplayState>(),std::move(other_copy),std::move(other_upload),256);
 bounded_cache.Keep(other);CHECK(bounded_cache.Bytes()==256&&!bounded_cache.Touch(p));
 CHECK(read(t.get(),2)==77);bounded_cache.Reset();g_texture_upload_publications.Reset();p.reset();leases.clear();
 // Unrestricted callbacks still invalidate state: their caller may change any binding.
 commands->begin();ResetNativeReplayState(commands.get());
 auto external=CreateNativeDrawCommands();const plume::RenderViewport viewport{};
 const auto before_external=g_native_test_viewport_binds;
 external->setViewports(&viewport,1);external->External([](auto*){});
 external->setViewports(&viewport,1);external->Replay(commands.get());
 CHECK(g_native_test_viewport_binds==before_external+2);
 commands->end();queue->executeCommandLists(commands.get(),fence.get());queue->waitForCommandFence(fence.get());
 ID3D12InfoQueue* info=nullptr;const bool has_info=SUCCEEDED(raw->QueryInterface(IID_PPV_ARGS(&info)));unsigned errors=0;
 for(UINT64 i=0;has_info&&i<info->GetNumStoredMessages();++i){SIZE_T n=0;info->GetMessage(i,nullptr,&n);std::vector<uint8_t> b(n);
  auto* m=reinterpret_cast<D3D12_MESSAGE*>(b.data());info->GetMessage(i,m,&n);
  if(m->Severity<=D3D12_MESSAGE_SEVERITY_ERROR){++errors;std::fprintf(stderr,"D3D12 %u: %s\n",m->ID,m->pDescription);}}
 if(info)info->Release();CHECK(errors==0&&raw->GetDeviceRemovedReason()==S_OK);CHECK(cache_hits);
 std::printf("PASS %u GPU readbacks and retained-binding checks, %u actual decoded-cache hits: six preparation permutations, A/B/A, three tape depths, abandoned producer, GPU overwrite, bounded/fenced eviction and conservative external invalidation; debug_layer=%u device_removed=0\n",checks,cache_hits,unsigned(debug_enabled));
}
