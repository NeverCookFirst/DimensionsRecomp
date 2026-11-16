#pragma once
#include "gpu_native/pool_copy.h"
#include <rex/system/xmemory.h>
#include <algorithm>
#include <cstdint>
#include <exception>
#include <memory>
#include <span>
#include <vector>

namespace legodimensions::gpu_native {
// Keep write-watch arming and the following physical write window in the
// same SDK protection transaction. No renderer/GPU wait may occur inside it.
inline auto LockPhysicalWriteback() {
  return rex::thread::global_critical_region::AcquireDirect();
}

// GPU publication is a hardware write, independent of the CPU view's write
// permission. Open only the internal physical host mapping, never guest aliases
// or guest protection metadata. Construct after GPU completion: this scope
// holds the SDK critical region and must not contain a wait for another thread.
class PhysicalWriteScope {
  struct Changed { void* address; size_t length; rex::memory::PageAccess access; };
  std::unique_lock<std::recursive_mutex> lock_;
  std::vector<Changed> changed_;
  size_t opened_ = 0;
  bool valid_ = false;

  void Restore() noexcept {
    while (opened_) {
      const auto& item=changed_[--opened_];
      if (!rex::memory::Protect(item.address,item.length,item.access))
        std::terminate(); // Never silently retain a protection change.
    }
    changed_.clear();
  }
 public:
  PhysicalWriteScope(rex::memory::Memory& memory,std::span<const PhysicalCopyRange> ranges)
      :lock_(rex::thread::global_critical_region::AcquireDirect()) {
    constexpr uint32_t limit=0x20000000;
    // Validate all allocations before making even a temporary host change.
    for (auto range:ranges) {
      if (!range.length || range.address>=limit || range.length>limit-range.address ||
          !memory.GetPhysicalHeap()->IsRangeCommittedReadable(range.address,range.length)) return;
    }
    const auto page_size=rex::memory::page_size();
    for (auto range:ranges) {
      auto* first=memory.TranslatePhysical<uint8_t*>(range.address);
      const auto start=reinterpret_cast<uintptr_t>(first)&~uintptr_t(page_size-1);
      const auto end=(reinterpret_cast<uintptr_t>(first)+range.length+page_size-1)&~uintptr_t(page_size-1);
      for (auto cursor=start;cursor<end;) {
        size_t available=0;
        rex::memory::PageAccess access;
        if (!rex::memory::QueryProtect(reinterpret_cast<void*>(cursor),available,access) || !available) {
          Restore();return;
        }
        const size_t length=std::min(size_t(end-cursor),available);
        if (access==rex::memory::PageAccess::kReadOnly) {
          // Plan every region before the first protection change. Allocation
          // failure during construction then cannot leave an opened window.
          changed_.push_back({reinterpret_cast<void*>(cursor),length,access});
        } else if (access!=rex::memory::PageAccess::kReadWrite) {
          Restore();return;
        }
        cursor+=length;
      }
    }
    for (const auto& item:changed_) {
      if (!rex::memory::Protect(item.address,item.length,rex::memory::PageAccess::kReadWrite)) {
        Restore();return;
      }
      ++opened_;
    }
    valid_=true;
  }
  PhysicalWriteScope(const PhysicalWriteScope&)=delete;
  PhysicalWriteScope& operator=(const PhysicalWriteScope&)=delete;
  ~PhysicalWriteScope(){Restore();}
  explicit operator bool()const{return valid_;}
};

inline std::unique_ptr<PhysicalWriteScope> OpenPhysicalWriteback(
    rex::memory::Memory* memory,std::span<const PhysicalCopyRange> ranges) {
  if (!memory) return {};
  auto result=std::make_unique<PhysicalWriteScope>(*memory,ranges);
  return *result ? std::move(result) : nullptr;
}
} // namespace legodimensions::gpu_native
