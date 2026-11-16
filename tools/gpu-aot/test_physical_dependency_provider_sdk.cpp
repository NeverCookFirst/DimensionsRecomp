#define NOMINMAX
#include "provider-under-test.h"
#include "gpu_native/guest_metadata.h"
#include "gpu_native/physical_write_scope.h"
#include <windows.h>
#include <atomic>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <thread>
using namespace legodimensions::gpu_native;
using namespace std::chrono_literals;
#define CHECK(x) do{if(!(x)){std::fprintf(stderr,"line %u: %s\n",__LINE__,#x);std::exit(23);}}while(0)
using Proof=PhysicalDependencyProvider::StopProof;
struct Event {
 std::mutex mutex;std::condition_variable cv;bool signaled=false;
 void set(){std::lock_guard lock(mutex);signaled=true;cv.notify_all();}
 void wait(){std::unique_lock lock(mutex);CHECK(cv.wait_for(lock,3s,[&]{return signaled;}));}
};
std::atomic<unsigned>destructors=0;
struct Producer {
 u8 value=0;bool check_destructor=false;std::shared_ptr<Event>ready;
 ~Producer(){if(!check_destructor)return;
  // Producer/resource destruction must not happen under the SDK lock held by
  // this thread. A second thread must be able to acquire it before we return.
  auto acquired=std::async(std::launch::async,[]{auto critical=rex::thread::global_critical_region::AcquireDirect();return true;});
  CHECK(acquired.wait_for(500ms)==std::future_status::ready&&acquired.get());++destructors;
 }
};
struct Legacy {
 rex::memory::Memory&memory;u32 page;bool pending=true,running=false;
 std::condition_variable_any cv;Event entered,ready;
 static bool Call(std::unique_lock<std::recursive_mutex>&critical,void*context,u32 address){
  auto&s=*static_cast<Legacy*>(context);if((address&~4095u)!=s.page)return false;
  s.cv.wait(critical,[&]{return !s.running;});if(!s.pending)return false;
  s.running=true;critical.unlock();s.entered.set();s.ready.wait();critical.lock();
  const PhysicalCopyRange range{s.page+64,1};
  {auto writable=OpenPhysicalWriteback(&s.memory,{&range,1});CHECK(writable);*s.memory.TranslatePhysical<u8*>(s.page+64)=0xD3;}
  s.pending=false;s.running=false;s.cv.notify_all();s.memory.DisablePhysicalMemoryDataProviders(s.page,4096);return true;
 }
};
int TestCrossPageOrder(rex::memory::Memory&memory,PhysicalDependencyProvider::Api api) {
 constexpr u32 origin=0x20000;
 std::vector<u64> delivered;
 PhysicalDependencyProvider provider(memory,api,[&](std::span<const PhysicalDependencyProvider::Pending>batch){
  for(const auto&ticket:batch){
   const auto value=*std::static_pointer_cast<u8>(ticket->producer);
   for(auto range:ticket->ranges){auto writable=OpenPhysicalWriteback(&memory,{&range,1});CHECK(writable);std::memset(memory.TranslatePhysical(range.address),value,range.length);}
   delivered.push_back(ticket->sequence);
  }
  return true;
 });CHECK(provider.registered());
 auto track=[&](u64 sequence,std::initializer_list<PhysicalCopyRange>ranges,u8 value){CHECK(provider.Track(sequence,{ranges.begin(),ranges.size()},std::make_shared<u8>(value)));};
 // Faulting B must include the older A producer before the A+B producer.
 const PhysicalCopyRange a{origin+128,16},b{origin+4096+128,16},c{origin+8192+128,16},d{origin+12288+128,16};
 track(1,{a},0xA1);track(2,{a,b},0xB2);
 CHECK(*memory.TranslateVirtual<volatile u8*>(0xC0000000+b.address)==0xB2);
 CHECK(delivered==std::vector<u64>({1,2})&&provider.pending_count()==0);
 CHECK(*memory.TranslateVirtual<volatile u8*>(0xA0000000+a.address)==0xB2);CHECK(provider.Reap());
 std::puts("cross-page direct overlap passed");std::fflush(stdout);
 // Transitive closure through A+B -> B+C, while unrelated D remains pending.
 delivered.clear();track(3,{d},0xD3);track(4,{a},0xA4);track(5,{a,b},0xB5);track(6,{b,c},0xC6);
 CHECK(*memory.TranslateVirtual<volatile u8*>(0xA0000000+c.address)==0xC6);
 CHECK(delivered==std::vector<u64>({4,5,6})&&provider.pending_count()==1);
 CHECK(*memory.TranslateVirtual<volatile u8*>(0xC0000000+b.address)==0xC6);
 CHECK(*memory.TranslateVirtual<volatile u8*>(0xC0000000+a.address)==0xB5);
 CHECK(*memory.TranslateVirtual<volatile u8*>(0xA0000000+d.address)==0xD3);
 CHECK(provider.pending_count()==0&&delivered==std::vector<u64>({4,5,6,3}));CHECK(provider.Reap());
 std::puts("cross-page transitive/disjoint overlap passed");std::fflush(stdout);
 // Differential against a simple sequential byte oracle. Each trial contains
 // overlapping, disjoint, adjacent and multi-page producers, with fault pages
 // visited in a different order. GPU/recorder work is a byte-publication boundary.
 u64 sequence=6;u32 random=0x12345678;
 auto next=[&]{random^=random<<13;random^=random>>17;random^=random<<5;return random;};
 for(unsigned trial=0;trial<96;++trial){
  if(!(trial%16)){std::printf("cross-page oracle trial %u\n",trial);std::fflush(stdout);}
  std::vector<u8> expected(16384,0x11);
  std::memset(memory.TranslatePhysical(origin),0x11,expected.size());
  for(unsigned write=0;write<24;++write){
   std::vector<PhysicalCopyRange>ranges;
   for(unsigned range=0,n=1+next()%3;range<n;++range){
    const u32 offset=(next()%4)*4096+(next()%32),length=1+next()%32;
    ranges.push_back({origin+offset,length});
   }
   const u8 value=u8(1+next()%255);
   CHECK(provider.Track(++sequence,ranges,std::make_shared<u8>(value)));
   for(auto range:ranges)std::memset(expected.data()+range.address-origin,value,range.length);
  }
  std::array<u32,4>pages{0,1,2,3};for(unsigned i=3;i>0;--i)std::swap(pages[i],pages[next()%(i+1)]);
  for(auto page:pages){volatile u8 value=*memory.TranslateVirtual<volatile u8*>(0xA0000000+origin+page*4096+64);(void)value;}
  CHECK(provider.pending_count()==0&&std::memcmp(memory.TranslatePhysical(origin),expected.data(),expected.size())==0);
  CHECK(provider.Reap());
 }
 CHECK(provider.Stop(Proof::kLegacyProducerQuiesced));
 std::puts("PASS cross-page producer dependency closure: direct/transitive overlap, unrelated pending producer retained, 96 differential byte-order trials / 2304 writes.");
 return 0;
}
int main(int argc,char**argv){
 SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
 std::set_terminate([]{std::fputs("EXPECTED_UNDELIVERED_DEPENDENCY_FATAL\n",stderr);std::_Exit(23);});
 const std::string mode=argc>1?argv[1]:"positive";
 char loaded[MAX_PATH]{};CHECK(GetModuleFileNameA(GetModuleHandleA("rexruntime.dll"),loaded,MAX_PATH));CHECK(!_stricmp(loaded,std::getenv("LEGO_TEST_RUNTIME_EXPECTED")));
 rex::memory::Memory memory;CHECK(memory.Initialize());
 auto module=GetModuleHandleA("rexruntime.dll");
 PhysicalDependencyProvider::Api api{
  reinterpret_cast<rex::memory::RegisterPhysicalDataProvider>(GetProcAddress(module,"rex_memory_register_data_provider_v1")),
  reinterpret_cast<rex::memory::UnregisterPhysicalDataProvider>(GetProcAddress(module,"rex_memory_unregister_data_provider_v1"))};CHECK(api.add&&api.remove);
 constexpr u32 page=0x20000;auto*heap=memory.LookupHeap(0xC0000000);CHECK(heap->Protect(0xC0000000+page,16384,rex::memory::kMemoryProtectRead|rex::memory::kMemoryProtectWrite));
 std::memset(memory.TranslatePhysical(page),0x11,16384);*memory.TranslatePhysical<u32*>(page+4000)=0xDEADBEEF;
 if(mode=="cross-page-order")return TestCrossPageOrder(memory,api);
 std::vector<u64>delivered;Event entered;bool first=true,allow_settle=mode!="retry-stop";
 PhysicalDependencyProvider* self=nullptr;
 PhysicalDependencyProvider provider(memory,api,[&](std::span<const PhysicalDependencyProvider::Pending>batch){
  CHECK(self&&!self->Stop(Proof::kLegacyProducerQuiesced)&&!self->Reap());
  if(mode=="failed"||!allow_settle)return false;
  if(mode=="unsafe-metadata") {CHECK(*memory.TranslateVirtual<volatile u32*>(0xC0000000+page+4000)==0xDEADBEEF);}
  u32 metadata=0;CHECK(CopyNativeGuestMetadata(memory,0xC0000000+page+4000,&metadata,4)&&metadata==0xDEADBEEF);
  for(const auto&ticket:batch){
   if(!delivered.empty())CHECK(ticket->sequence>delivered.back());
   auto payload=std::static_pointer_cast<Producer>(ticket->producer);
   if(first){first=false;entered.set();}if(payload->ready)payload->ready->wait();
   for(auto range:ticket->ranges){auto writable=OpenPhysicalWriteback(&memory,{&range,1});CHECK(writable);std::memset(memory.TranslatePhysical(range.address),payload->value,range.length);}
   delivered.push_back(ticket->sequence);
  }
  return true;
 });self=&provider;CHECK(provider.registered()&&provider.accepting());
 auto make=[&](u8 value,std::shared_ptr<Event>ready={}){auto owner=std::make_shared<Producer>();owner->value=value;owner->ready=std::move(ready);owner->check_destructor=mode=="positive";return owner;};
 const PhysicalCopyRange a{page+512,32},b{page+520,16};
 CHECK(!provider.Track(0,{&a,1},make(1))&&!provider.Track(1,{},make(1)));
 if(mode=="retry-stop"){
  CHECK(provider.Track(1,{&a,1},make(0xA1)));
  CHECK(!provider.Stop(Proof::kLegacyProducerQuiesced)&&provider.registered()&&!provider.accepting()&&provider.pending_count()==1);
  MEMORY_BASIC_INFORMATION protection{};CHECK(VirtualQuery(memory.TranslateVirtual(0xA0000000+page),&protection,sizeof(protection))&&((protection.Protect&255)==PAGE_NOACCESS));
  allow_settle=true;CHECK(provider.Stop(Proof::kLegacyProducerQuiesced)&&!provider.registered());
  CHECK(*memory.TranslateVirtual<volatile u8*>(0xC0000000+a.address)==0xA1);
  std::puts("PASS failed Stop retains pending ownership/protection and registration until a successful retry.");return 0;
 }
 if(mode!="positive"){
  CHECK(provider.Track(1,{&a,1},make(0xA1)));auto value=*memory.TranslateVirtual<volatile u8*>(0xA0000000+a.address);
  std::printf("UNSAFE STALE READ %u\n",value);return 23;
 }
 // Ownership exists before producer work is ready. The callback must drop
 // the SDK lock while awaiting that producer, permitting capture of ticket 2.
 auto ready=std::make_shared<Event>();CHECK(provider.Track(1,{&a,1},make(0xA1,ready)));
 auto reader=std::async(std::launch::async,[&]{return *memory.TranslateVirtual<volatile u8*>(0xA0000000+a.address);});entered.wait();
 CHECK(provider.Track(2,{&b,1},make(0xB2)));ready->set();CHECK(reader.get()==0xA1);
 CHECK(delivered==std::vector<u64>({1,2})&&provider.pending_count()==0);
 CHECK(*memory.TranslatePhysical<u8*>(b.address)==0xB2&&*memory.TranslatePhysical<u8*>(a.address+31)==0xA1);
 const auto before_reap=destructors.load();CHECK(provider.Reap());CHECK(destructors==before_reap+2);
 CHECK(!provider.Track(2,{&a,1},make(0)));
 // A fault outside the authored byte range still has to settle its page.
 CHECK(provider.Track(3,{&a,1},make(0xC3)));CHECK(*memory.TranslateVirtual<volatile u8*>(0xE0000000+page+17-4096)==0x11);
 CHECK(provider.pending_count()==0&&*memory.TranslatePhysical<u8*>(a.address)==0xC3);CHECK(provider.Reap());
 // A legacy GPU writer waits on the same page after our ticket retires.
 // Another CPU access must remain blocked rather than read its stale byte.
 Legacy legacy{memory,page};memory.SetPhysicalMemoryDataProvider(Legacy::Call,&legacy);
 CHECK(provider.Track(4,{&a,1},make(0xA4)));memory.EnablePhysicalMemoryDataProviders(page,4096);
 auto native_reader=std::async(std::launch::async,[&]{return *memory.TranslateVirtual<volatile u8*>(0xA0000000+a.address);});legacy.entered.wait();
 auto legacy_reader=std::async(std::launch::async,[&]{return *memory.TranslateVirtual<volatile u8*>(0xC0000000+page+64);});
 CHECK(legacy_reader.wait_for(40ms)==std::future_status::timeout);legacy.ready.set();
 CHECK(native_reader.get()==0xA4&&legacy_reader.get()==0xD3);memory.SetPhysicalMemoryDataProvider(nullptr,nullptr);CHECK(provider.Reap());
 // CPU-only read permission is preserved by publication and final retirement.
 CHECK(heap->Protect(0xC0000000+page,4096,rex::memory::kMemoryProtectRead));
 CHECK(provider.Track(5,{&a,1},make(0xA5)));CHECK(*memory.TranslateVirtual<volatile u8*>(0xC0000000+a.address)==0xA5);
 MEMORY_BASIC_INFORMATION protection{};CHECK(VirtualQuery(memory.TranslateVirtual(0xC0000000+page),&protection,sizeof(protection))&&((protection.Protect&255)==PAGE_READONLY));
 CHECK(provider.Stop(Proof::kLegacyProducerQuiesced)&&!provider.registered()&&!provider.accepting());
 CHECK(*memory.TranslateVirtual<volatile u8*>(0xA0000000+a.address)==0xA5);
 CHECK(!provider.Track(6,{&a,1},make(0))&&provider.Stop(Proof::kLegacyProducerQuiesced));
 std::puts("PASS production physical dependency protocol: pending-before-ready, unlocked wait/new overlapping ticket, ordered bytes, shared-page legacy preservation, deferred resource destruction, readonly aliases and explicit stop/unregister.");
}
