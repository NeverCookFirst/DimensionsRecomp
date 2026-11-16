#pragma once

#include "gpu_native/native_recording.h"
#include <atomic>
#include <list>
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace legodimensions::gpu_native {

// Only the final recording stream reads/writes this state. Preparing a tape
// never claims that its texture copy has already reached an earlier consumer.
struct TextureUploadReplayState { uint64_t version = 0; };

class TextureUploadPublication final : public std::enable_shared_from_this<TextureUploadPublication> {
  inline static std::atomic<uint64_t> next_version_{1};
  const uint64_t version_ = next_version_.fetch_add(1, std::memory_order_relaxed);
  std::shared_ptr<TextureUploadReplayState> state_;
  std::unique_ptr<NativeDrawCommands> copy_;
  // The consumer must also retain this publication through its frame fence.
  // The tape itself owns raw GPU addresses, not their allocation lifetimes.
  std::shared_ptr<void> staging_;
  uint64_t bytes_;

  void Publish(plume::RenderCommandList* commands) {
    if (auto* deferred = dynamic_cast<NativeDeferredCommands*>(commands)) {
      // Draw tapes may replay into another deferred recording tape. Publish
      // at the final stream, rather than advancing state at the outer layer.
      deferred->ResourceCommand([self = shared_from_this()](auto* list) { self->Publish(list); });
      return;
    }
    if (state_->version == version_) return;
    copy_->Replay(commands);
    state_->version = version_;
  }
 public:
  TextureUploadPublication(std::shared_ptr<TextureUploadReplayState> state,
      std::unique_ptr<NativeDrawCommands> copy, std::shared_ptr<void> staging, uint64_t bytes)
      : state_(std::move(state)), copy_(std::move(copy)), staging_(std::move(staging)), bytes_(bytes) {
    if (!state_ || !copy_ || !staging_ || !bytes_ || !version_)
      throw std::invalid_argument("Invalid retained texture upload");
  }
  uint64_t Bytes() const { return bytes_; }
  void Enqueue(plume::RenderCommandList* commands) {
    if (!commands) throw std::invalid_argument("Missing texture upload consumer");
    Publish(commands);
  }
};

// A host resolve/alias transfer overwrites the same GPU allocation. Invalidate
// in that write's stream, including when the stream is deferred more than once.
inline void InvalidateTextureUploadReplay(plume::RenderCommandList* commands,
    const std::shared_ptr<TextureUploadReplayState>& state) {
  if (!commands || !state) return;
  if (auto* deferred = dynamic_cast<NativeDeferredCommands*>(commands))
    deferred->ResourceCommand([state](auto* list) { InvalidateTextureUploadReplay(list, state); });
  else state->version = 0;
}

// Resources keep weak references. A bounded LRU retains reusable staging;
// queued consumers/fence leases independently keep evicted versions alive.
// Expiry is a cache miss and causes a fresh decode, never an uninitialized bind.
class TextureUploadPublicationCache {
  std::mutex mutex_;
  using Entries = std::list<std::shared_ptr<TextureUploadPublication>>;
  Entries entries_;
  std::unordered_map<const TextureUploadPublication*, Entries::iterator> index_;
  uint64_t bytes_ = 0;
  const uint64_t budget_;
 public:
  explicit TextureUploadPublicationCache(uint64_t budget) : budget_(budget) {}
  void Keep(std::shared_ptr<TextureUploadPublication> publication) {
    if (!publication || publication->Bytes() > budget_) return;
    Entries retired;
    {
      std::lock_guard lock(mutex_);
      if (const auto found = index_.find(publication.get()); found != index_.end()) {
        entries_.splice(entries_.end(), entries_, found->second);
        return;
      }
      while (!entries_.empty() && bytes_ > budget_ - publication->Bytes()) {
        bytes_ -= entries_.front()->Bytes();
        index_.erase(entries_.front().get());
        retired.splice(retired.end(), entries_, entries_.begin());
      }
      entries_.push_back(std::move(publication));
      const auto added = std::prev(entries_.end());
      try { index_.emplace(added->get(), added); }
      catch (...) { retired.splice(retired.end(), entries_, added); throw; }
      bytes_ += (*added)->Bytes();
    }
    // GPU allocation destructors run outside the cache mutex.
  }
  // A hit retains no new bytes and allocates no list/index node. Promote it
  // before cold streaming uploads evict staging the next draw will reuse.
  bool Touch(const std::shared_ptr<TextureUploadPublication>& publication) {
    if (!publication) return false;
    std::lock_guard lock(mutex_);
    const auto found = index_.find(publication.get());
    if (found == index_.end()) return false;
    entries_.splice(entries_.end(), entries_, found->second);
    return true;
  }
  void Reset() {
    Entries retired;
    { std::lock_guard lock(mutex_); entries_.swap(retired); index_.clear(); bytes_ = 0; }
  }
  uint64_t Bytes() { std::lock_guard lock(mutex_); return bytes_; }
};
}  // namespace legodimensions::gpu_native
