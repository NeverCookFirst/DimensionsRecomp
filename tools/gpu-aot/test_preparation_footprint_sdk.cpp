#define NOMINMAX
#include "gpu_native/d3d.h"
#include "gpu_native/guest_metadata.h"
#include "gpu_native/preparation_footprint.h"
#include "gpu_native/texture_endian.h"
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/system/memory_data_provider.h>
#include <windows.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <unordered_map>
#include <future>
#include <thread>
#include <chrono>
using namespace legodimensions::gpu_native;
#define CHECK(x) do{if(!(x)){std::fprintf(stderr,"line %u: %s\n",__LINE__,#x);std::exit(23);}}while(0)
rex::memory::Memory* memory_pointer;
#define REX_KERNEL_MEMORY() memory_pointer
struct TextureResource {
 u32 guest_address=0,mirror_address=0,mirror_size=0,width=64,height=32,depth=1,levels=2,guest_format=6,adopted_header_type=3;
 bool surface=false,owns_guest_memory=false;
 mutable bool layout_cached=false;mutable rex::graphics::xenos::xe_gpu_texture_fetch_t layout_fetch{};
 mutable u32 layout_key[4]{},layout_base_bytes=0,layout_mips_bytes=0;
 std::atomic<bool>replaced=false;std::mutex mutex;
 rex::graphics::xenos::xe_gpu_texture_fetch_t guest_fetch{};
};
struct TexturePreparationSnapshot {std::shared_ptr<void>owner;std::array<u32,7>header{};TexturePreparationFootprint footprint;bool alias_settled=false;};
struct CaptureTemplate{struct {std::array<TexturePreparationSnapshot,kNativeTextureSlots>texture_inputs;}draw;};
std::unordered_map<u32,std::shared_ptr<TextureResource>>resources;
auto FindTexture(u32 address){auto i=resources.find(address);return i==resources.end()?std::shared_ptr<TextureResource>{}:i->second;}
bool DepthAliasesEnabled(){return false;}
void*NativeResourceCommands(void*){return nullptr;}
bool SettleDepthAlias(u32,TextureResource&,void*,bool&){CHECK(false);return false;}
#include "footprint-functions.inc"
struct Provider {
 std::atomic<unsigned>queries=0,provides=0;
 static bool Call(std::unique_lock<std::recursive_mutex>&lock,void*context,u32,rex::memory::PhysicalProviderOperation operation){
  CHECK(lock.owns_lock());auto&s=*static_cast<Provider*>(context);if(operation==rex::memory::PhysicalProviderOperation::kQuery)++s.queries;else ++s.provides;return false;
 }
};
void WriteHeader(TextureResource&resource){
 D3DTexture header{};header.resource.common=resource.adopted_header_type;
 const auto*words=reinterpret_cast<const u32*>(&resource.guest_fetch);for(unsigned i=0;i<6;++i)header.format.dword[i]=words[i];
 std::memcpy(REX_KERNEL_MEMORY()->TranslatePhysical(REX_KERNEL_MEMORY()->GetPhysicalAddress(resource.guest_address)),&header,sizeof(header));
}
int main(){
 SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
 char loaded[MAX_PATH]{};CHECK(GetModuleFileNameA(GetModuleHandleA("rexruntime.dll"),loaded,MAX_PATH));CHECK(!_stricmp(loaded,std::getenv("LEGO_TEST_RUNTIME_EXPECTED")));
 rex::memory::Memory memory;CHECK(memory.Initialize());memory_pointer=&memory;
 CHECK(memory.LookupHeap(0xC0000000)->Protect(0xC0020000,0x100000,rex::memory::kMemoryProtectRead|rex::memory::kMemoryProtectWrite));
 auto module=GetModuleHandleA("rexruntime.dll");
 auto add=reinterpret_cast<rex::memory::RegisterPhysicalDataProvider>(GetProcAddress(module,"rex_memory_register_data_provider_v1"));
 auto remove=reinterpret_cast<rex::memory::UnregisterPhysicalDataProvider>(GetProcAddress(module,"rex_memory_unregister_data_provider_v1"));CHECK(add&&remove);
 Provider provider;auto registration=add(&memory,Provider::Call,&provider);CHECK(registration);
 unsigned tested=0;
 for(u32 alias:{0xA0020FF0u,0xC0020FF0u,0xE001FFF0u})for(bool ef:{false,true})for(bool tiled:{false,true}){
  auto resource=std::make_shared<TextureResource>();resource->guest_address=alias;auto&fetch=resource->guest_fetch;
  fetch.type=decltype(fetch.type)(2);fetch.dimension=rex::graphics::xenos::DataDimension::k2DOrStacked;fetch.format=rex::graphics::xenos::TextureFormat::k_8_8_8_8;
  fetch.pitch=2;fetch.tiled=tiled;fetch.base_address=(ef?0xE005F000u:0x60000u)>>12;fetch.mip_address=0x80000>>12;
  reinterpret_cast<u32*>(&fetch)[2]=(resource->height-1)<<13|(resource->width-1);resources[alias]=resource;WriteHeader(*resource);
  memory.EnablePhysicalMemoryDataProviders(0x20000,8192);
  // Arming protection can query providers itself; count only consumers below.
  provider.queries=provider.provides=0;
  const auto snapshot=CaptureTexturePreparation(alias,{});CHECK(snapshot.owner==resource&&!snapshot.footprint.unknown);
  const auto footprint=CaptureTexturePreparationFootprint(alias);CHECK(!footprint.unknown&&footprint.ranges[0].address==0x20FF0&&footprint.ranges[0].length==52);
  CHECK(footprint.ranges[1].address==0x60000&&footprint.ranges[2].address==0x80000&&footprint.ranges[2].length>0);
  const auto layout=rex::graphics::texture_util::GetGuestTextureLayout(fetch.dimension,fetch.pitch,64,32,1,tiled,fetch.format,fetch.packed_mips,true,1);
  CHECK(footprint.ranges[1].length==layout.base.level_data_extent_bytes&&footprint.ranges[2].length==layout.mips_total_extent_bytes);
  if(!tested){
   std::promise<void>held,release;auto ready=release.get_future();
   std::thread uploader([&]{std::lock_guard lock(resource->mutex);held.set_value();ready.wait();});held.get_future().wait();
   auto capture=std::async(std::launch::async,[&]{return CaptureTexturePreparationFootprint(alias);});
   CHECK(capture.wait_for(std::chrono::milliseconds(250))==std::future_status::ready&&capture.get().unknown);
   release.set_value();uploader.join();
  }
  CaptureTemplate previous;previous.draw.texture_inputs[0]=snapshot;
  CHECK(TemplateHeadersRepeat(previous,{alias,0}));if(provider.queries)std::fprintf(stderr,"alias=%08X ef=%d tiled=%d queries=%u\n",alias,ef,tiled,provider.queries.load());CHECK(provider.queries==0&&provider.provides==0);
  // Resolve parameters share the protected metadata page; copied words retain signed/endian values.
  std::array<be_u32,4>words{};words[0]=0xFFFFFFFF;words[1]=123;words[2]=0x80000000;words[3]=456;
  std::memcpy(memory.TranslatePhysical(0x20F00),words.data(),sizeof(words));i32 output[4]{};
  memory.EnablePhysicalMemoryDataProviders(0x20000,8192);CHECK(ReadResolveWords(alias-0xF0,output,4));
  CHECK(output[0]==-1&&output[1]==123&&output[2]==INT32_MIN&&output[3]==456&&provider.queries==0);
  CHECK(!ReadResolveWords(alias,output,0)&&!ReadResolveWords(alias,output,5)&&!ReadResolveWords(0xFFFFFFFC,output,2));
  // Relocation after capture must not reuse the old resource's ranges as a current-placement proof.
  auto changed=fetch;++changed.base_address;D3DTexture header{};header.resource.common=3;const auto*changed_words=reinterpret_cast<const u32*>(&changed);
  for(unsigned i=0;i<6;++i)header.format.dword[i]=changed_words[i];std::memcpy(memory.TranslatePhysical(0x20FF0),&header,sizeof(header));
  CHECK(CaptureTexturePreparationFootprint(alias).unknown&&!TemplateHeadersRepeat(previous,{alias,0}));
  resource->replaced=true;CHECK(CaptureTexturePreparationFootprint(alias).unknown);resource->replaced=false;WriteHeader(*resource);
  memory.DisablePhysicalMemoryDataProviders(0x20000,8192);++tested;
 }
 {auto resource=std::make_shared<TextureResource>();resource->guest_address=0xA0022000;resources[resource->guest_address]=resource;
  resource->surface=true;auto footprint=CaptureTexturePreparationFootprint(resource->guest_address);CHECK(!footprint.unknown&&footprint.ranges[0].length==52&&!footprint.ranges[1].length);
  resource->surface=false;resource->owns_guest_memory=true;resource->mirror_address=0xA0090000;resource->mirror_size=4096;
  footprint=CaptureTexturePreparationFootprint(resource->guest_address);CHECK(!footprint.unknown&&footprint.ranges[1].address==0x90000&&footprint.ranges[1].length==4096);}
 // Explicit inaccessible aliases must not acquire guessed footprints or retained captures.
 auto resource=resources.begin()->second;const auto address=resource->guest_address;
 CHECK(memory.LookupHeap(address)->Protect(address&~4095u,4096,rex::memory::kMemoryProtectNoAccess));
 CHECK(CaptureTexturePreparationFootprint(address).unknown);auto unavailable=CaptureTexturePreparation(address,{});CHECK(!unavailable.owner&&unavailable.footprint.unknown);
 CHECK(CaptureTexturePreparationFootprint(0xA0110000).unknown);CHECK(remove(&memory,registration));
 std::printf("PASS %u real SDK footprint cases: A/C/E headers, E/F pixel bias, linear/tiled base+mips, protected cross-page metadata, repeated headers/resolve words, changed/retired/unreadable placements; no game/GPU.\n",tested);
}
