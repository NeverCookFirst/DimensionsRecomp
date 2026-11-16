#pragma once
// One-shot GPU pass profile. F10 arms it at a Swap; every draw/clear/resolve
// of the following frame is bracketed by GPU timestamps. At the next Swap the
// GPU is drained, the timestamps are read and the passes are logged grouped by
// (kind, VS, PS, viewport), costliest first. Costs one stalled frame per
// capture; nothing is recorded otherwise.
#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <map>
#include <memory>
#include <tuple>
#include <vector>

#include <plume_d3d12.h>
#include <rex/logging.h>

#include "gpu_native/device.h"

namespace legodimensions::gpu_native::pass_profile {

struct Key {
  const char* kind;
  uint64_t vs, ps;
  uint32_t width, height;
  bool operator<(const Key& o) const {
    return std::tie(kind, vs, ps, width, height) < std::tie(o.kind, o.vs, o.ps, o.width, o.height);
  }
};

constexpr uint32_t kMaxPairs = 8192;

struct State {
  std::unique_ptr<plume::RenderQueryPool> pool;
  std::vector<Key> keys;
  bool capturing = false;
  bool key_was_down = false;
};

inline State& Get() {
  static State state;
  return state;
}

// Caller drained GPU submissions and still owns the creating device.
inline void Reset() {
  auto& s = Get();
  s.capturing = false;
  s.key_was_down = false;
  s.keys.clear();
  s.pool.reset();
}

// Returns the timestamp index for the pass start, or ~0u when not capturing.
inline uint32_t Begin(plume::RenderCommandList* commands) {
  auto& s = Get();
  if (!s.capturing || !commands || s.keys.size() >= kMaxPairs) return ~0u;
  const uint32_t index = uint32_t(s.keys.size()) * 2;
  commands->writeTimestamp(s.pool.get(), index);
  return index;
}

inline void End(plume::RenderCommandList* commands, uint32_t index, const Key& key) {
  auto& s = Get();
  if (index == ~0u || !s.capturing || !commands) return;
  commands->writeTimestamp(s.pool.get(), index + 1);
  s.keys.push_back(key);
}

// Called at every Swap with the recording lock held.
inline void OnSwap() {
  auto& s = Get();
  if (s.capturing) {
    s.capturing = false;
    HostDevice::Synchronize(SyncReason::kOther);
    auto* pool = static_cast<plume::D3D12QueryPool*>(s.pool.get());
    pool->queryResults();
    const uint64_t* ns = pool->getResults();
    struct Total { double ms = 0; uint32_t count = 0; };
    std::map<Key, Total> totals;
    double frame_ms = 0;
    for (size_t i = 0; i < s.keys.size(); ++i) {
      const uint64_t a = ns[i * 2], b = ns[i * 2 + 1];
      const double ms = b > a ? double(b - a) / 1e6 : 0.0;
      auto& t = totals[s.keys[i]];
      t.ms += ms;
      ++t.count;
      frame_ms += ms;
    }
    std::vector<std::pair<Key, Total>> sorted(totals.begin(), totals.end());
    std::sort(sorted.begin(), sorted.end(),
              [](const auto& x, const auto& y) { return x.second.ms > y.second.ms; });
    REXLOG_INFO("GPU pass profile: {} passes, {:.2f} ms summed GPU time", s.keys.size(), frame_ms);
    for (size_t i = 0; i < sorted.size() && i < 40; ++i) {
      const auto& [k, t] = sorted[i];
      REXLOG_INFO("GPU pass #{:2} {:6.2f} ms {:5.1f}% x{:4} {:8} {}x{} VS={:016X} PS={:016X}",
                  i + 1, t.ms, 100.0 * t.ms / std::max(frame_ms, 1e-9), t.count, k.kind,
                  k.width, k.height, k.vs, k.ps);
    }
    s.keys.clear();
  }
  // Bit 0 = pressed since the previous call, so a short tap between two
  // (rare, during loading) Swaps is not missed.
  const SHORT key = GetAsyncKeyState(VK_F10);
  const bool down = (key & 0x8000) != 0;
  if ((down && !s.key_was_down) || (key & 1)) {
    if (!s.pool) {
      if (auto* device = HostDevice::Device()) s.pool = device->createQueryPool(kMaxPairs * 2);
    }
    if (s.pool) {
      s.keys.clear();
      s.keys.reserve(kMaxPairs);
      s.capturing = true;
      REXLOG_INFO("GPU pass profile: capturing the next frame");
      Beep(660, 80);
    }
  }
  s.key_was_down = down;
}

}  // namespace legodimensions::gpu_native::pass_profile
