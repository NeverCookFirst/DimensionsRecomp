#define NOMINMAX
#include <rex/system/xmemory.h>
#include <rex/system/memory_data_provider.h>
#include "gpu_native/physical_write_scope.h"
#include <windows.h>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <future>
#include <thread>
#include <vector>
using namespace rex::memory;
using namespace legodimensions::gpu_native;
#define CHECK(x) do{if(!(x)){fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x);std::exit(23);}}while(0)
RegisterPhysicalDataProvider register_provider;
UnregisterPhysicalDataProvider unregister_provider;
DWORD Protection(void* at){MEMORY_BASIC_INFORMATION info{};CHECK(VirtualQuery(at,&info,sizeof(info)));return info.Protect&255;}
struct Client {
 Memory& memory;uint32_t page,offset,length;uint8_t value;
 std::atomic<bool> pending=false;
 std::atomic<unsigned> queries=0,provides=0;
 bool block=false,self_unregister=false,fail=false,stuck=false,mutate_from_query=false;
 void* handle=nullptr;void* other_handle=nullptr;
 std::mutex mutex;std::condition_variable condition;bool entered=false,released=false;
 Client(Memory& m,uint32_t p,uint32_t o,uint32_t n,uint8_t v):memory(m),page(p),offset(o),length(n),value(v){}
 void Arm(){pending=true;memory.EnablePhysicalMemoryDataProviders(page,4096);}
 static bool Call(std::unique_lock<std::recursive_mutex>& lock,void* context,uint32_t physical,PhysicalProviderOperation operation){
  auto& c=*static_cast<Client*>(context);CHECK(lock.owns_lock());
  if(operation==PhysicalProviderOperation::kQuery){
   ++c.queries;
   if(c.mutate_from_query){CHECK(!register_provider(&c.memory,Client::Call,&c));CHECK(!unregister_provider(&c.memory,c.other_handle));}
   return c.pending&&(physical&~4095u)==c.page;
  }
  CHECK(c.pending&&(physical&~4095u)==c.page);++c.provides;
  if(c.fail)return false;
  if(c.stuck)return true;
  if(c.self_unregister)CHECK(!unregister_provider(&c.memory,c.handle));
  if(c.block){
   lock.unlock();std::unique_lock wait(c.mutex);c.entered=true;c.condition.notify_all();
   c.condition.wait(wait,[&]{return c.released;});wait.unlock();lock.lock();
  }
  {const PhysicalCopyRange span{c.page+c.offset,c.length};auto scope=OpenPhysicalWriteback(&c.memory,{&span,1});CHECK(scope);
   std::memset(c.memory.TranslatePhysical(c.page+c.offset),c.value,c.length);}
  c.pending=false;c.memory.DisablePhysicalMemoryDataProviders(c.page,4096);return true;
 }
};
struct Legacy {
 Memory& memory;std::vector<Client*> clients;unsigned calls=0;uint32_t extra_disable=0;
 static bool Call(std::unique_lock<std::recursive_mutex>& lock,void* context,uint32_t physical){
  auto& c=*static_cast<Legacy*>(context);++c.calls;
  for(auto* client:c.clients)if(client->pending)Client::Call(lock,client,client->page,PhysicalProviderOperation::kProvide);
  if(c.extra_disable)c.memory.DisablePhysicalMemoryDataProviders(c.extra_disable,4096);
  c.memory.DisablePhysicalMemoryDataProviders(physical&~4095u,4096);return true;
 }
};
int main(int argc,char**argv){
 SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);setvbuf(stdout,nullptr,_IONBF,0);
 auto module=GetModuleHandleA("rexruntime.dll");CHECK(module);
 char loaded[MAX_PATH]{};CHECK(GetModuleFileNameA(module,loaded,MAX_PATH));
 CHECK(!_stricmp(loaded,std::getenv("LEGO_TEST_RUNTIME_EXPECTED")));
 register_provider=reinterpret_cast<RegisterPhysicalDataProvider>(GetProcAddress(module,"rex_memory_register_data_provider_v1"));
 unregister_provider=reinterpret_cast<UnregisterPhysicalDataProvider>(GetProcAddress(module,"rex_memory_unregister_data_provider_v1"));
 CHECK(register_provider&&unregister_provider);
 auto memory=std::make_unique<Memory>();CHECK(memory->Initialize());
 constexpr uint32_t physical=0x02000000,a=0xA2000000,c=0xC2000000,e=0xE1FFF000;
 auto* heap=memory->LookupHeap(a);CHECK(heap);
 CHECK(heap->AllocFixed(a,65536,65536,kMemoryAllocationReserve|kMemoryAllocationCommit,kMemoryProtectRead|kMemoryProtectWrite));
 std::memset(memory->TranslatePhysical(physical),0x55,65536);
 Client first(*memory,physical,0,256,0xA1),second(*memory,physical,256,256,0xB2),old(*memory,physical+8192,0,256,0xC3);
 Legacy legacy{*memory,{&old}};memory->SetPhysicalMemoryDataProvider(Legacy::Call,&legacy);
 if(argc>1){
  first.stuck=std::strcmp(argv[1],"stuck-provider")==0;first.fail=!first.stuck;
  first.handle=register_provider(memory.get(),Client::Call,&first);CHECK(first.handle);first.Arm();
  auto observed=*memory->TranslateVirtual<volatile uint8_t*>(a);
  printf("UNSAFE STALE READ %u\n",observed);return 23;
 }
 CHECK(!register_provider(nullptr,Client::Call,&first));CHECK(!register_provider(memory.get(),nullptr,&first));
 CHECK(!unregister_provider(memory.get(),nullptr)&&!unregister_provider(memory.get(),reinterpret_cast<void*>(999999)));
 first.handle=register_provider(memory.get(),Client::Call,&first);second.handle=register_provider(memory.get(),Client::Call,&second);
 CHECK(first.handle&&second.handle&&first.handle!=second.handle);
 first.other_handle=second.handle;first.mutate_from_query=true;
 for(uint32_t alias:{a,c,e}){
  first.Arm();second.Arm();old.Arm();const auto legacy_before=legacy.calls;
  CHECK(*memory->TranslateVirtual<volatile uint8_t*>(alias)==0xA1);
  CHECK(!first.pending&&!second.pending&&!old.pending&&legacy.calls==legacy_before+1);
  CHECK(*memory->TranslatePhysical<uint8_t*>(physical+256)==0xB2);
  CHECK(*memory->TranslatePhysical<uint8_t*>(old.page)==0xC3);
 }
 first.mutate_from_query=false;
 // Legacy replacement cannot replace the supplemental registrations. A
 // legacy retirement must not unprotect a still-owned supplemental page.
 Legacy replacement{*memory,{&old},0,physical};memory->SetPhysicalMemoryDataProvider(Legacy::Call,&replacement);
 first.Arm();second.Arm();old.Arm();const auto before_legacy=legacy.calls;
 CHECK(*memory->TranslateVirtual<volatile uint8_t*>(a+8192)==0xC3);
 CHECK(first.pending&&second.pending&&legacy.calls==before_legacy&&replacement.calls==1);
 CHECK(Protection(memory->TranslateVirtual(a))==PAGE_NOACCESS);
 CHECK(*memory->TranslateVirtual<volatile uint8_t*>(a)==0xA1&&!first.pending&&!second.pending);
 memory->SetPhysicalMemoryDataProvider(nullptr,nullptr);
 // Host physical reads work without a legacy provider and preserve disjoint
 // byte ownership within the same protected page.
 first.Arm();second.Arm();memory->ProvidePhysicalMemory(physical,4096);
 CHECK(!first.pending&&!second.pending&&*memory->TranslatePhysical<uint8_t*>(physical+256)==0xB2);
 // Read-only CPU views remain read-only after both owners publish their data.
 CHECK(heap->Protect(a,4096,kMemoryProtectRead));first.Arm();second.Arm();
 CHECK(*memory->TranslateVirtual<volatile uint8_t*>(e)==0xA1);
 CHECK(Protection(memory->TranslateVirtual(a))==PAGE_READONLY&&Protection(memory->TranslatePhysical(physical))==PAGE_READONLY);
 CHECK(heap->Protect(a,4096,kMemoryProtectRead|kMemoryProtectWrite));
 first.self_unregister=true;first.Arm();CHECK(*memory->TranslateVirtual<volatile uint8_t*>(a)==0xA1);first.self_unregister=false;
 const auto old_handle=first.handle;CHECK(unregister_provider(memory.get(),old_handle));CHECK(!unregister_provider(memory.get(),old_handle));
 first.handle=register_provider(memory.get(),Client::Call,&first);CHECK(first.handle&&first.handle!=old_handle);
 CHECK(!unregister_provider(memory.get(),old_handle));
 // A second owner may register while an earlier provider released the
 // critical region for its wait. A host physical read must re-poll the same
 // page after that callback; unlike guest reads, it cannot rely on a retry.
 first.block=true;first.Arm();
 auto host_reader=std::async(std::launch::async,[&]{memory->ProvidePhysicalMemory(physical,4096);});
 {std::unique_lock lock(first.mutex);CHECK(first.condition.wait_for(lock,std::chrono::seconds(5),[&]{return first.entered;}));}
 Client late(*memory,physical,768,256,0xD4);late.handle=register_provider(memory.get(),Client::Call,&late);CHECK(late.handle);late.Arm();
 {std::lock_guard lock(first.mutex);first.released=true;first.condition.notify_all();}
 host_reader.get();CHECK(!late.pending&&*memory->TranslatePhysical<uint8_t*>(physical+768)==0xD4);
 CHECK(unregister_provider(memory.get(),late.handle));
 first.entered=false;first.released=false;
 // Unregister waits for a callback which released the critical region, without
 // retaining that region or blocking the callback's final publication.
 first.block=true;first.Arm();
 auto reader=std::async(std::launch::async,[&]{return *memory->TranslateVirtual<volatile uint8_t*>(a);});
 {std::unique_lock lock(first.mutex);CHECK(first.condition.wait_for(lock,std::chrono::seconds(5),[&]{return first.entered;}));}
 auto retiring=std::async(std::launch::async,[&]{return unregister_provider(memory.get(),first.handle);});
 CHECK(retiring.wait_for(std::chrono::milliseconds(30))==std::future_status::timeout);
 {auto global=rex::thread::global_critical_region::AcquireDirect();}
 {std::lock_guard lock(first.mutex);first.released=true;first.condition.notify_all();}
 CHECK(reader.get()==0xA1&&retiring.get());CHECK(unregister_provider(memory.get(),second.handle));
 // Destructor cleanup also invalidates handles; stale tokens cannot remove a
 // later registration, even if an address is reused by the allocator.
 auto leftover=register_provider(memory.get(),Client::Call,&first);CHECK(leftover);auto* address=memory.get();memory.reset();
 CHECK(!unregister_provider(address,leftover));
 puts("PASS SDK provider chain: legacy preservation/replacement, shared-page ownership, A/C/E faults, host reads, read-only payloads, self-unregister rejection, stale handles and in-flight retirement");
}
