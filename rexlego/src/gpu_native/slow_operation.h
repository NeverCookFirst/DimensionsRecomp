#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <rex/logging.h>

namespace legodimensions::gpu_native {

// Opt-in CPU timings for transition stalls. No GPU readback, per-frame file,
// allocation or clock reads when disabled. Bound log traffic for long sessions.
class SlowNativeOperation {
 public:
  explicit SlowNativeOperation(const char* label) : label_(label) {
    static const bool enabled = [] {
      const char* value = std::getenv("LEGO_NATIVE_SLOW_OPERATIONS");
      return value && *value && std::strcmp(value, "0") != 0;
    }();
    active_ = enabled && logged_.load(std::memory_order_relaxed) < 128;
    if (active_) start_ = previous_ = Clock::now();
  }

  void Mark(const char* phase) {
    if (!active_ || phases_ == names_.size()) return;
    const auto now = Clock::now();
    names_[phases_] = phase;
    times_[phases_++] = Milliseconds(now - previous_);
    previous_ = now;
  }

  ~SlowNativeOperation() {
    if (!active_) return;
    const auto end = Clock::now();
    const double total = Milliseconds(end - start_);
    if (total < 4.0 || logged_.fetch_add(1, std::memory_order_relaxed) >= 128) return;
    REXLOG_INFO("Native slow CPU operation: {} total_ms={:.3f} "
        "{}={:.3f} {}={:.3f} {}={:.3f} {}={:.3f} tail_ms={:.3f}",
        label_, total, names_[0], times_[0], names_[1], times_[1],
        names_[2], times_[2], names_[3], times_[3],
        Milliseconds(end - previous_));
  }

 private:
  using Clock = std::chrono::steady_clock;
  static double Milliseconds(Clock::duration duration) {
    return std::chrono::duration<double, std::milli>(duration).count();
  }
  inline static std::atomic<unsigned> logged_{0};
  const char* label_;
  bool active_ = false;
  unsigned phases_ = 0;
  Clock::time_point start_{}, previous_{};
  std::array<const char*, 4> names_{"unused", "unused", "unused", "unused"};
  std::array<double, 4> times_{};
};

}  // namespace legodimensions::gpu_native
