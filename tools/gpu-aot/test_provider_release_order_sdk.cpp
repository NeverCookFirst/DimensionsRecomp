#define NOMINMAX
#include "gpu_native/physical_write_scope.h"
#include <rex/system/memory_data_provider.h>
#include <rex/types.h>
#include <windows.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>
using namespace legodimensions::gpu_native;
#define CHECK(x) do{if(!(x)){std::fprintf(stderr,"line %u: %s\n",__LINE__,#x);std::exit(23);}}while(0)
using namespace std::chrono_literals;
struct Sync {
 std::mutex mutex;std::condition_variable changed;
 bool entered=false,armed=false,retired=false,read_done=false;
 void set(bool&flag){std::lock_guard lock(mutex);flag=true;changed.notify_all();}
 void wait(bool&flag){std::unique_lock lock(mutex);CHECK(changed.wait_for(lock,3s,[&]{return flag;}));}
};
struct State {
 rex::memory::Memory&memory;Sync sync;
 static constexpr u32 base=0x20000,bytes=4096,legacy_at=64,native_at=128;
 bool legacy_pending=false,native_pending=false,defer=false;
 std::atomic<unsigned>legacy_calls=0,native_calls=0;
 bool same_page(u32 physical)const{return physical>=base&&physical<base+bytes;}
 void publish(u32 at,u8 value){const PhysicalCopyRange range{base,bytes};auto write=OpenPhysicalWriteback(&memory,{&range,1});CHECK(write);*memory.TranslatePhysical<u8*>(base+at)=value;}
 static bool Legacy(std::unique_lock<std::recursive_mutex>&lock,void*context,u32 physical){
  auto&s=*static_cast<State*>(context);CHECK(lock.owns_lock());
  if(!s.same_page(physical)||!s.legacy_pending)return false;
  ++s.legacy_calls;s.publish(legacy_at,0x33);s.legacy_pending=false;
  s.memory.DisablePhysicalMemoryDataProviders(base,bytes);return true;
 }
 static bool Native(std::unique_lock<std::recursive_mutex>&lock,void*context,u32 physical,rex::memory::PhysicalProviderOperation operation){
  auto&s=*static_cast<State*>(context);CHECK(lock.owns_lock());
  if(operation==rex::memory::PhysicalProviderOperation::kQuery)return s.same_page(physical)&&s.native_pending;
  ++s.native_calls;CHECK(s.native_pending);
  // Model recorder/GPU work without keeping the SDK critical region locked.
  lock.unlock();s.sync.set(s.sync.entered);s.sync.wait(s.sync.armed);lock.lock();
  s.publish(native_at,0x22);s.native_pending=false;
  // The naive release is unsafe: the legacy writer was armed during the wait
  // and cannot be queried by the supplemental SDK ownership predicate.
  if(!s.defer)s.memory.DisablePhysicalMemoryDataProviders(base,bytes);
  s.sync.set(s.sync.retired);
  lock.unlock();s.sync.wait(s.sync.read_done);lock.lock();return true;
 }
};
int main(int argc,char**argv){
 SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);CHECK(argc==2);
 char loaded[MAX_PATH]{};CHECK(GetModuleFileNameA(GetModuleHandleA("rexruntime.dll"),loaded,MAX_PATH));CHECK(!_stricmp(loaded,std::getenv("LEGO_TEST_RUNTIME_EXPECTED")));
 rex::memory::Memory memory;CHECK(memory.Initialize());State state{memory};state.defer=std::atoi(argv[1])!=0;
 auto module=GetModuleHandleA("rexruntime.dll");
 auto add=reinterpret_cast<rex::memory::RegisterPhysicalDataProvider>(GetProcAddress(module,"rex_memory_register_data_provider_v1"));
 auto remove=reinterpret_cast<rex::memory::UnregisterPhysicalDataProvider>(GetProcAddress(module,"rex_memory_unregister_data_provider_v1"));CHECK(add&&remove);
 auto*heap=memory.LookupHeap(0xC0000000);CHECK(heap->Protect(0xC0000000+State::base,State::bytes,rex::memory::kMemoryProtectRead|rex::memory::kMemoryProtectWrite));
 state.publish(State::legacy_at,0x11);state.publish(State::native_at,0);
 memory.SetPhysicalMemoryDataProvider(State::Legacy,&state);auto registration=add(&memory,State::Native,&state);CHECK(registration);
 {auto lock=rex::thread::global_critical_region::AcquireDirect();state.native_pending=true;memory.EnablePhysicalMemoryDataProviders(State::base,State::bytes);}
 u8 native_value=0;
 std::thread reader([&]{native_value=*memory.TranslateVirtual<volatile u8*>(0xA0000000+State::base+State::native_at);});
 state.sync.wait(state.sync.entered);
 {auto lock=rex::thread::global_critical_region::AcquireDirect();state.legacy_pending=true;memory.EnablePhysicalMemoryDataProviders(State::base,State::bytes);}
 state.sync.set(state.sync.armed);state.sync.wait(state.sync.retired);
 // This read races the supplemental callback's return to the SDK. If it was
 // unprotected too early, no fault delivers the still-pending legacy byte.
 const u8 observed=*memory.TranslateVirtual<volatile u8*>(0xC0000000+State::base+State::legacy_at);
 const auto legacy_before_return=state.legacy_calls.load();
 state.sync.set(state.sync.read_done);reader.join();
 CHECK(native_value==0x22&&state.native_calls==1&&state.legacy_calls==1);
 CHECK(*memory.TranslatePhysical<u8*>(State::base+State::legacy_at)==0x33);
 if(state.defer){CHECK(observed==0x33&&legacy_before_return==1);std::puts("PASS deferred page release: concurrent legacy write delivered before CPU read; supplemental slot and legacy slot both retained.");}
 else{CHECK(observed==0x11&&legacy_before_return==0);std::puts("REPRODUCED early release exposes stale legacy byte 0x11 before pending 0x33; final publication later succeeds.");}
 CHECK(remove(&memory,registration));memory.SetPhysicalMemoryDataProvider(nullptr,nullptr);
}
