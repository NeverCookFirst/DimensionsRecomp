#pragma once
// Private candidate. SDK hooks must call this while the title heap lock is held.
// The original allocation/free/bind/cleanup functions retain ownership semantics.
#include "heap_shape.h"
#include <algorithm>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>
namespace native_load_heap {
using Reader=std::function<bool(uint32_t,void*,uint32_t)>;
inline bool Word(const Reader&read,uint32_t at,uint32_t&v){
  std::array<uint8_t,4>b{};if(!read(at,b.data(),4))return false;
  v=uint32_t(b[0])<<24|uint32_t(b[1])<<16|uint32_t(b[2])<<8|b[3];return true;
}
struct SlotIdentity {
  uint32_t slot{},heap{},backing{},bytes{},loader{},name{},id{};
  bool operator==(const SlotIdentity&)const=default;
};
inline std::optional<SlotIdentity> ReadSlot(const Reader&read,uint32_t slot){
  // TU23 inline vectors and exact slot stride, proven from constructors.
  for(auto [vector,capacity]:{std::pair{0x847E1650u,7u},std::pair{0x847E2740u,4u}}){
    uint32_t data{},packed{},count{};
    if(!Word(read,vector,data)||!Word(read,vector+4,count)||!Word(read,vector+8,packed))continue;
    if(data!=vector+16||(packed>>2)!=capacity||(packed&3)!=2||count>capacity||slot<data||
        (slot-data)%616||(slot-data)/616>=count)continue;
    SlotIdentity out{};out.slot=slot;
    uint32_t loader_storage{};
    uint32_t loader_heap{};
    if(!Word(read,slot+8,out.loader)||!Word(read,slot+12,out.name)||!Word(read,slot+28,out.heap)||
        !Word(read,slot+36,loader_storage)||!Word(read,slot+40,out.backing)||!Word(read,slot+44,out.bytes)||
        !out.loader||out.loader!=loader_storage||!out.heap||!out.backing||out.bytes<64||
        uint64_t(out.backing)+out.bytes>0x100000000ull||!Word(read,out.heap+12,out.id)||
        !Word(read,out.loader+4,loader_heap)||loader_heap!=out.heap)return {};
    const auto expected=std::string(vector==0x847E1650u?"MechPortal Load Slot ":"MechPortal Player Load Slot ")+std::to_string((slot-data)/616);
    std::array<char,40> text{};
    if(!out.name||expected.size()>=text.size()||!read(out.name,text.data(),uint32_t(expected.size()+1)))return {};
    if(expected.size()>=text.size()||!std::equal(expected.begin(),expected.end(),text.begin())||text[expected.size()])return {};
    std::array<char,40> heap_name{};
    if(!read(out.heap+16,heap_name.data(),uint32_t(expected.size()+1))||
        !std::equal(expected.begin(),expected.end(),heap_name.begin())||heap_name[expected.size()])return {};
    return out;
  }
  return {};
}
struct GrowthApi {
  Reader read;
  std::function<uint32_t(uint32_t)> allocate; // SDK physical pages,64KiB alignment.
  std::function<bool(uint32_t,uint32_t)> release; // Exact base/bytes,never foreign.
};
enum class Phase {kActive,kClosing,kRetired};
struct AllocationAttempt {bool growth_used=false;};
class AllocationAttemptScope {
 public:
  AllocationAttemptScope():previous_(current_){current_=&attempt_;}
  ~AllocationAttemptScope(){current_=previous_;}
  static AllocationAttempt* Current(){return current_;}
 private:
  AllocationAttempt attempt_{};AllocationAttempt* previous_{};
  static inline thread_local AllocationAttempt* current_=nullptr;
};
class SlotGrowth {
 public:
  SlotGrowth(SlotIdentity id,uint32_t provider,GrowthApi api):identity_(id),provider_(provider),api_(std::move(api)){
    regions_.reserve(64);regions_.push_back({{id.backing,id.bytes,true},true});
  }
  bool ValidateRegistration(){
    std::lock_guard guard(mutex_);uint32_t old{};
    return CurrentIdentity()&&Word(api_.read,identity_.heap+0x90,old)&&!old&&Shape();
  }
  bool Grow(uint32_t provider,uint32_t heap,uint32_t needed,uint32_t ceiling,AllocationAttempt&attempt,
            const std::function<bool(uint32_t,uint32_t,uint32_t)>&bind_owned){
    try{
      std::lock_guard guard(mutex_);
      if(phase_!=Phase::kActive||provider!=provider_||heap!=identity_.heap||!CurrentIdentity()||!OwnProvider()||
          attempt.growth_used||regions_.size()>=64||HasPending()||needed<0x4000||ceiling!=0x1FFFFFFF||!Shape())return false;
      // A failed retry with no successful allocation must not keep adding
      // regions for an unsupported alignment/request or a damaged allocator.
      const uint64_t rounded=(uint64_t(needed)+65536+65535)&~uint64_t(65535);
      if(rounded>ceiling||rounded>UINT32_MAX)return false;
      const auto address=api_.allocate(uint32_t(rounded));if(!address)return false;
      // The SDK aligns physical backing. Its E alias is biased by4KiB, so
      // the guest alias itself is page aligned but need not be64KiB aligned.
      if((address&4095)||uint64_t(address)+rounded>0x100000000ull){
        api_.release(address,uint32_t(rounded));return false;
      }
      // Record ownership before binding. A failed/uncertain native bind may
      // have touched the heap; never release potentially linked pages blindly.
      const Region region{address,uint32_t(rounded),false};
      regions_.push_back({region,false});
      attempt.growth_used=true;
      bind_owned(heap,address,uint32_t(rounded));
      auto all=Known();all.push_back(region);
      if(ValidateShape(heap,all,api_.read)){regions_.back().confirmed=true;return true;}
      // If the old heap is still exact and the new region is absent, this is
      // a proven unlinked allocation. Otherwise retain it as pending evidence.
      if(Shape()&&api_.release(address,uint32_t(rounded)))regions_.pop_back();
      return false;
    }catch(...){return false;} // No host allocation exception may cross PPC/noexcept.
  }
  bool Release(uint32_t provider,uint32_t heap,uint32_t address,uint32_t bytes){
    try{
      std::lock_guard guard(mutex_);
      if(phase_==Phase::kRetired||provider!=provider_||heap!=identity_.heap||!OwnProvider()||
          (phase_==Phase::kActive&&!CurrentIdentity()))return false;
      auto it=std::find_if(regions_.begin(),regions_.end(),[&](auto r){return !r.region.external&&r.region.address==address&&r.region.bytes==bytes;});
      if(it==regions_.end())return false;
      // B7E0 already removed this whole free block from its bin. Validate the
      // one region here; full-bin validation belongs before growth/cleanup.
      uint32_t begin{},end{},packed{},footer{},original{},size{},flag{};
      if(!Word(api_.read,address,original)||!Word(api_.read,address+4,size)||
          !Word(api_.read,address+8,begin)||!Word(api_.read,address+12,end)||
          !Word(api_.read,address+24,flag)||original!=address||size!=bytes||begin!=address+32||
          end!=((address+bytes)&~3u)-4||(flag>>24)||!Word(api_.read,begin,packed)||
          (packed&0x78000000)||uint64_t(begin)+(packed&0x7FFFFFF)*4!=end||
          !Word(api_.read,end-4,footer)||(footer&0x7FFFFFF)!=(packed&0x7FFFFFF)||(footer>>27))return false;
      if(!api_.release(address,bytes))return false;
      regions_.erase(it);return true;
    }catch(...){return false;}
  }
  bool DetachedExternal(uint32_t heap,uint32_t address){
    std::lock_guard guard(mutex_);
    if(phase_==Phase::kRetired||heap!=identity_.heap||!OwnProvider())return false;
    auto it=std::find_if(regions_.begin(),regions_.end(),[&](auto r){return r.region.external&&r.region.address==address;});
    if(it==regions_.end())return false;
    auto known=Known();known.erase(std::remove_if(known.begin(),known.end(),[&](auto r){return r.address==address&&r.external;}),known.end());
    if(!ValidateShape(heap,known,api_.read))return false;
    regions_.erase(it);return true;
  }
  bool BeginDestroy(){
    std::lock_guard guard(mutex_);if(phase_!=Phase::kActive||!OwnProvider())return false;
    phase_=Phase::kClosing;return !HasPending()&&bool(Shape());
  }
  bool FinishDestroy(){
    std::lock_guard guard(mutex_);
    if(std::any_of(regions_.begin(),regions_.end(),[](auto r){return !r.region.external;})){phase_=Phase::kClosing;return false;}
    phase_=Phase::kRetired;return true;
  }
  size_t OwnedCount(){std::lock_guard guard(mutex_);return std::count_if(regions_.begin(),regions_.end(),[](auto r){return !r.region.external;});}
 private:
  bool CurrentIdentity(){const auto current=ReadSlot(api_.read,identity_.slot);return current&&*current==identity_;}
  bool OwnProvider(){uint32_t current{},id{};return Word(api_.read,identity_.heap+0x90,current)&&current==provider_&&Word(api_.read,identity_.heap+12,id)&&id==identity_.id;}
  std::vector<Region> Known(){std::vector<Region> out;out.reserve(regions_.size());for(auto r:regions_)if(r.confirmed)out.push_back(r.region);return out;}
  bool HasPending(){return std::any_of(regions_.begin(),regions_.end(),[](auto r){return !r.confirmed;});}
  ShapeResult Shape(){auto known=Known();return ValidateShape(identity_.heap,known,api_.read);}
  SlotIdentity identity_;uint32_t provider_{};GrowthApi api_;Phase phase_=Phase::kActive;
  struct Record {Region region;bool confirmed;};
  std::vector<Record> regions_;std::mutex mutex_;
};
} // namespace native_load_heap
