#pragma once
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace legodimensions::gpu_native {
// Recording-lock serialized, metadata-only diagnostic. A trigger arms the
// following frame; allocation activity during startup consumes no record budget.
class BufferAllocationDiagnostic {
 public:
  static std::optional<uint32_t> ParseGuest(std::string_view value) {
    if (value.size() != 8) return std::nullopt;
    uint32_t guest = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), guest, 16);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) return std::nullopt;
    return guest;
  }
  BufferAllocationDiagnostic(std::optional<uint32_t> guest = {},
      std::filesystem::path trigger = {}) : guest_(guest), trigger_(std::move(trigger)) {}
  bool Select(uint32_t frame, uint32_t guest) {
    if (!guest_ || trigger_.empty() || guest != *guest_ || records_ >= 128) return false;
    if (!armed_) {
      if (checked_ && checked_frame_ == frame) return false;
      checked_ = true;
      checked_frame_ = frame;
      std::error_code error;
      if (!std::filesystem::is_regular_file(trigger_, error) || error ||
          !std::filesystem::remove(trigger_, error) || error) return false;
      armed_ = true;
      armed_frame_ = frame;
    }
    if (frame == armed_frame_) return false;
    ++records_;
    return true;
  }
 private:
  std::optional<uint32_t> guest_;
  std::filesystem::path trigger_;
  bool checked_ = false, armed_ = false;
  uint32_t checked_frame_ = 0, armed_frame_ = 0, records_ = 0;
};
}  // namespace legodimensions::gpu_native
