#pragma once
#include "gpu_native/native_recording.h"
#include <functional>
#include <atomic>
#include <stdexcept>
#include <vector>

namespace legodimensions::gpu_native {
// Each ownership handoff inherits earlier eDRAM bytes once. Every consumer
// enqueues its retained handoff, so an abandoned producer cannot strand it.
class ColorAliasInitialization final : public std::enable_shared_from_this<ColorAliasInitialization> {
  std::atomic<bool> initialized_{false};
  std::unique_ptr<NativeDrawCommands> copy_;
  std::vector<std::shared_ptr<void>> owners_;
  std::function<bool()> validate_;
  std::function<void(std::shared_ptr<void>)> retire_;
  const std::vector<uint32_t> dependencies_;
  std::shared_ptr<ColorAliasInitialization> parent_;
  ColorAliasInitialization* deletion_next_ = nullptr;
  static void Destroy(ColorAliasInitialization* node) noexcept {
    // Shared-pointer reference counts reach zero normally. Only their delete
    // operations are queued, so no live node or weak-lock operation is raced.
    static thread_local ColorAliasInitialization* head=nullptr;
    static thread_local bool active=false;
    node->deletion_next_=head;head=node;
    if (active) return;
    active=true;
    while (head) {auto* current=head;head=current->deletion_next_;delete current;}
    active=false;
  }
  void Complete() {
    for (auto& owner : owners_) retire_(std::move(owner));
    owners_.clear();copy_.reset();validate_={};retire_={};parent_.reset();
    initialized_.store(true,std::memory_order_release);
  }
  void Publish(plume::RenderCommandList* commands) {
    if (auto* deferred = dynamic_cast<NativeDeferredCommands*>(commands)) {
      deferred->ResourceCommand([self=shared_from_this()](auto* final) { self->Publish(final); });
      return;
    }
    // A stalled recorder can accumulate thousands of handoffs. Neither replay
    // nor cancellation may recurse through that chain on a bounded stack.
    std::vector<std::shared_ptr<ColorAliasInitialization>> chain;
    for (auto node=shared_from_this();node && !node->initialized_.load(std::memory_order_acquire);node=node->parent_)
      chain.push_back(node);
    for (auto it=chain.rbegin();it!=chain.rend();++it) {
      auto& node=**it;
      if (!node.validate_()) throw std::runtime_error("Color alias storage changed after capture");
      node.copy_->Replay(commands);
      // The source allocations survive the actual recording's frame fence.
      node.Complete();
    }
  }
  ColorAliasInitialization(std::unique_ptr<NativeDrawCommands> copy,
      std::vector<std::shared_ptr<void>> owners, std::function<bool()> validate,
      std::function<void(std::shared_ptr<void>)> retire, std::vector<uint32_t> dependencies,
      std::shared_ptr<ColorAliasInitialization> parent)
      : copy_(std::move(copy)),owners_(std::move(owners)),validate_(std::move(validate)),retire_(std::move(retire)),dependencies_(std::move(dependencies)),parent_(std::move(parent)) {
    if (!copy_ || !validate_ || !retire_) throw std::invalid_argument("Invalid color alias initialization");
  }
 public:
  static std::shared_ptr<ColorAliasInitialization> Create(std::unique_ptr<NativeDrawCommands> copy,
      std::vector<std::shared_ptr<void>> owners, std::function<bool()> validate,
      std::function<void(std::shared_ptr<void>)> retire, std::vector<uint32_t> dependencies,
      std::shared_ptr<ColorAliasInitialization> parent) {
    return std::shared_ptr<ColorAliasInitialization>(new ColorAliasInitialization(std::move(copy),
        std::move(owners),std::move(validate),std::move(retire),std::move(dependencies),std::move(parent)),Destroy);
  }
  void Enqueue(plume::RenderCommandList* commands) {
    if (!commands) throw std::invalid_argument("Missing color alias consumer");
    // A handoff commits once and never becomes pending again. Unlike an
    // upload version, this monotonic state cannot change back before replay.
    if (initialized_.load(std::memory_order_acquire)) return;
    Publish(commands);
  }
  std::vector<uint32_t> Dependencies() const {
    return initialized_.load(std::memory_order_acquire) ? std::vector<uint32_t>{} : dependencies_;
  }
  void Overwritten(plume::RenderCommandList* commands) {
    if (!commands) throw std::invalid_argument("Missing color overwrite stream");
    if (auto* deferred=dynamic_cast<NativeDeferredCommands*>(commands))
      deferred->ResourceCommand([self=shared_from_this()](auto* final){self->Overwritten(final);});
    else if (!initialized_) Complete();
  }
};
} // namespace legodimensions::gpu_native
