#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <plume_render_interface_types.h>

namespace legodimensions::gpu_native {
// IA bindings survive PSO/root-signature changes, but never a command-list
// reset. The native renderer is the sole IA writer during guest draws; its
// raw overlay runs only at present, immediately before ending the recording.
class VertexBindingCache {
 public:
  static constexpr size_t kSlots = 32;
  template<class Emit>
  void Bind(const void* commands,uint64_t serial,
      std::span<const plume::RenderVertexBufferView> views,
      std::span<const plume::RenderInputSlot> slots,Emit&& emit) {
    if (!commands || !serial || views.size()>kSlots || views.size()!=slots.size()) {
      Reset();emit(0,uint32_t(views.size()));return;
    }
    if (commands_!=commands || serial_!=serial) {
      valid_.fill(false);commands_=commands;serial_=serial;
    }
    std::array<bool,kSlots> changed{};
    for(size_t i=0;i<views.size();++i) {
      // Native arrays are indexed by physical slot. An unfamiliar layout
      // keeps the ordinary full binding instead of making a partial guess.
      if(slots[i].index!=i) {Reset();emit(0,uint32_t(views.size()));return;}
      Key key{views[i].buffer.ref,views[i].buffer.offset,views[i].size,
              slots[i].stride,slots[i].classification};
      changed[i]=!valid_[i] || !(keys_[i]==key);
      keys_[i]=key;valid_[i]=true;
    }
    for(uint32_t i=0;i<views.size();) {
      if(!changed[i]) {++i;continue;}
      const uint32_t first=i++;
      while(i<views.size() && changed[i]) ++i;
      emit(first,i-first);
    }
  }
  void Reset() {*this={};}
 private:
  struct Key {
    const void* buffer=nullptr;
    uint64_t offset=0;
    uint32_t size=0,stride=0;
    plume::RenderInputSlotClassification classification{};
    bool operator==(const Key&)const=default;
  };
  const void* commands_=nullptr;
  uint64_t serial_=0;
  std::array<Key,kSlots> keys_{};
  std::array<bool,kSlots> valid_{};
};
}
