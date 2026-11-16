#define NOMINMAX
#include "gpu_native/guest_metadata.h"
#include "gpu_native/physical_write_scope.h"
#include <rex/system/memory_data_provider.h>
#include <rex/types.h>
#include <windows.h>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <span>
using namespace legodimensions::gpu_native;
#define CHECK(x) do{if(!(x)){std::fprintf(stderr,"line %u: %s\n",__LINE__,#x);std::exit(23);}}while(0)
#include "header-abi.inc"
struct TextureResource {bool surface=false,owns_guest_memory=false;uint32_t guest_address=0,adopted_header_type=3;std::array<uint32_t,6> guest_fetch{};};
rex::memory::Memory* fixture_memory=nullptr;
#define REX_KERNEL_MEMORY() (fixture_memory)
template<class T>T* GuestAt(uint32_t at){return fixture_memory->TranslateVirtual<T*>(at);}
#include "placement-production.inc"

DWORD Protection(void* pointer){MEMORY_BASIC_INFORMATION info{};CHECK(VirtualQuery(pointer,&info,sizeof(info)));return info.Protect&255;}
struct Provider {
 rex::memory::Memory& memory;
 TextureResource resource;
 uint32_t base=0,length=8192,calls=0;
 uint8_t value=0;
 bool pending=false;
 static bool Call(std::unique_lock<std::recursive_mutex>& global,void* context,uint32_t physical,rex::memory::PhysicalProviderOperation operation){
  auto& self=*static_cast<Provider*>(context);
  if(operation==rex::memory::PhysicalProviderOperation::kQuery)return self.pending&&physical>=self.base&&physical<self.base+self.length;
  static thread_local bool delivering=false;
  if(delivering){std::fputs("REENTERED_PROVIDER_DURING_PLACEMENT_HEADER_READ\n",stderr);std::exit(23);}
  delivering=true;++self.calls;
  CHECK(global.owns_lock());
  CHECK(Protection(self.memory.TranslateVirtual(self.resource.guest_address))==PAGE_NOACCESS);
  CHECK(TexturePlacementHeaderMatches(self.resource));
  CHECK(Protection(self.memory.TranslateVirtual(self.resource.guest_address))==PAGE_NOACCESS);
  const PhysicalCopyRange range{self.base,self.length};
  {auto writable=OpenPhysicalWriteback(&self.memory,{&range,1});CHECK(writable);
   *self.memory.TranslatePhysical<uint8_t*>(self.base+17)=self.value;}
  self.pending=false;self.memory.DisablePhysicalMemoryDataProviders(self.base,self.length);
  delivering=false;return true;
 }
};
int main(){
 SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
 char loaded[MAX_PATH]{};CHECK(GetModuleFileNameA(GetModuleHandleA("rexruntime.dll"),loaded,MAX_PATH));
 CHECK(!_stricmp(loaded,std::getenv("LEGO_TEST_RUNTIME_EXPECTED")));
 rex::memory::Memory memory;CHECK(memory.Initialize());fixture_memory=&memory;
 auto module=GetModuleHandleA("rexruntime.dll");
 auto add=reinterpret_cast<rex::memory::RegisterPhysicalDataProvider>(GetProcAddress(module,"rex_memory_register_data_provider_v1"));
 auto remove=reinterpret_cast<rex::memory::UnregisterPhysicalDataProvider>(GetProcAddress(module,"rex_memory_unregister_data_provider_v1"));CHECK(add&&remove);
 Provider provider{memory};auto registration=add(&memory,Provider::Call,&provider);CHECK(registration);
 auto* owner=memory.LookupHeap(0xC0000000);uint32_t cases=0;
 for(uint32_t header_alias:{1u,0u,2u})for(uint32_t reader=0;reader<3;++reader)
 for(bool ro:{false,true})for(uint32_t offset:{512u,4096u-52,4096u-20}){
  provider.base=0x20000+cases*0x4000;
  CHECK(owner->Protect(0xC0000000+provider.base,provider.length,rex::memory::kMemoryProtectRead|rex::memory::kMemoryProtectWrite));
  const uint32_t physical=provider.base+offset;
  const std::array<uint32_t,3> headers{0xA0000000+physical,0xC0000000+physical,0xE0000000+physical-4096};
  provider.resource.guest_address=headers[header_alias];provider.resource.guest_fetch={0x11,0xF4000052,0x001FE0FF,0xD10,0x200,0xF4100A00};
  D3DTexture header{};header.resource.common=3;
  for(uint32_t i=0;i<6;++i)header.format.dword[i]=provider.resource.guest_fetch[i];
  std::memcpy(memory.TranslatePhysical(physical),&header,sizeof(header));
  if(ro)CHECK(owner->Protect(0xC0000000+provider.base,provider.length,rex::memory::kMemoryProtectRead));
  provider.pending=true;provider.value=uint8_t(0x40+cases);provider.calls=0;
  memory.EnablePhysicalMemoryDataProviders(provider.base,provider.length);
  // Metadata reads leave ordinary guest protection in place, including E's
  // physical bias, empty aliases and a header crossing a protected page.
  D3DTexture copied{};
  CHECK(CopyNativeGuestMetadata(memory,headers[header_alias],&copied,sizeof(copied)));
  CHECK(!std::memcmp(&copied,&header,sizeof(header))&&provider.calls==0);
  ++provider.resource.guest_fetch[2];CHECK(!TexturePlacementHeaderMatches(provider.resource));--provider.resource.guest_fetch[2];
  CHECK(Protection(memory.TranslateVirtual(headers[header_alias]))==PAGE_NOACCESS);
  const auto read_address=reader==0?0xA0000000+provider.base+17:reader==1?0xC0000000+provider.base+17:0xE0000000+provider.base+17-4096;
  CHECK(*memory.TranslateVirtual<volatile uint8_t*>(read_address)==provider.value);
  CHECK(provider.calls==1&&!provider.pending);++cases;
 }
 CHECK(remove(&memory,registration));
 // Explicit alias metadata takes precedence over canonical physical backing.
 constexpr uint32_t a=0xA2800000;auto* heap=memory.LookupHeap(a);CHECK(heap);
 CHECK(heap->AllocFixed(a,65536,65536,rex::memory::kMemoryAllocationReserve|rex::memory::kMemoryAllocationCommit,
   rex::memory::kMemoryProtectRead|rex::memory::kMemoryProtectWrite));
 CHECK(heap->rex::memory::BaseHeap::Protect(a,65536,rex::memory::kMemoryProtectNoAccess));
 std::array<uint8_t,52> output{};output.fill(0xA5);const auto sentinel=output;
 CHECK(memory.GetPhysicalHeap()->IsRangeCommittedReadable(0x02800000,52));
 CHECK(!CopyNativeGuestMetadata(memory,a,output.data(),52)&&output==sentinel);
 CHECK(heap->rex::memory::BaseHeap::Protect(a,65536,rex::memory::kMemoryProtectRead|rex::memory::kMemoryProtectWrite));
 uint32_t virtual_at=0;auto* virtual_heap=memory.LookupHeap(0x10000000);CHECK(virtual_heap);
 CHECK(virtual_heap->Alloc(8192,4096,rex::memory::kMemoryAllocationReserve|rex::memory::kMemoryAllocationCommit,
   rex::memory::kMemoryProtectRead|rex::memory::kMemoryProtectWrite,false,&virtual_at));
 const uint32_t virtual_header=virtual_at+4096-20;
 std::memcpy(memory.TranslateVirtual(virtual_header),sentinel.data(),52);
 CHECK(CopyNativeGuestMetadata(memory,virtual_header,output.data(),52)&&output==sentinel);
 for(auto range:{std::array<uint32_t,2>{0,52},{0xFFFFFFF0,52},{0x60000000,52},{virtual_at,65537}}){
  output.fill(0xA5);CHECK(!CopyNativeGuestMetadata(memory,range[0],output.data(),range[1])&&output==sentinel);
 }
 std::printf("PASS %u real protected A/C/E placement-header cases; no provider reentry, unchanged protection, mismatch rejection, RO publication, explicit no-access and virtual/cross-page bounds\n",cases);
}
