#pragma once
#include <array>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstddef>

namespace legodimensions::gpu_native {
// Average actual completed-present intervals, not instantaneous reciprocal FPS.
// Keep a full second of samples and refresh digits ten times per second.
// The frame-time graph uses the latest complete interval, without averaging.
class NativeFpsCounter {
 public:
  void RecordPresent(double now) {
    if (!std::isfinite(now)) return;
    if (!started_ || now <= previous_) {
      *this = {};
      started_ = true;
      previous_ = displayed_at_ = now;
      return;
    }
    const double interval = now - previous_;
    last_interval_ = interval;
    previous_ = now;
    if (count_ == intervals_.size()) Pop();
    intervals_[(first_ + count_) % intervals_.size()] = interval;
    ++count_;
    seconds_ += interval;
    // Include the complete long frame even when it crosses the window edge.
    while (count_ > 1 && seconds_ - intervals_[first_] >= 1.0) Pop();
    if (now - displayed_at_ >= 0.1) {
      value_ = uint32_t(std::clamp(double(count_) / seconds_ + 0.5, 0.0, 9999.0));
      displayed_at_ = now;
    }
  }
  uint32_t Value() const { return value_; }
  double FrameTimeMilliseconds() const { return last_interval_ * 1000.0; }

 private:
  void Pop() {
    seconds_ -= intervals_[first_];
    first_ = (first_ + 1) % intervals_.size();
    --count_;
  }
  std::array<double, 512> intervals_{};
  std::size_t first_ = 0, count_ = 0;
  double seconds_ = 0, previous_ = 0, displayed_at_ = 0, last_interval_ = 0;
  uint32_t value_ = 0;
  bool started_ = false;
};
}  // namespace legodimensions::gpu_native
