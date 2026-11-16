#pragma once

#include <array>
#include <atomic>
#include <bit>
#include <cstdint>
#include <memory>
#include <vector>

namespace legodimensions::gpu_native {
template<class Work, size_t Count = 64>
class DrawWorkPool {
  struct Chunk { std::array<Work, Count> items; };
  std::vector<std::shared_ptr<Chunk>> pool_;
  std::shared_ptr<Chunk> chunk_;
  size_t next_ = Count, cursor_ = 0;
 public:
  std::shared_ptr<Work> Acquire() {
    if (next_ == Count) {
      chunk_.reset();
      for (size_t checked = 0; checked < pool_.size(); ++checked) {
        cursor_ = (cursor_ + 1) % pool_.size();
        if (pool_[cursor_].use_count() == 1) {
          // Another thread may have reset these works just before dropping its reference.
          std::atomic_thread_fence(std::memory_order_acquire);
          chunk_ = pool_[cursor_];
          break;
        }
      }
      if (!chunk_) {
        chunk_ = std::make_shared<Chunk>();
        if (pool_.size() < 256) pool_.push_back(chunk_);
      }
      next_ = 0;
    }
    auto result = std::shared_ptr<Work>(chunk_, &chunk_->items[next_++]);
    result->Reset();
    return result;
  }
  // The next Acquire's work was last written on another core; fetch it for writing ahead of use.
  void PrefetchNext() const {
    if (!chunk_ || next_ >= Count) return;
    const auto* bytes = reinterpret_cast<const char*>(&chunk_->items[next_]);
    for (size_t offset = 0; offset < sizeof(Work); offset += 64) __builtin_prefetch(bytes + offset, 1, 3);
  }
};

class DrawResourceLease {
  std::vector<std::shared_ptr<void>> owners_;
 public:
  template <size_t Slots>
  class BasicBuilder {
    static_assert((Slots & (Slots - 1)) == 0);
    std::array<const void*, Slots> seen_{};
    std::shared_ptr<DrawResourceLease> lease_ = std::make_shared<DrawResourceLease>();
   public:
    bool Seen(const void* address) {
      size_t slot = ((reinterpret_cast<uintptr_t>(address) >> 4) * 11400714819323198485ull) >>
          (64 - std::countr_zero(Slots));
      for (size_t probe = 0; probe < seen_.size(); ++probe, slot = (slot + 1) & (Slots - 1)) {
        if (seen_[slot] == address) return true;
        if (!seen_[slot]) { seen_[slot] = address; return false; }
      }
      return false;
    }
    void Add(const std::shared_ptr<void>& owner) {
      if (!owner || Seen(owner.get())) return;
      lease_->owners_.push_back(owner);
    }
    // Takes the reference instead of copying it: no refcount traffic on the caller.
    template<class T>
    void Take(std::shared_ptr<T>& owner) {
      if (!owner) return;
      if (Seen(owner.get())) { owner.reset(); return; }
      lease_->owners_.push_back(std::move(owner));
    }
    std::shared_ptr<DrawResourceLease> Finish() { return std::move(lease_); }
  };
  using Builder = BasicBuilder<4096>;
  // One preparation chunk holds a few hundred owners at most.
  using ChunkBuilder = BasicBuilder<1024>;
};
}
