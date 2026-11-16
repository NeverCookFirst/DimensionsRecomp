#pragma once

#include <cassert>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace legodimensions::gpu_native {

// Cache misses for different PSOs may enter the driver concurrently. A miss
// for the same key must recheck the cache after taking its lease, so only one
// worker creates/publishes that PSO. Do not hold the cache mutex while waiting
// for a lease. Driver failures and exceptions release the lease normally.
template <class Key, class Hash = std::hash<Key>>
class PipelineCreationGate {
 public:
  class Lease {
   public:
    explicit Lease(std::shared_ptr<std::mutex> mutex)
        : mutex_(std::move(mutex)), lock_(*mutex_) {}
    Lease(Lease&&) = default;
    Lease& operator=(Lease&&) = delete;

   private:
    // The lock must be destroyed before its mutex owner.
    std::shared_ptr<std::mutex> mutex_;
    std::unique_lock<std::mutex> lock_;
  };

  Lease Acquire(const Key& key) {
    std::shared_ptr<std::mutex> mutex;
    {
      std::lock_guard lock(mutex_);
      auto& entry = keys_[key];
      if (!entry) entry = std::make_shared<std::mutex>();
      mutex = entry;
    }
    return Lease(std::move(mutex));
  }

  // Device teardown joins preparation workers before clearing either cache.
  void ClearAfterDrain() {
    std::lock_guard lock(mutex_);
    for (const auto& [key, mutex] : keys_) assert(mutex.use_count() == 1);
    keys_.clear();
  }

 private:
  std::mutex mutex_;
  std::unordered_map<Key, std::shared_ptr<std::mutex>, Hash> keys_;
};

}  // namespace legodimensions::gpu_native
