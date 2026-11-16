#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <rex/system/xmemory.h>

namespace legodimensions::gpu_native {

// Internal CPU-authored metadata, not a guest-visible payload read. A provider
// must validate placement even when the header shares a protected data page.
// Physical metadata uses the SDK's existing physical bypass; guest aliases
// keep their provider protection. Never use this to bypass a data dependency.
inline bool CopyNativeGuestMetadata(rex::memory::Memory& memory, uint32_t address,
                                    void* destination, uint32_t length) {
  constexpr uint32_t physical_limit=0x20000000u;
  constexpr uint32_t metadata_limit=64u*1024;
  if (!address || !destination || !length || length>metadata_limit ||
      uint64_t(address)+length>0x100000000ull) return false;
  auto lock=rex::thread::global_critical_region::AcquireDirect();
  auto* heap=memory.LookupHeap(address);
  const uint32_t last=address+length-1;
  if (!heap || memory.LookupHeap(last)!=heap) return false;
  const void* source=nullptr;
  if (heap->heap_type()==rex::memory::HeapType::kGuestPhysical) {
    const uint32_t physical=memory.GetPhysicalAddress(address);
    if (physical>=physical_limit || length>physical_limit-physical ||
        memory.GetPhysicalAddress(last)!=physical+length-1 ||
        !memory.GetPhysicalHeap()->IsRangeCommittedReadable(physical,length)) return false;
    // Empty A/C/E aliases inherit canonical permissions. Explicit mappings
    // must themselves be committed and readable; reserved/no-access pages
    // must not become readable merely because physical backing exists.
    const uint32_t page=heap->page_size();
    for (uint64_t at=address,end=uint64_t(address)+length;at<end;) {
      const auto next=std::min(end,(at&~uint64_t(page-1))+page);
      if (!heap->IsRangeCommittedReadable(uint32_t(at),uint32_t(next-at))) {
        uint32_t region=uint32_t(at),size=0;
        if (!heap->QueryBaseAndSize(&region,&size) || size) return false;
      }
      at=next;
    }
    source=memory.TranslatePhysical<const void*>(physical);
  } else {
    if (!heap->IsRangeCommittedReadable(address,length)) return false;
    source=memory.TranslateVirtual<const void*>(address);
  }
  std::memcpy(destination,source,length);
  return true;
}
}  // namespace legodimensions::gpu_native
