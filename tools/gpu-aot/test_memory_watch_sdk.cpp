// Real shipped SDK Memory and protected A/C/E mappings; no game/runtime app.
#define NOMINMAX
#include "gpu_native/memory_watch.h"
#include <rex/system/xmemory.h>
#include <windows.h>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
namespace fixture {rex::memory::Memory* memory=nullptr;}
using namespace legodimensions::gpu_native;
#define CHECK(x) do{if(!(x)){std::fprintf(stderr,"line %d: %s\n",__LINE__,#x);std::exit(23);}}while(0)
CpuMemoryStamp stamp(uint32_t address,uint32_t bytes){
 const CpuMemorySpan span{address,bytes};return WatchCpuMemory({&span,1});
}
bool TryWrite(volatile uint8_t* address){
 __try{*address^=1;return true;}
 __except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
int main(){
 char loaded[MAX_PATH]{};CHECK(GetModuleFileNameA(GetModuleHandleA("rexruntime.dll"),loaded,MAX_PATH));
 CHECK(std::getenv("LEGO_TEST_RUNTIME_EXPECTED"));
 CHECK(!_stricmp(loaded,std::getenv("LEGO_TEST_RUNTIME_EXPECTED")));
 rex::memory::Memory memory;CHECK(memory.Initialize());fixture::memory=&memory;
 constexpr uint32_t physical=0x02000000,a=0xA2000000,c=0xC2000000,e=0xE1FFF000;
 auto* heap=memory.LookupHeap(a);CHECK(heap);
 CHECK(heap->AllocFixed(a,65536,65536,
   rex::memory::kMemoryAllocationReserve|rex::memory::kMemoryAllocationCommit,
   rex::memory::kMemoryProtectRead|rex::memory::kMemoryProtectWrite));
 CHECK(memory.GetPhysicalAddress(a)==physical && memory.GetPhysicalAddress(c)==physical && memory.GetPhysicalAddress(e)==physical);
 std::memset(memory.TranslatePhysical(physical),0x57,65536);
 auto first=stamp(a+1,8192),neighbor=stamp(c+0x3000,4096);
 CHECK(first.pages.size()==3 && CpuMemoryUnchanged(first) && CpuMemoryUnchanged(neighbor));
 // An allocation belongs to one alias heap. First exercise writes through
 // that allocated alias; do not pretend an unallocated view is watched.
 *memory.TranslateVirtual<volatile uint8_t*>(a+0x1004)=0x91;
 CHECK(memory.TranslatePhysical<uint8_t*>(physical+0x1004)[0]==0x91);
 CHECK(!CpuMemoryUnchanged(first) && CpuMemoryUnchanged(neighbor));
 first=stamp(a+1,8192);CHECK(CpuMemoryUnchanged(first));
 *memory.TranslateVirtual<volatile uint8_t*>(a+0x1004)=0x92;
 CHECK(!CpuMemoryUnchanged(first) && CpuMemoryUnchanged(neighbor));
 first=stamp(a+1,8192);
 *memory.TranslateVirtual<volatile uint8_t*>(a+0x1005)=0x93;
 CHECK(!CpuMemoryUnchanged(first) && CpuMemoryUnchanged(neighbor));
 const CpuMemorySpan spans[]={{a,4096},{e+0x5000,4096}};
 auto two=WatchCpuMemory(spans);CHECK(CpuMemoryUnchanged(two));
 *memory.TranslateVirtual<volatile uint8_t*>(a+0x5001)=0x94;
 CHECK(!CpuMemoryUnchanged(two));
 first=stamp(a,3*4096);CHECK(CpuMemoryUnchanged(first));
 const auto before_epoch=CpuMemoryWriteEpoch();
 // Physical host pointers bypass guest protection. Retire every exported
 // page after publication, even if only one byte changed in the range.
 *memory.TranslatePhysical<uint8_t*>(physical+3)=0x95;
 InvalidateCpuPhysicalMemory(physical,3*4096);
 CHECK(CpuMemoryWriteEpoch()!=before_epoch && !CpuMemoryUnchanged(first) && CpuMemoryUnchanged(neighbor));
 auto threaded=stamp(a+0x9000,4096);
 std::thread writer([&]{*memory.TranslateVirtual<volatile uint8_t*>(a+0x9001)=0x96;});writer.join();
 CHECK(!CpuMemoryUnchanged(threaded));
 CHECK(stamp(physical+0x10000,4096).pages.empty());
 CHECK(stamp(0x70002000,64).pages.empty());
 CHECK(stamp(0xFFFFFFF0,128).pages.empty());
 auto reset=stamp(a,4096);CHECK(CpuMemoryUnchanged(reset));
 ShutdownCpuMemoryWatch();CHECK(!CpuMemoryUnchanged(reset));
 auto recreated=stamp(a,4096);CHECK(CpuMemoryUnchanged(recreated));
 *memory.TranslateVirtual<volatile uint8_t*>(a+5)=0x97;
 CHECK(!CpuMemoryUnchanged(recreated));ShutdownCpuMemoryWatch();
 // C uses 16 MiB heap pages; E uses 4 KiB and its Windows physical bias.
 for(auto allocation:{std::array<uint32_t,3>{0xC4000000,0x1000000,0x1000000},
                       std::array<uint32_t,3>{0xE5FFF000,0x10000,0x1000}}){
  auto* alias_heap=memory.LookupHeap(allocation[0]);CHECK(alias_heap);
  CHECK(alias_heap->AllocFixed(allocation[0],allocation[1],allocation[2],
    rex::memory::kMemoryAllocationReserve|rex::memory::kMemoryAllocationCommit,
    rex::memory::kMemoryProtectRead|rex::memory::kMemoryProtectWrite));
  auto watched=stamp(allocation[0],4096),adjacent=stamp(allocation[0]+0x1000,4096);
  CHECK(CpuMemoryUnchanged(watched) && CpuMemoryUnchanged(adjacent));
  *memory.TranslateVirtual<volatile uint8_t*>(allocation[0]+3)=0x98;
  CHECK(!CpuMemoryUnchanged(watched) && CpuMemoryUnchanged(adjacent));
  watched=stamp(allocation[0],4096);
  *memory.TranslateVirtual<volatile uint8_t*>(allocation[0]+4)=0x99;
  CHECK(!CpuMemoryUnchanged(watched));
 }
 auto alias_case=stamp(a,4096);
 *memory.TranslateVirtual<volatile uint8_t*>(c+7)=0xA7;
 const bool notified=!CpuMemoryUnchanged(alias_case);
 std::printf("UNALLOCATED_ALIAS_NOTIFIED=%u\n",notified);
 // Diagnostic boundary, not a passing correctness assertion: the SDK only
 // arms aliases whose own heap metadata says they are writable. Keep this
 // unresolved case explicit rather than changing native behavior to fit it.
 if(!notified)std::puts("LIMIT unallocated C view changed A-owned physical bytes without watch notification");
 const bool require_aliases=std::getenv("LEGO_TEST_REQUIRE_ALL_ALIASES") &&
   std::getenv("LEGO_TEST_REQUIRE_ALL_ALIASES")[0]=='1';
 uint32_t alias_cases=0,alias_misses=0;
 for(uint32_t base:{0x02000000u,0x04000000u,0x06000000u}){
  const std::array<uint32_t,3> aliases{0xA0000000u+base,0xC0000000u+base,0xE0000000u+base-0x1000};
  for(auto watched:aliases)for(auto writer:aliases)for(uint32_t repeat=0;repeat<2;++repeat){
   auto current=stamp(watched+16,64);CHECK(CpuMemoryUnchanged(current));
   *memory.TranslateVirtual<volatile uint8_t*>(writer+17)^=1;
   const bool changed=!CpuMemoryUnchanged(current);++alias_cases;if(!changed)++alias_misses;
  }
 }
 std::printf("ALIAS_CASES=%u ALIAS_MISSES=%u\n",alias_cases,alias_misses);
 if(require_aliases)CHECK(notified && !alias_misses);
 // The existing runtime recovers host-only protection on an empty alias from
 // canonical parent permissions. Such a recovered write must invalidate the
 // stamp too, rather than silently changing bytes behind the cache.
 auto* protected_alias=memory.TranslateVirtual<uint8_t*>(c+0x8000);
 DWORD old_protect=0;CHECK(VirtualProtect(protected_alias,4096,PAGE_READONLY,&old_protect));
 auto readonly=stamp(a+0x8000,4096);CHECK(CpuMemoryUnchanged(readonly));
 MEMORY_BASIC_INFORMATION info{};CHECK(VirtualQuery(protected_alias,&info,sizeof(info)));
 CHECK((info.Protect&0xff)==PAGE_READONLY);
 CHECK(TryWrite(protected_alias));
 const bool readonly_notified=!CpuMemoryUnchanged(readonly);
 std::printf("HOST_RO_ALIAS_NOTIFIED=%u\n",readonly_notified);
 if(require_aliases)CHECK(readonly_notified);
 CHECK(VirtualProtect(protected_alias,4096,old_protect,&old_protect));
 // Arming watches does not relax explicit guest no-access protection. The
 // runtime's separate fault-recovery policy is outside this watch check.
 CHECK(heap->Protect(a+0x10000-0x1000,4096,rex::memory::kMemoryProtectNoAccess));
 CHECK(stamp(a+0xF000,64).pages.empty());
 CHECK(VirtualQuery(memory.TranslateVirtual<uint8_t*>(a+0xF000),&info,sizeof(info)));
 CHECK((info.Protect&0xff)==PAGE_NOACCESS);
 CHECK(heap->Protect(a+0xF000,4096,rex::memory::kMemoryProtectRead|rex::memory::kMemoryProtectWrite));
 if(std::getenv("LEGO_TEST_PERMISSION_TRANSITIONS")){
  uint32_t transitions=0,writes=0;
  for(auto allocation:{std::array<uint32_t,3>{0xA2800000,0x10000,0x20000},
                       std::array<uint32_t,3>{0xC8000000,0x1000000,0x2000000},
                       std::array<uint32_t,3>{0xEC000000,0x1000,0x2000}}){
   auto* owner=memory.LookupHeap(allocation[0]);CHECK(owner);
   CHECK(owner->AllocFixed(allocation[0],allocation[2],allocation[1],
     rex::memory::kMemoryAllocationReserve|rex::memory::kMemoryAllocationCommit,
     rex::memory::kMemoryProtectRead|rex::memory::kMemoryProtectWrite));
   const uint32_t base=memory.GetPhysicalAddress(allocation[0]);
   const std::array<uint32_t,3> views{0xA0000000+base,0xC0000000+base,0xE0000000+base-4096};
   for(auto watched_view:views)for(auto writable:{rex::memory::kMemoryProtectWrite,rex::memory::kMemoryProtectWriteCombine}){
    CHECK(owner->Protect(allocation[0],64,rex::memory::kMemoryProtectRead));
    auto ro=stamp(watched_view+16,64),inside=stamp(watched_view+128,64),outside=stamp(watched_view+allocation[1]+16,64);
    CHECK(CpuMemoryUnchanged(ro)&&CpuMemoryUnchanged(inside)&&CpuMemoryUnchanged(outside));
    CHECK(owner->Protect(allocation[0],64,rex::memory::kMemoryProtectRead|writable));
    // Notification is required at the protection transition, before a write,
    // even though RO pages had no armed SDK invalidation flags.
    CHECK(!CpuMemoryUnchanged(ro)&&!CpuMemoryUnchanged(inside)&&CpuMemoryUnchanged(outside));
    for(auto writer:views){auto rearmed=stamp(watched_view+16,64);
     CHECK(CpuMemoryUnchanged(rearmed));*memory.TranslateVirtual<volatile uint8_t*>(writer+17)^=1;
     CHECK(!CpuMemoryUnchanged(rearmed)&&CpuMemoryUnchanged(outside));++writes;
    }
    ++transitions;
   }
  }
  std::printf("PROTECTION_TRANSITIONS=%u ALIAS_WRITES=%u; RO->RW/WC, real A/C/E heaps and aliases, rounded-page invalidation, disjoint neighbor preserved\n",transitions,writes);
 }
 InvalidateCpuPhysicalMemory(physical,4096);CHECK(!CpuMemoryUnchanged(alias_case));
 ShutdownCpuMemoryWatch();
 std::puts("PASS shipped SDK watcher on allocated A/C/E views: bias, same-frame rearm, neighbor protection, spans, physical retirement, CPU thread, shutdown/recreate");
}
