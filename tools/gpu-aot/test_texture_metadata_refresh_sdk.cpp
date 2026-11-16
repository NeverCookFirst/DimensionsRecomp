#define NOMINMAX
#include "gpu_native/guest_metadata.h"
#include "gpu_native/physical_write_scope.h"
#include "gpu_native/resource_registry.h"
#include <rex/system/memory_data_provider.h>
#include <rex/graphics/xenos.h>
#include <rex/types.h>
#include <windows.h>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
using namespace legodimensions::gpu_native;
#define CHECK(x) do{if(!(x)){std::fprintf(stderr,"line %u: %s\n",__LINE__,#x);std::exit(23);}}while(0)
#define REXLOG_INFO(...) ((void)0)
#include "header-abi.inc"
namespace plume {
enum class RenderFormat {UNKNOWN,R8G8B8A8_UNORM};
enum class RenderSwizzle {R,G,B,A,ZERO,ONE};
enum class RenderTextureViewDimension {TEXTURE_2D,TEXTURE_3D,TEXTURE_CUBE};
struct RenderTextureViewDesc{int componentMapping=0;RenderFormat format{};u32 mipLevels=1,mipSlice=0;RenderTextureViewDimension dimension{};};
int RenderComponentMapping(RenderSwizzle,RenderSwizzle,RenderSwizzle,RenderSwizzle){return 0;}
struct RenderTextureView{};
struct RenderTexture{void setName(const char*){}std::unique_ptr<RenderTextureView>createTextureView(const RenderTextureViewDesc&){return std::make_unique<RenderTextureView>();}};
}
struct TextureResource {
 bool surface=false,owns_guest_memory=false,resolved_on_host=true,guest_backing_current=true;
 bool guest_uploaded=true,upload_source_key_valid=true;
 u32 guest_address=0,adopted_header_type=3,guest_format=0,d3d_type=3,width=0,height=0,depth=1,levels=1;
 u32 cpu_stamp=11,depth_alias_generation=12,descriptor_index=4,resolved_descriptor_index=5,sampled_descriptor_index=6,mirror_address=0;
 std::atomic<u32>header_sequence{0};std::atomic<bool>replaced{false};std::mutex mutex;
 rex::graphics::xenos::xe_gpu_texture_fetch_t guest_fetch{};
 plume::RenderFormat format{};
 std::unique_ptr<plume::RenderTexture>texture;
 std::unique_ptr<plume::RenderTextureView>view,resolved_view;
 std::vector<u32>previous_surface_descriptors;
};
struct TexturePreparationSnapshot{std::shared_ptr<void>owner;std::array<u32,7>header{};bool alias_settled=false;};
rex::memory::Memory*fixture_memory=nullptr;
#define REX_KERNEL_MEMORY() (fixture_memory)
template<class T>T*GuestAt(u32 at){return fixture_memory->TranslateVirtual<T*>(at);}
ResourceRegistry<TextureResource>g_textures;std::mutex g_textures_mutex;
constexpr int kPinTexture=0;
std::shared_ptr<void>FindPinnedOwner(int,u32){return {};}
std::recursive_mutex recording;
struct HostDevice {
 static std::unique_lock<std::recursive_mutex>LockRecording(){return std::unique_lock(recording);}
 static bool IsReady(){return true;}
 static void UnregisterTexture(u32){}
 static void RetireResource(std::shared_ptr<TextureResource>){}
 static u32 RegisterTexture(plume::RenderTexture*,plume::RenderTextureView*){return 9;}
};
plume::RenderFormat ConvertXenosTextureFormat(u32){return plume::RenderFormat::R8G8B8A8_UNORM;}
std::unique_ptr<plume::RenderTexture>CreateHostTexture(TextureResource&,u32,u32){return std::make_unique<plume::RenderTexture>();}
u32 NativeTextureSwizzle(u32,u32 swizzle){return swizzle;}
u32 NativeColorResolveSwizzle(u32 swizzle){return swizzle;}
void NoteTextureFootprintChange(u32){}
void InvalidateFramebufferReferences(u32){}
bool LongProbeEnabled(){return false;}
template<class...T>void LongProbeEvent(T&&...){}
template<class T>int LongProbeHex(const T&){return 0;}
bool DepthAliasesEnabled(){return false;}
void*NativeResourceCommands(void*){return nullptr;}
bool SettleDepthAlias(u32,TextureResource&,void*,bool&){CHECK(false);return false;}
bool RefreshTextureResourceHeader(u32,const std::shared_ptr<TextureResource>&);
void RefreshTextureHeader(u32);
std::shared_ptr<void>RefreshTextureHeaderOwner(u32);
#include "metadata-production.inc"

