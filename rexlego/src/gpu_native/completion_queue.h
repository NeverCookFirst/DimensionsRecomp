#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <utility>

namespace legodimensions::gpu_native {

// One graphics queue has a monotonically ordered submission timeline. The
// caller serializes access; popping never invokes guest code under its mutex.
class CompletionQueue {
 public:
  void Push(std::uint64_t submission, std::function<void()> callback) {
    entries_.push_back({submission, std::move(callback)});
  }
  std::function<void()> PopReady(std::uint64_t completed) {
    if (entries_.empty() || entries_.front().submission > completed) return {};
    auto callback = std::move(entries_.front().callback);
    entries_.pop_front();
    return callback;
  }
  std::size_t size() const { return entries_.size(); }

 private:
  struct Entry { std::uint64_t submission; std::function<void()> callback; };
  std::deque<Entry> entries_;
};

}  // namespace legodimensions::gpu_native
