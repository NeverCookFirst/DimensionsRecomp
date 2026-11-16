#pragma once
#include <array>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

namespace legodimensions::gpu_native {
class ColorAliasInitialization;
struct ColorAliasDecision {
  std::shared_ptr<void> source, destination;
  const void* source_texture = nullptr;
  const void* destination_texture = nullptr;
  uint32_t source_address = 0;
  std::shared_ptr<ColorAliasInitialization> initialization;
  std::vector<uint32_t> source_dependencies;
};
using ColorAliasDecisions = std::array<ColorAliasDecision, 4>;

// Only the known 32-bit, single-sample Xenos layout is transferred. Other
// writes invalidate authority instead of interpreting incompatible raw data.
struct ColorAliasLayout {
  uint32_t base = ~0u, width = 0, height = 0;
  uint32_t host_width = 0, host_height = 0;
  bool supported = false;
  bool range_known = true;
  bool operator==(const ColorAliasLayout&) const = default;
  uint32_t Tiles() const {
    if (!range_known || !width || !height || width > 16384 || height > 16384)
      return 0;
    return ((width + 79) / 80) * ((height + 15) / 16);
  }
  bool Valid() const {
    const auto tiles = Tiles();
    return tiles && base < 2048 && tiles <= 2048 - base &&
        host_width && host_height;
  }
};

// Retain the last actual producer after its guest header is released. There
// is one owner per disjoint eDRAM interval, bounded by the 2048 hardware tiles.
// Creating or zero-initializing host storage never publishes a guest write.
class ColorAliasTracker {
 public:
  struct Producer {
    std::shared_ptr<void> owner;
    std::shared_ptr<ColorAliasInitialization> initialization;
    uint64_t sequence = 0;
  };
 private:
  struct Entry { ColorAliasLayout layout; Producer producer; };
  std::unordered_map<uint32_t, Entry> entries_;
  uint64_t next_sequence_ = 0;
 public:
  Producer Find(const ColorAliasLayout& layout) const {
    if (!layout.supported || !layout.Valid()) return {};
    const auto it=entries_.find(layout.base);
    return it!=entries_.end() && it->second.layout==layout ? it->second.producer : Producer{};
  }
  std::shared_ptr<void> Source(const ColorAliasLayout& layout,
                              const std::shared_ptr<void>& destination) const {
    const auto producer=Find(layout);
    if (!destination || producer.owner==destination) return {};
    return producer.owner;
  }
  std::vector<std::shared_ptr<void>> Written(const ColorAliasLayout& layout, std::shared_ptr<void> owner,
      std::shared_ptr<ColorAliasInitialization> initialization = {}) {
    if (!layout.Valid() || !owner) return Reset();
    // The common scene draw writes the same allocation again. Its authority
    // stays in place; only the capture sequence advances.
    if (layout.supported) {
      const auto same=entries_.find(layout.base);
      if (same!=entries_.end() && same->second.layout==layout &&
          same->second.producer.owner==owner && same->second.producer.initialization==initialization) {
        same->second.producer.sequence=++next_sequence_;
        return {};
      }
    }
    std::vector<std::shared_ptr<void>> retired;
    const uint32_t end = layout.base + layout.Tiles();
    for (auto it = entries_.begin(); it != entries_.end();) {
      const auto& old = it->second.layout;
      if (layout.base < old.base + old.Tiles() && old.base < end) {
        retired.push_back(std::move(it->second.producer.owner));
        if (it->second.producer.initialization)
          retired.push_back(std::move(it->second.producer.initialization));
        it = entries_.erase(it);
      } else ++it;
    }
    if (layout.supported) entries_.emplace(layout.base,
        Entry{layout, Producer{std::move(owner),std::move(initialization),++next_sequence_}});
    return retired;
  }
  std::vector<std::shared_ptr<void>> Reset() {
    std::vector<std::shared_ptr<void>> retired;
    retired.reserve(entries_.size());
    for (auto& [base, entry] : entries_) {
      retired.push_back(std::move(entry.producer.owner));
      if (entry.producer.initialization) retired.push_back(std::move(entry.producer.initialization));
    }
    entries_.clear();
    return retired;
  }
  size_t Size() const { return entries_.size(); }
};
} // namespace legodimensions::gpu_native