DWORD Protection(void*p){MEMORY_BASIC_INFORMATION info{};CHECK(VirtualQuery(p,&info,sizeof(info)));return info.Protect&255;}
struct Provider {
 rex::memory::Memory&memory;std::shared_ptr<TextureResource>resource;
 u32 base=0,length=8192,calls=0,mode=0;u8 value=0;bool pending=false;
 std::array<u32,6>expected{};
 static bool Call(std::unique_lock<std::recursive_mutex>&global,void*context,u32 physical,rex::memory::PhysicalProviderOperation operation){
  auto&self=*static_cast<Provider*>(context);
  if(operation==rex::memory::PhysicalProviderOperation::kQuery)return self.pending&&physical>=self.base&&physical<self.base+self.length;
  static thread_local bool delivering=false;
  if(delivering){std::fputs("REENTERED_PROVIDER_DURING_TEXTURE_METADATA_REFRESH\n",stderr);std::exit(23);}
  delivering=true;++self.calls;CHECK(global.owns_lock());
  auto lock=HostDevice::LockRecording();const auto address=self.resource->guest_address;
  CHECK(Protection(self.memory.TranslateVirtual(address))==PAGE_NOACCESS);
  if(self.mode==0)CHECK(TexturePlacementHeaderMatches(*self.resource));
  if(self.mode==1)CHECK(RefreshTextureResourceHeader(address,self.resource));
  if(self.mode==2)CHECK(RefreshTextureHeaderOwner(address)==self.resource);
  if(self.mode==3)CHECK(FindTextureAtUse(address)==self.resource);
  if(self.mode==4){auto snapshot=CaptureTexturePreparation(address, self.resource);CHECK(snapshot.owner==self.resource&&snapshot.header[0]==3);
   for(u32 i=0;i<6;++i)CHECK(snapshot.header[i+1]==self.expected[i]);}
  if(self.mode==5){g_textures.erase(address);auto adopted=AdoptTexture(address);CHECK(adopted&&adopted->guest_address==address&&adopted->texture);
   CHECK(!std::memcmp(&adopted->guest_fetch,self.expected.data(),24));}
  CHECK(Protection(self.memory.TranslateVirtual(address))==PAGE_NOACCESS);
  const PhysicalCopyRange range{self.base,self.length};
  {auto writable=OpenPhysicalWriteback(&self.memory,{&range,1});CHECK(writable);*self.memory.TranslatePhysical<u8*>(self.base+17)=self.value;}
  self.pending=false;self.memory.DisablePhysicalMemoryDataProviders(self.base,self.length);
  delivering=false;return true;
 }
};
int main(int argc,char**argv){
 SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);CHECK(argc==2);
 char loaded[MAX_PATH]{};CHECK(GetModuleFileNameA(GetModuleHandleA("rexruntime.dll"),loaded,MAX_PATH));CHECK(!_stricmp(loaded,std::getenv("LEGO_TEST_RUNTIME_EXPECTED")));
 rex::memory::Memory memory;CHECK(memory.Initialize());fixture_memory=&memory;
 auto module=GetModuleHandleA("rexruntime.dll");
 auto add=reinterpret_cast<rex::memory::RegisterPhysicalDataProvider>(GetProcAddress(module,"rex_memory_register_data_provider_v1"));
 auto remove=reinterpret_cast<rex::memory::UnregisterPhysicalDataProvider>(GetProcAddress(module,"rex_memory_unregister_data_provider_v1"));CHECK(add&&remove);
 Provider provider{memory};provider.mode=std::atoi(argv[1]);CHECK(provider.mode<6);
 auto registration=add(&memory,Provider::Call,&provider);CHECK(registration);auto*heap=memory.LookupHeap(0xC0000000);u32 cases=0;
 for(u32 alias:{1u,0u,2u})for(u32 reader=0;reader<3;++reader)for(bool ro:{false,true})for(u32 offset:{512u,4096u-52,4096u-20}){
  provider.base=0x20000+cases*0x4000;CHECK(heap->Protect(0xC0000000+provider.base,provider.length,rex::memory::kMemoryProtectRead|rex::memory::kMemoryProtectWrite));
  const u32 physical=provider.base+offset;
  const std::array<u32,3>headers{0xA0000000+physical,0xC0000000+physical,0xE0000000+physical-4096};
  provider.resource=std::make_shared<TextureResource>();provider.resource->guest_address=headers[alias];
  provider.expected={0x11,0xF4000052,0x001FE0FF,0xD10,0x200,0xF4100A00};
  std::memcpy(&provider.resource->guest_fetch,provider.expected.data(),24);g_textures[headers[alias]]=provider.resource;
  D3DTexture header{};header.resource.common=3;for(u32 i=0;i<6;++i)header.format.dword[i]=provider.expected[i];
  std::memcpy(memory.TranslatePhysical(physical),&header,sizeof(header));
  if(ro)CHECK(heap->Protect(0xC0000000+provider.base,provider.length,rex::memory::kMemoryProtectRead));
  provider.pending=true;provider.calls=0;provider.value=u8(0x40+cases);memory.EnablePhysicalMemoryDataProviders(provider.base,provider.length);
  const auto read=reader==0?0xA0000000+provider.base+17:reader==1?0xC0000000+provider.base+17:0xE0000000+provider.base+17-4096;
  CHECK(*memory.TranslateVirtual<volatile u8*>(read)==provider.value);CHECK(provider.calls==1&&!provider.pending);g_textures.erase(headers[alias]);++cases;
 }
 CHECK(remove(&memory,registration));
 // Owned resources/surfaces do not have a borrowed D3DTexture header to read.
 auto owned=std::make_shared<TextureResource>();owned->guest_address=0x10000000;owned->owns_guest_memory=true;g_textures[owned->guest_address]=owned;
 CHECK(CaptureTexturePreparation(owned->guest_address,owned).owner==owned);owned->owns_guest_memory=false;owned->surface=true;
 CHECK(CaptureTexturePreparation(owned->guest_address,owned).owner==owned);g_textures.erase(owned->guest_address);
 std::printf("PASS mode %u: %u actual SDK protected A/C/E texture metadata paths, RO/cross-page headers; ordinary guest read triggers exactly one provider, unchanged protection; owned/surface capture skips borrowed metadata.\n",provider.mode,cases);
}
