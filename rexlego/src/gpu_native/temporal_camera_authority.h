#pragma once
#include <array>
#include <cmath>
#include <cstdint>

namespace legodimensions::gpu_native {
// Matrix-register usage alone cannot distinguish the scene from the menu's
// separate 3D camera. Only a verified scene program may establish authority.
// Other programs may consume history when their entire current view-projection
// matrix is identical. Authority never survives a frame boundary or ambiguity.
class TemporalCameraAuthority {
 public:
  using Matrix = std::array<float,16>;
  void Observe(uint64_t frame, const Matrix& matrix) {
    if (frame_ != frame) {
      frame_ = frame;
      matrix_ = matrix;
      ambiguous_ = false;
    } else if (matrix_ != matrix) {
      ambiguous_ = true;
    }
    for (float value : matrix) if (!std::isfinite(value)) ambiguous_ = true;
  }
  bool Matches(uint64_t frame, const Matrix& matrix) const {
    return frame_ == frame && !ambiguous_ && matrix_ == matrix;
  }
  void Reset() { *this = {}; }
 private:
  uint64_t frame_ = ~uint64_t{0};
  Matrix matrix_{};
  bool ambiguous_ = true;
};
}
