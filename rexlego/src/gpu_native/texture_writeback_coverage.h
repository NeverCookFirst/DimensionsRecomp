#pragma once
#include <algorithm>
#include <bit>
#include <cstdint>
#include <vector>

namespace legodimensions::gpu_native {
// Pixels written by GPU resolves since the last publication. Unwritten host
// texels may be uninitialized: they must never replace valid guest bytes.
// Whole-image coverage needs no bitmap. Partial coverage is bounded to 2 MiB
// per texture, matching the readback helper's maximum 4096x4096 dimensions.
class TextureWritebackCoverage {
 public:
  void Reset() {
    width_ = height_ = count_ = 0;
    valid_ = full_ = false;
    bits_.clear();
  }
  void Record(uint32_t width, uint32_t height, uint32_t x, uint32_t y,
              uint32_t w, uint32_t h) {
    if (width != width_ || height != height_) {
      Reset(); width_ = width; height_ = height;
      valid_ = width && height && width <= 4096 && height <= 4096;
    }
    if (!valid_ || !w || !h || x > width || w > width - x ||
        y > height || h > height - y) { valid_ = false; return; }
    if (full_) return;
    if (!x && !y && w == width && h == height) {
      full_ = true; count_ = width * height; bits_.clear(); return;
    }
    if (bits_.empty()) bits_.resize((uint64_t(width) * height + 63) / 64);
    for (uint32_t row = y; row < y + h; ++row) {
      uint32_t at = row * width + x, end = at + w;
      while (at < end) {
        const uint32_t n = std::min(64u - (at & 63), end - at);
        const uint64_t mask = (UINT64_MAX >> (64 - n)) << (at & 63);
        auto& bits = bits_[at >> 6];
        count_ += std::popcount(mask & ~bits);
        bits |= mask; at += n;
      }
    }
    if (count_ == width * height) { full_ = true; bits_.clear(); }
  }
  bool Matches(uint32_t width, uint32_t height) const {
    return valid_ && count_ && width == width_ && height == height_;
  }
  bool Contains(uint32_t x, uint32_t y) const {
    if (!valid_ || x >= width_ || y >= height_) return false;
    if (full_) return true;
    const uint32_t at = y * width_ + x;
    return !bits_.empty() && ((bits_[at >> 6] >> (at & 63)) & 1);
  }

 private:
  uint32_t width_ = 0, height_ = 0, count_ = 0;
  bool valid_ = false, full_ = false;
  std::vector<uint64_t> bits_;
};
}  // namespace legodimensions::gpu_native
