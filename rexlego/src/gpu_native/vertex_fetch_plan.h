#pragma once

#include "gpu_native/vertex_fetch_range.h"
#include "gpu_native/vertex_layout_plan.h"

#include <array>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <optional>
#include <unordered_map>
#include <utility>

namespace legodimensions::gpu_native {

// Immutable shader/declaration facts only; buffer lengths, index bounds, bindings and stamps are checked per draw.
struct VertexFetchPlan {
  struct Stream {
    uint64_t attribute_end = 0;
    bool required = false, widths_valid = true;
  };
  VertexLayoutPlan layout;
  std::array<Stream, 16> reads{};
  std::array<bool, 16> upload_streams{};
};

template<class Declaration, class FormatSize>
VertexFetchPlan PrepareVertexFetchPlan(const Declaration& declaration,
    uint64_t required_inputs, bool reduce_upload_streams, FormatSize format_size) {
  VertexFetchPlan result;
  if (declaration.element_count > 64) { result.layout.valid = false; return result; }
  result.layout = PrepareVertexLayout(declaration, format_size);
  if (!result.layout.valid) return result;
  for (uint32_t slot = 0; slot < result.upload_streams.size(); ++slot)
    result.upload_streams[slot] = !reduce_upload_streams && result.layout.streams[slot].active;
  for (uint32_t e = 0; e < declaration.element_count; ++e) {
    if (!(required_inputs & (uint64_t{1} << e))) continue;
    const auto& element = declaration.elements[e];
    auto& stream = result.reads[element.slotIndex];
    const uint32_t width = format_size(element.format);
    stream.required = true;
    stream.widths_valid &= width != 0;
    stream.attribute_end = std::max(stream.attribute_end,
        uint64_t(element.alignedByteOffset) + width);
    if (reduce_upload_streams) result.upload_streams[element.slotIndex] = true;
  }
  return result;
}

inline bool VertexFetchStreamFits(const VertexFetchPlan::Stream& stream,
    uint32_t minimum, uint32_t maximum, int32_t base, uint32_t stride,
    uint32_t stream_offset, uint32_t buffer_bytes) {
  if (!stream.required) return true;
  // The greatest attribute end fitting equals every attribute fitting; the sum stays wide against overflow.
  if (!stream.widths_valid || stream.attribute_end > stride) return false;
  return VertexFetchRangeFits(minimum, maximum, base, stride, stream_offset,
      0, uint32_t(stream.attribute_end), buffer_bytes);
}

inline std::atomic<bool>& VertexFetchPlanCacheLruState() {
  static std::atomic<bool> enabled{[] {
    const char* value = std::getenv("LEGO_NATIVE_VERTEX_FETCH_PLAN_LRU");
    return value && std::strcmp(value, "0") != 0;
  }()};
  return enabled;
}
inline bool VertexFetchPlanCacheLruEnabled() {
  return VertexFetchPlanCacheLruState().load(std::memory_order_relaxed);
}

class VertexFetchPlanCache {
 public:
  struct Key {
    uint64_t shader = 0, declaration = 0;
    uint64_t reversed = 0, half_swapped = 0, half_swapped_wide = 0;
    uint64_t shader_generation = 0;
    uint32_t specialization = 0, elements = 0;
    bool reduce_unused = false;
    bool operator==(const Key&) const = default;
  };
  struct Stats { uint64_t hits = 0, misses = 0, uncached = 0, evicted = 0; };
  explicit VertexFetchPlanCache(size_t limit = 128) : limit_(limit) {}

