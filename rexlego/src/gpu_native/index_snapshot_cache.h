#pragma once

#include "gpu_native/index_bounds.h"
#include "gpu_native/memory_watch.h"
#include <memory>
#include <unordered_map>

namespace legodimensions::gpu_native {
struct IndexBoundsSnapshot {
  CpuMemoryStamp stamp;
  IndexBounds bounds;
};

class IndexSnapshotCache {
 public:
  template <typename Capture>
  std::shared_ptr<const IndexBoundsSnapshot> Get(uint32_t offset, uint32_t length,
      bool reuse, Capture&& capture) {
    const uint64_t key = (uint64_t(offset) << 32) | length;
    if (reuse) {
      const auto found = entries_.find(key);
      if (found != entries_.end() && CpuMemoryUnchanged(found->second->stamp))
        return found->second;
    }
    auto snapshot = capture();
    if (!reuse || !snapshot || snapshot->stamp.pages.empty()) return snapshot;
    if (entries_.size() >= 4096 || stamp_bytes_ >= 8u * 1024 * 1024) {
      entries_.clear();
      stamp_bytes_ = 0;
    }
    auto& entry = entries_[key];
    if (entry) stamp_bytes_ -= StampBytes(*entry);
    stamp_bytes_ += StampBytes(*snapshot);
    entry = snapshot;
    return snapshot;
  }

 private:
  static size_t StampBytes(const IndexBoundsSnapshot& snapshot) {
    return snapshot.stamp.pages.capacity() * sizeof(snapshot.stamp.pages[0]) +
        snapshot.stamp.regions.capacity() * sizeof(snapshot.stamp.regions[0]);
  }
  std::unordered_map<uint64_t, std::shared_ptr<const IndexBoundsSnapshot>> entries_;
  size_t stamp_bytes_ = 0;
};
}
