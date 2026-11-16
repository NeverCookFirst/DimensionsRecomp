#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <map>

namespace legodimensions::gpu_native {
// Geometry/content identity is supplied after the actual immutable VB/IB
// uploads have been resolved. Guest addresses alone must never form this key.
using TemporalDrawKey = std::array<uint64_t, 160>;
using TemporalVertexConstants = std::array<uint32_t, 1024>;
struct alignas(256) TemporalPreviousConstants {
  TemporalVertexConstants constants{};
  uint32_t valid = 0;
  std::array<uint32_t, 63> padding{};
};
static_assert(sizeof(TemporalPreviousConstants) == 4352);

class TemporalDrawHistory {
 public:
  explicit TemporalDrawHistory(size_t limit = 4096) : limit_(limit) {}
  TemporalPreviousConstants Record(const TemporalDrawKey& key,
                                  const TemporalVertexConstants& constants,
                                  bool verified_world_transform = false) {
    TemporalPreviousConstants result;
    auto found = previous_.find(key);
    // Verified world shaders transform positions with c48..51, then the c0..3
    // camera. Their previous shader evaluates the entire previous constant
    // image (including wind), while stable object matrices distinguish reused
    // meshes. Other callers retain the stricter noncamera comparison. A changed
    // object matrix or ambiguous duplicate rejects history.
    if (found != previous_.end() && !found->second.ambiguous &&
        found->second.world == verified_world_transform &&
        (verified_world_transform
          ? std::equal(constants.begin()+48*4,constants.begin()+52*4,
                       found->second.constants.begin()+48*4)
          : std::equal(constants.begin()+16,constants.end(),
                       found->second.constants.begin()+16))) {
      result.constants = found->second.constants;
      result.valid = 1;
    }
    auto current = current_.find(key);
    if (current != current_.end()) {
      if (current->second.constants != constants) current->second.ambiguous = true;
      if (current->second.ambiguous) result.valid = 0;
    } else if (current_.size() < limit_) {
      current_.emplace(key, Entry{constants, false, verified_world_transform});
    } else result.valid = 0;
    return result;
  }
  void EndFrame(bool presented) {
    if (presented) previous_.swap(current_);
    else previous_.clear();
    current_.clear();
  }
  void Reset() { previous_.clear(); current_.clear(); }
 private:
  struct Entry { TemporalVertexConstants constants; bool ambiguous; bool world; };
  size_t limit_;
  std::map<TemporalDrawKey, Entry> previous_, current_;
};
}  // namespace legodimensions::gpu_native