  template<class Prepare>
  const std::shared_ptr<const VertexFetchPlan>& Resolve(const Key& key, Prepare prepare,
      bool per_entry = VertexFetchPlanCacheLruEnabled()) {
    const uint64_t used = ++clock_;
    if (const auto found = entries_.find(key); found != entries_.end()) {
      ++stats_.hits;
      found->second.used = used;
      return found->second.plan;
    }
    ++stats_.misses;
    auto plan = prepare();
    // An additive pack may publish a missing shader later, so absence is never cached.
    if (!plan || !key.shader || !key.declaration || key.elements > 64 || !limit_) {
      ++stats_.uncached;
      transient_ = std::move(plan);
      return transient_;
    }
    // Work items alias the immutable layout through this same shared owner,
    // so evicting a cache entry cannot invalidate preparation already queued.
    if (entries_.size() >= limit_) {
      if (per_entry) {
        // Evicts the least recently used pair; only misses scan the bounded table.
        const auto oldest = std::min_element(entries_.begin(), entries_.end(),
            [](const auto& a, const auto& b) { return a.second.used < b.second.used; });
        entries_.erase(oldest);
        ++stats_.evicted;
      } else {
        stats_.evicted += entries_.size();
        entries_.clear();
      }
    }
    return entries_.emplace(key, Entry{std::move(plan), used}).first->second.plan;
  }
  Stats TakeStats() { return std::exchange(stats_, {}); }
  size_t Size() const { return entries_.size(); }

 private:
  struct Entry { std::shared_ptr<const VertexFetchPlan> plan; uint64_t used; };
  struct Hash {
    size_t operator()(const Key& key) const {
      uint64_t hash = key.shader ^ (key.declaration << 17) ^ (key.declaration >> 47);
      for (const uint64_t value : {key.reversed, key.half_swapped, key.half_swapped_wide,
          key.shader_generation, (uint64_t(key.specialization) << 32) | key.elements})
        hash = (hash ^ value) * 0x9E3779B185EBCA87ull;
      return size_t(hash ^ uint64_t(key.reduce_unused));
    }
  };
  size_t limit_;
  uint64_t clock_ = 0;
  Stats stats_;
  std::unordered_map<Key, Entry, Hash> entries_;
  std::shared_ptr<const VertexFetchPlan> transient_;
};

inline VertexFetchPlanCache& ThreadVertexFetchPlans() {
  static thread_local VertexFetchPlanCache cache;
  return cache;
}

// Policy changes only alter eviction, so queued work needs no drain.
inline std::optional<bool> PollVertexFetchPlanCacheLru() {
  static const char* path = std::getenv("LEGO_NATIVE_VERTEX_FETCH_PLAN_LRU_FILE");
  if (!path || !*path) return {};
  using Clock = std::chrono::steady_clock;
  static Clock::time_point next;
  static bool acknowledged = false;
  const auto now = Clock::now();
  if (now < next) return {};
  next = now + std::chrono::milliseconds(500);
  std::ifstream input(path);
  char value = 0, extra = 0;
  if (!(input >> value) || (value != '0' && value != '1') || (input >> extra)) return {};
  const bool enabled = value == '1';
  const bool previous = VertexFetchPlanCacheLruState().exchange(enabled, std::memory_order_relaxed);
  if (acknowledged && previous == enabled) return {};
  acknowledged = true;
  return enabled;
}

inline std::atomic<bool>& VertexFetchPlanEnabledState() {
  static std::atomic<bool> enabled{[] {
    const char* value = std::getenv("LEGO_NATIVE_VERTEX_FETCH_PLAN");
    return value && std::strcmp(value, "0") != 0;
  }()};
  return enabled;
}
inline bool VertexFetchPlanEnabled() {
  return VertexFetchPlanEnabledState().load(std::memory_order_relaxed);
}
// Called only by the swap coordinator; no file operations occur in draw work.
inline std::optional<bool> PollVertexFetchPlan() {
  static const char* path = std::getenv("LEGO_NATIVE_VERTEX_FETCH_PLAN_FILE");
  if (!path || !*path) return {};
  using Clock = std::chrono::steady_clock;
  static Clock::time_point next;
  static bool acknowledged = false;
  const auto now = Clock::now();
  if (now < next) return {};
  next = now + std::chrono::milliseconds(500);
  std::ifstream input(path);
  char value = 0, extra = 0;
  if (!(input >> value) || (value != '0' && value != '1') || (input >> extra)) return {};
  const bool enabled = value == '1';
  const bool previous = VertexFetchPlanEnabledState().exchange(enabled, std::memory_order_relaxed);
  if (acknowledged && previous == enabled) return {};
  acknowledged = true;
  return enabled;
}
}  // namespace legodimensions::gpu_native
