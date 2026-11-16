#pragma once
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace legodimensions::gpu_native {
// A descriptor may be referenced by several submitted lists and by the list
// still being recorded. Each ticket must finish before its slot is reusable.
class DescriptorRetirement {
 public:
  explicit DescriptorRetirement(std::size_t count = 0, uint32_t slots = 3)
      : pending_(count, 0) {
    if (!slots || slots > 32) throw std::invalid_argument("descriptor retirement slots outside 1..32");
    tickets_.resize(slots);
  }
  bool Retire(uint32_t index, uint32_t live_frames) {
    if (pending_[index]) return false;
    for (uint32_t frame = 0; frame < tickets_.size(); ++frame) {
      if (!(live_frames & (1u << frame))) continue;
      tickets_[frame].push_back(index);
      ++pending_[index];
    }
    return pending_[index] == 0;
  }
  template<class Release> void CompleteFrame(uint32_t frame, Release release) {
    for (uint32_t index : tickets_[frame])
      if (--pending_[index] == 0) release(index);
    tickets_[frame].clear();
  }
 private:
  std::vector<uint8_t> pending_;
  std::vector<std::vector<uint32_t>> tickets_;
};
}  // namespace legodimensions::gpu_native
