#pragma once
#include <algorithm>
#include <bit>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace legodimensions::gpu_native {
// Acquire and Reset are serialized by the recording owner. A last-owner
// deleter may run on another thread. Only unused immutable buffer versions
// enter this pool: the caller must retain submitted versions through its GPU
// fences, exactly as it would without pooling.
template <typename Buffer>
class UploadBufferPool {
 public:
  struct Counters {uint64_t hits=0,misses=0,bytes=0,count=0;};
  template <typename Create>
  std::shared_ptr<Buffer> Acquire(uint32_t length,uint8_t kind,Create create) {
    if (!length) return {};
    const uint64_t capacity=std::bit_ceil(std::max(uint64_t(length),uint64_t{256}));
    const uint64_t key=(capacity<<8)|kind;
    const uint64_t charge=std::max(capacity,uint64_t{65536});
    auto state=state_;
    std::unique_ptr<Buffer> buffer;
    {
      std::lock_guard lock(state->mutex);
      const auto found=state->free.find(key);
      if (found!=state->free.end() && !found->second.empty()) {
        buffer=std::move(found->second.back());found->second.pop_back();
        state->stats.bytes-=charge;--state->stats.count;++state->stats.hits;
        if(found->second.empty())state->free.erase(found);
      } else ++state->stats.misses;
    }
    if (!buffer) buffer=create(capacity);
    if (!buffer) return {};
    return std::shared_ptr<Buffer>(buffer.release(),[state,key,charge](Buffer* pointer) {
      std::unique_ptr<Buffer> owner(pointer);
      std::lock_guard lock(state->mutex);
      if (!state->alive || charge>kBudget || state->stats.bytes>kBudget-charge ||
          state->stats.count>=4096) return;
      auto& free=state->free[key];
      if (free.size()>=256) return;
      free.push_back(std::move(owner));state->stats.bytes+=charge;++state->stats.count;
    });
  }
  Counters Stats() const {std::lock_guard lock(state_->mutex);return state_->stats;}
  void Reset() {
    {
      std::lock_guard lock(state_->mutex);
      state_->alive=false;state_->free.clear();state_->stats.bytes=state_->stats.count=0;
    }
    // Older in-flight owners retain the dead state. Releasing them after a
    // device reset cannot place an old device's buffer into the new pool.
    state_=std::make_shared<State>();
  }
 private:
  static constexpr uint64_t kBudget=192*1024*1024;
  struct State {
    std::mutex mutex;
    bool alive=true;
    Counters stats;
    std::unordered_map<uint64_t,std::vector<std::unique_ptr<Buffer>>> free;
  };
  std::shared_ptr<State> state_=std::make_shared<State>();
};
}
