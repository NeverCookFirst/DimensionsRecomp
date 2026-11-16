#pragma once
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <utility>

namespace legodimensions::gpu_native {
// Nonindexed QuadList expansion depends only on its first vertex and count.
// Buffers are immutable. Eviction transfers ownership to the caller's GPU
// retirement queue; Clear is reserved for device shutdown after draining it.
template <typename Buffer>
class QuadIndexCache {
 public:
  std::shared_ptr<Buffer> Find(uint32_t first, uint32_t count) const {
    const auto it = entries_.find(Key(first,count));
    return it == entries_.end() ? nullptr : it->second.buffer;
  }
  template <typename Retire>
  void Remember(uint32_t first, uint32_t count, uint64_t bytes,
                std::shared_ptr<Buffer> buffer, Retire retire) {
    constexpr uint64_t budget = 16 * 1024 * 1024;
    if (!buffer || bytes > budget) return;
    const auto key = Key(first,count);
    if (entries_.contains(key)) return;
    while (!entries_.empty() && (entries_.size() >= 256 || bytes_ + bytes > budget)) {
      auto old = entries_.begin();
      bytes_ -= old->second.bytes;
      retire(std::move(old->second.buffer));
      entries_.erase(old);
    }
    entries_.emplace(key,Entry{std::move(buffer),bytes});
    bytes_ += bytes;
  }
  void Clear() {entries_.clear();bytes_=0;}
 private:
  static uint64_t Key(uint32_t first,uint32_t count) {return (uint64_t(first)<<32)|count;}
  struct Entry {std::shared_ptr<Buffer> buffer;uint64_t bytes;};
  std::unordered_map<uint64_t,Entry> entries_;
  uint64_t bytes_ = 0;
};
}
