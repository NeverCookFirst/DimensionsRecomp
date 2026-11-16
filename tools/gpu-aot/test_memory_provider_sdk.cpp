// Actual SDK faults and physical host-read requests; no game or graphics app.
#define NOMINMAX
#include <rex/system/xmemory.h>
#include <windows.h>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <thread>
#include <vector>
#ifdef LEGO_TEST_PHYSICAL_WRITE_SCOPE
#include "gpu_native/physical_write_scope.h"
#endif
#define CHECK(x) do{if(!(x)){std::fprintf(stderr,"line %d: %s\n",__LINE__,#x);std::exit(23);}}while(0)
struct Provider {
 rex::memory::Memory& memory;
 std::set<uint32_t> pending;
 std::vector<uint32_t> delivered;
 bool wait_unlocked=false;
 static bool Deliver(std::unique_lock<std::recursive_mutex>& lock,void* context,uint32_t physical){
  auto& self=*static_cast<Provider*>(context);CHECK(lock.owns_lock());
  const auto page=physical&~4095u;CHECK(self.pending.contains(page));
  if(std::getenv("LEGO_TEST_PROVIDER_TRACE"))std::printf("deliver %08X wait=%u\n",physical,self.wait_unlocked);
  if(self.wait_unlocked){
   lock.unlock();std::thread gpu_completion([&]{auto critical=rex::thread::global_critical_region::AcquireDirect();});
   gpu_completion.join();lock.lock();
  }
  {
#ifdef LEGO_TEST_PHYSICAL_WRITE_SCOPE
   const legodimensions::gpu_native::PhysicalCopyRange range{page,4096};
   auto scope=legodimensions::gpu_native::OpenPhysicalWriteback(&self.memory,{&range,1});CHECK(scope);
#endif
   std::memset(self.memory.TranslatePhysical(page),0xA7,4096);
  }
  self.pending.erase(page);self.delivered.push_back(page);
  self.memory.DisablePhysicalMemoryDataProviders(page,4096);return true;
 }
 void Arm(uint32_t page){pending.insert(page);memory.EnablePhysicalMemoryDataProviders(page,4096);}
 void Reset(){for(auto page:pending)memory.DisablePhysicalMemoryDataProviders(page,4096);pending.clear();delivered.clear();}
};
bool TryRead(volatile uint8_t* at,uint8_t* result){__try{*result=*at;return true;}__except(EXCEPTION_EXECUTE_HANDLER){return false;}}
bool TryWrite(volatile uint8_t* at){__try{*at=0xCE;return true;}__except(EXCEPTION_EXECUTE_HANDLER){return false;}}
int main(int argc,char**argv){
 SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
 std::setvbuf(stdout,nullptr,_IONBF,0);
 const bool readonly_probe=argc>1&&std::strcmp(argv[1],"readonly-probe")==0;
 const bool diagnostic=argc>1&&!readonly_probe;
 char loaded[MAX_PATH]{};CHECK(GetModuleFileNameA(GetModuleHandleA("rexruntime.dll"),loaded,MAX_PATH));
 CHECK(std::getenv("LEGO_TEST_RUNTIME_EXPECTED")&&!_stricmp(loaded,std::getenv("LEGO_TEST_RUNTIME_EXPECTED")));
 rex::memory::Memory memory;CHECK(memory.Initialize());Provider provider{memory};
 memory.SetPhysicalMemoryDataProvider(Provider::Deliver,&provider);
 constexpr uint32_t physical=0x02000000,a=0xA2000000,c=0xC2000000,e=0xE1FFF000;
 auto* heap=memory.LookupHeap(a);CHECK(heap);
 CHECK(heap->AllocFixed(a,65536,65536,rex::memory::kMemoryAllocationReserve|rex::memory::kMemoryAllocationCommit,
   rex::memory::kMemoryProtectRead|rex::memory::kMemoryProtectWrite));
 if(readonly_probe){
  CHECK(heap->Protect(a+0xD000,4096,rex::memory::kMemoryProtectRead));
  provider.Arm(physical+0xD000);uint8_t observed=0;
  MEMORY_BASIC_INFORMATION backing{};CHECK(VirtualQuery(memory.TranslatePhysical(physical+0xD000),&backing,sizeof(backing)));
  std::printf("physical_backing_protection=%08X\n",backing.Protect);
  std::puts("PROBE read-only backing: provider must write through the physical view");
  CHECK(TryRead(memory.TranslateVirtual<volatile uint8_t*>(a+0xD000),&observed)&&observed==0xA7);
  MEMORY_BASIC_INFORMATION restored{};CHECK(VirtualQuery(memory.TranslatePhysical(physical+0xD000),&restored,sizeof(restored)));
  CHECK((restored.Protect&255)==PAGE_READONLY);
  uint32_t guest_protect=0;CHECK(heap->QueryProtect(a+0xD000,&guest_protect));CHECK(guest_protect==rex::memory::kMemoryProtectRead);
  CHECK(VirtualQuery(memory.TranslateVirtual(a+0xD000),&restored,sizeof(restored)));
  CHECK((restored.Protect&255)==PAGE_READONLY);
  std::puts("PASS physical-only write window publishes provider bytes and restores host/guest read-only protection");
  return 0;
 }
 CHECK(memory.LookupHeap(0xC4000000)->AllocFixed(0xC4000000,0x1000000,0x1000000,
   rex::memory::kMemoryAllocationReserve|rex::memory::kMemoryAllocationCommit,
   rex::memory::kMemoryProtectRead|rex::memory::kMemoryProtectWrite));
 CHECK(memory.LookupHeap(0xE5FFF000)->AllocFixed(0xE5FFF000,65536,4096,
   rex::memory::kMemoryAllocationReserve|rex::memory::kMemoryAllocationCommit,
   rex::memory::kMemoryProtectRead|rex::memory::kMemoryProtectWrite));
 uint32_t faults=0,misses=0,cases=0;
 for(uint32_t base:{physical,0x04000000u,0x06000000u})
 for(uint32_t alias:{0xA0000000u+base,0xC0000000u+base,0xE0000000u+base-4096})
 for(uint32_t page=0;page<4;++page)for(bool wait:{false,true}){
  const uint32_t at=base+page*4096;std::memset(memory.TranslatePhysical(at),0x55,4096);
  if(std::getenv("LEGO_TEST_PROVIDER_TRACE"))std::printf("case base=%08X alias=%08X page=%u wait=%u\n",base,alias,page,wait);
  provider.wait_unlocked=wait;provider.Arm(at);
  uint8_t observed=0;const bool ok=TryRead(memory.TranslateVirtual<volatile uint8_t*>(alias+page*4096+19),&observed);
  const bool delivered=ok&&observed==0xA7&&provider.delivered==std::vector<uint32_t>{at};
  ++cases;if(delivered)++faults;else++misses;
  if(!diagnostic)CHECK(delivered);
  provider.Reset();
 }
 std::printf("guest_alias_cases=%u delivered_faults=%u stale_or_missing=%u\n",cases,faults,misses);
 // Host reads bypass fault protection, so a requested multi-page range must
 // ask every still-pending page provider, even when each callback delivers one.
 const uint32_t first=physical+4096,last=physical+3*4096;
 provider.wait_unlocked=true;std::memset(memory.TranslatePhysical(physical),0x55,65536);
 provider.Arm(first);provider.Arm(last);memory.ProvidePhysicalMemory(first+17,last-first+1);
 const bool all=provider.delivered==std::vector<uint32_t>{first,last}&&
   memory.TranslatePhysical<uint8_t*>(first)[0]==0xA7&&memory.TranslatePhysical<uint8_t*>(last)[0]==0xA7;
 std::printf("host_multi_page_delivered=%zu complete=%u\n",provider.delivered.size(),all);
 if(!diagnostic)CHECK(all);
 provider.Reset();
 if(diagnostic){CHECK(misses&& !all);memory.SetPhysicalMemoryDataProvider(nullptr,nullptr);
  std::puts("PASS baseline reproduction: unallocated alias and physical host-range delivery gaps");return 0;}
 // Boundary rounding must not poll or publish neighboring pending pages.
 for(uint32_t page:{physical,physical+4096,physical+8192})provider.Arm(page);
 memory.ProvidePhysicalMemory(physical+4096+1,4095);
 CHECK(provider.delivered==std::vector<uint32_t>{physical+4096});
 CHECK((provider.pending==std::set<uint32_t>{physical,physical+8192}));
 provider.delivered.clear();memory.ProvidePhysicalMemory(physical+4095,2);
 CHECK(provider.delivered==std::vector<uint32_t>{physical});
 provider.Reset();
 // Reject invalid physical ranges, including overflow, without callbacks.
 provider.Arm(physical);memory.ProvidePhysicalMemory(physical,0);
 memory.ProvidePhysicalMemory(0x1FFFFFF0,64);memory.ProvidePhysicalMemory(0xFFFFFFF0,64);
 CHECK(provider.delivered.empty());provider.Reset();
 // The provider must restore the existing write-watch protection after the
 // read, including empty aliases whose canonical allocation is writable.
 for(uint32_t alias:{a,c,e}){
  memory.EnablePhysicalMemoryAccessCallbacks(physical,4096,true,false);provider.Arm(physical);
  uint8_t value=0;CHECK(TryRead(memory.TranslateVirtual<volatile uint8_t*>(alias),&value)&&value==0xA7);
  MEMORY_BASIC_INFORMATION info{};CHECK(VirtualQuery(memory.TranslateVirtual(alias),&info,sizeof(info)));
  CHECK((info.Protect&255)==PAGE_READONLY);CHECK(TryWrite(memory.TranslateVirtual<volatile uint8_t*>(alias)));
  CHECK(memory.TranslatePhysical<uint8_t*>(physical)[0]==0xCE);provider.Reset();
 }
 // Do not reinterpret an explicitly inaccessible alias as an empty alias.
 CHECK(heap->Protect(a+0xF000,4096,rex::memory::kMemoryProtectNoAccess));
 provider.Arm(physical+0xF000);uint8_t value=0;
 // An actual forbidden guest access terminates via the SDK exception router
 // before this fixture's SEH handler. Inspect its authoritative host protection
 // and provider bit instead; do not turn that intentional fault into a read.
 CHECK(!static_cast<rex::memory::PhysicalHeap*>(heap)->IsDataProvided(a+0xF000));CHECK(provider.delivered.empty());
 provider.Reset();MEMORY_BASIC_INFORMATION info{};CHECK(VirtualQuery(memory.TranslateVirtual(a+0xF000),&info,sizeof(info)));
 CHECK((info.Protect&255)==PAGE_NOACCESS);
 CHECK(heap->Protect(a+0xF000,4096,rex::memory::kMemoryProtectRead|rex::memory::kMemoryProtectWrite));
 memory.SetPhysicalMemoryDataProvider(nullptr,nullptr);
 std::puts("PASS real SDK data providers on writable backing: all physical aliases, scoped host ranges, unlocked waits, watch restoration and explicit no-access; read-only payload delivery remains a separate failing probe");
}
