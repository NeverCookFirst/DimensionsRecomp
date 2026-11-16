// Private SDK adapter candidate. Not included in the game build yet.
#ifdef LEGODIMENSIONS_NATIVE_GPU
#include "slot_growth.h"
#include "native_frontend_guest.h"
#include "gpu_native/guest_metadata.h"
#include <rex/hook.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#include <memory>
#include <unordered_map>
REX_EXTERN(__imp__sub_836F9E48);
REX_EXTERN(__imp__sub_82A3B6D8);
REX_EXTERN(__imp__sub_82A57FE0);
REX_EXTERN(sub_82A3B578);
namespace legodimensions::native_slot_growth {
namespace {
using namespace native_load_heap;
using native_frontend::CallGuest;
struct Entry {SlotIdentity identity;uint32_t provider;std::shared_ptr<SlotGrowth> growth;};
std::mutex registry_mutex;
std::unordered_map<uint32_t,std::shared_ptr<Entry>> by_heap,by_provider;
rex::memory::Memory* memory=nullptr;
rex::runtime::FunctionDispatcher* dispatcher=nullptr;
uint32_t provider_vtable=0;
uint32_t provider_slab=0,provider_cursor=8;
std::array<uint32_t,16382> unused_providers{};size_t unused_count=0;
bool Read(uint32_t address,void*data,uint32_t bytes){return memory&&gpu_native::CopyNativeGuestMetadata(*memory,address,data,bytes);}
bool WriteWordOn(rex::memory::Memory*mem,uint32_t address,uint32_t value){
  auto*heap=mem?mem->LookupHeap(address):nullptr;rex::memory::HeapAllocationInfo info{};
  if(!heap||!heap->QueryRegionInfo(address,&info)||!(info.state&rex::memory::kMemoryAllocationCommit)||
      !(info.protect&rex::memory::kMemoryProtectWrite))return false;
  const auto encoded=__builtin_bswap32(value);std::memcpy(mem->TranslateVirtual<void*>(address),&encoded,4);return true;
}
bool WriteWord(uint32_t address,uint32_t value){return WriteWordOn(memory,address,value);}
struct ProviderReservation {
  uint32_t address=0;bool published=false;
  ProviderReservation(){
    std::lock_guard guard(registry_mutex);
    if(unused_count)address=unused_providers[--unused_count];
    else if(provider_slab&&provider_cursor<=65532){address=provider_slab+provider_cursor;provider_cursor+=4;}
  }
  ~ProviderReservation(){
    if(address&&!published){std::lock_guard guard(registry_mutex);unused_providers[unused_count++]=address;}
  }
};
std::shared_ptr<Entry> Find(uint32_t value,bool provider){
  std::lock_guard guard(registry_mutex);auto&map=provider?by_provider:by_heap;
  const auto it=map.find(value);return it==map.end()?nullptr:it->second;
}
struct GuestHeapLock {
  PPCContext&ctx;uint8_t*base;PPCFunc*leave=nullptr;uint32_t critical=0;bool held=false;
  GuestHeapLock(PPCContext&c,uint8_t*b,uint32_t heap):ctx(c),base(b){
    auto*enter=dispatcher?dispatcher->GetFunction(0x8411319C):nullptr;
    leave=dispatcher?dispatcher->GetFunction(0x841131BC):nullptr;
    if(enter&&leave&&Word(Read,heap+0x3478,critical)&&critical){CallGuest(enter,ctx,base,critical);held=true;}
  }
  ~GuestHeapLock(){if(held)CallGuest(leave,ctx,base,critical);}
};
REX_FUNC(Grow){
  try{
    auto entry=Find(ctx.r3.u32,true);auto*attempt=AllocationAttemptScope::Current();
    if(!entry||!attempt||uint32_t(ctx.lr)!=0x82A33FC4){ctx.r3.u32=0;return;}
    const auto ok=entry->growth->Grow(ctx.r3.u32,ctx.r4.u32,ctx.r5.u32,ctx.r6.u32,*attempt,
        [&](uint32_t heap,uint32_t address,uint32_t bytes){
          CallGuest(sub_82A3B578,ctx,base,heap,address,bytes,0);
          uint32_t head{};return Word(Read,heap+0x98,head)&&head==address;
        });
    if(ok)REXLOG_INFO("Native slot heap: extended heap={:08X} slot={:08X} owned_regions={}",entry->identity.heap,entry->identity.slot,entry->growth->OwnedCount());
    ctx.r3.u32=ok;
  }catch(...){ctx.r3.u32=0;}
}
REX_FUNC(Release){
  try{
    auto entry=Find(ctx.r3.u32,true);
    if(!entry||uint32_t(ctx.lr)!=0x82A3B88C){ctx.r3.u32=0;return;}
    ctx.r3.u32=entry->growth->Release(ctx.r3.u32,ctx.r4.u32,ctx.r5.u32,ctx.r6.u32);
  }catch(...){ctx.r3.u32=0;}
}
bool Runtime(uint8_t*base){
  std::lock_guard guard(registry_mutex);
  auto*kernel=rex::system::kernel_state();
  if(!kernel||!kernel->memory()||!kernel->function_dispatcher()||kernel->memory()->virtual_membase()!=base)return false;
  if(memory)return memory==kernel->memory()&&dispatcher==kernel->function_dispatcher()&&provider_vtable;
  auto*mem=kernel->memory();auto*dispatch=kernel->function_dispatcher();
  const auto grow=dispatch->AllocateThunk(Grow,0),release=dispatch->AllocateThunk(Release,0);
  if(!grow||!release)return false;
  const auto table=mem->SystemHeapAlloc(65536);if(!table)return false;
  if(!WriteWordOn(mem,table,grow)||!WriteWordOn(mem,table+4,release)){mem->SystemHeapFree(table);return false;}
  // Runtime pointers are immutable after this one synchronized publication.
  memory=mem;dispatcher=dispatch;provider_slab=table;provider_vtable=table;return true;
}
void Register(PPCContext&ctx,uint8_t*base,uint32_t slot){
  if(!Runtime(base))return;
  const auto identity=ReadSlot(Read,slot);if(!identity)return;
  GuestHeapLock lock(ctx,base,identity->heap);if(!lock.held)return;
  const auto current=ReadSlot(Read,slot);uint32_t old{};
  if(!current||*current!=*identity||!Word(Read,identity->heap+0x90,old)||old||Find(identity->heap,false))return;
  ProviderReservation reservation;const auto provider=reservation.address;
  if(!provider||!WriteWord(provider,provider_vtable))return;
  GrowthApi api;api.read=Read;
  api.allocate=[](uint32_t bytes){return memory->SystemHeapAlloc(bytes,65536,rex::memory::kSystemHeapPhysical);};
  api.release=[](uint32_t address,uint32_t bytes){
    auto*heap=memory->LookupHeap(address);rex::memory::HeapAllocationInfo info{};
    if(!heap||!heap->QueryRegionInfo(address,&info)||info.allocation_base!=address||info.allocation_size!=bytes||
        !(info.state&rex::memory::kMemoryAllocationCommit))return false;
    return heap->Release(address);
  };
  auto growth=std::make_shared<SlotGrowth>(*identity,provider,std::move(api));
  if(!growth->ValidateRegistration())return;
  auto entry=std::make_shared<Entry>(Entry{*identity,provider,growth});
  {
    std::lock_guard guard(registry_mutex);
    if(by_heap.contains(identity->heap))return;
    const auto [position,inserted]=by_provider.emplace(provider,entry);if(!inserted)return;
    try{if(!by_heap.emplace(identity->heap,entry).second){by_provider.erase(position);return;}}
    catch(...){by_provider.erase(position);throw;}
  }
  if(!WriteWord(identity->heap+0x90,provider)){
    std::lock_guard guard(registry_mutex);by_heap.erase(identity->heap);by_provider.erase(provider);return;
  }
  reservation.published=true;
  REXLOG_INFO("Native slot heap: registered slot={:08X} heap={:08X} initial_bytes={} original_budget_unchanged=true",slot,identity->heap,identity->bytes);
}
} // namespace
} // namespace legodimensions::native_slot_growth
REX_HOOK_RAW(sub_836F9E48){
  const auto slot=ctx.r3.u32;__imp__sub_836F9E48(ctx,base);
  try{legodimensions::native_slot_growth::Register(ctx,base,slot);}catch(...){}
}
REX_HOOK_RAW(sub_82A3B6D8){
  using namespace legodimensions::native_slot_growth;
  const auto heap=ctx.r3.u32,address=ctx.r4.u32;__imp__sub_82A3B6D8(ctx,base);
  if(ctx.r3.u32)return;
  try{
    auto entry=Find(heap,false);if(!entry||address!=entry->identity.backing)return;
    GuestHeapLock lock(ctx,base,heap);
    if(!lock.held||!entry->growth->DetachedExternal(heap,address))REXLOG_ERROR("Native slot heap: external detach verification failed heap={:08X}",heap);
  }catch(...){}
}
REX_HOOK_RAW(sub_82A57FE0){
  using namespace legodimensions::native_slot_growth;
  const auto heap=ctx.r3.u32;std::shared_ptr<Entry> entry;
  try{entry=Find(heap,false);}catch(...){}
  if(!entry){__imp__sub_82A57FE0(ctx,base);return;}
  // The embedded critical object survives82A57FE0. Keep the original guest
  // lock held across preflight/traversal/retirement, with no host mutex held
  // across original code or its reentrant Release callback.
  GuestHeapLock lock(ctx,base,heap);
  bool valid=false;try{valid=lock.held&&entry->growth->BeginDestroy();}catch(...){}
  if(!valid){REX_FATAL("Native slot heap metadata invalid before teardown heap={:08X}",heap);return;}
  __imp__sub_82A57FE0(ctx,base); // Original leak diagnostics and healthy teardown.
  try{
    const bool complete=entry->growth->FinishDestroy();
    if(complete){
      std::lock_guard guard(registry_mutex);
      if(auto it=by_heap.find(heap);it!=by_heap.end()&&it->second==entry)by_heap.erase(it);
      by_provider.erase(entry->provider);
      // Published four-byte descriptors remain as bounded tombstones in one
      // 64KiB slab. No per-generation SDK page or retired host Entry survives.
    }else REXLOG_ERROR("Native slot heap: retained {} owned regions after original teardown heap={:08X}",entry->growth->OwnedCount(),heap);
  }catch(...){}
}
#endif
