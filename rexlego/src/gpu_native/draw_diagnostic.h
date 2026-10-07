#pragma once

#include <cstdint>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace legodimensions::gpu_native {

// One trigger selects the first following frame containing a draw. The
// recording lock serializes this CPU-only diagnostic with normal draw calls.
class DrawDiagnosticSession {
 public:
  static constexpr uint32_t kMaxDraws = 64;
  static constexpr size_t kMaxBytes = 1024 * 1024;
  static constexpr size_t kMaxRecordBytes = kMaxBytes / kMaxDraws;

  explicit DrawDiagnosticSession(std::filesystem::path trigger = {})
      : trigger_(std::move(trigger)) {
    if (!trigger_.empty()) output_ = trigger_.string() + ".draws.tsv";
  }

  static std::optional<uint32_t> ParseTrigger(std::string_view value) {
    if (!value.empty() && value.back() == '\n') value.remove_suffix(1);
    if (!value.empty() && value.back() == '\r') value.remove_suffix(1);
    if (value.empty()) return 0;
    if (!value.starts_with("skip=")) return std::nullopt;
    value.remove_prefix(5);
    uint32_t skip = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), skip);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || skip > 4096)
      return std::nullopt;
    return skip;
  }

  uint32_t Begin(uint32_t frame) {
    if (trigger_.empty() || done_) return 0;
    if (!armed_) {
      if (checked_ && checked_frame_ == frame) return 0;
      checked_ = true;
      checked_frame_ = frame;
      std::error_code error;
      if (!std::filesystem::is_regular_file(trigger_, error) || error) return 0;
      char text[32]{};
      std::ifstream request(trigger_, std::ios::binary);
      if (!request) return 0;
      request.read(text, sizeof(text));
      const auto skip = !request.bad() && request.gcount() < std::streamsize(sizeof(text))
          ? ParseTrigger(std::string_view(text, size_t(request.gcount()))) : std::nullopt;
      request.close();  // Windows cannot remove an open trigger file.
      if (!std::filesystem::remove(trigger_, error) || error || !skip) return 0;
      skip_ = *skip;
      armed_ = true;
      armed_frame_ = frame;
      return 0;  // A trigger noticed mid-frame must not select a partial frame.
    }
    if (!active_) {
      if (frame == armed_frame_) return 0;
      active_ = true;
      selected_frame_ = frame;
      file_.open(output_, std::ios::binary | std::ios::trunc);
      if (!file_) { done_ = true; return 0; }
    }
    if (frame != selected_frame_ || draws_ >= kMaxDraws) {
      done_ = true;
      file_.flush();
      return 0;
    }
    if (++attempts_ <= skip_) return 0;
    return ++draws_;
  }

  void Write(std::string_view record) {
    if (!file_ || record.empty() || records_ >= draws_) return;
    ++records_;
    // Bound even malformed/unexpected records, rather than relying on the
    // expected number of declaration elements or their printed representation.
    if (record.size() > kMaxRecordBytes ||
        record.size() > kMaxBytes - written_) {
      constexpr std::string_view message = "record_truncated\n";
      if (message.size() <= kMaxBytes - written_) {
        file_.write(message.data(), message.size());
        written_ += message.size();
      }
    } else {
      file_.write(record.data(), record.size());
      written_ += record.size();
    }
    file_.flush();
    if (!file_) done_ = true;
  }

  const std::filesystem::path& output() const { return output_; }
  size_t written() const { return written_; }
  uint32_t attempted_ordinal() const { return attempts_; }

 private:
  std::filesystem::path trigger_, output_;
  std::ofstream file_;
  bool checked_ = false, armed_ = false, active_ = false, done_ = false;
  uint32_t checked_frame_ = 0, armed_frame_ = 0, selected_frame_ = 0;
  uint32_t draws_ = 0, records_ = 0;
  uint32_t skip_ = 0, attempts_ = 0;
  size_t written_ = 0;
};

// Reject negative/rebased vertices and arithmetic overflow before touching
// guest memory. The requested sample must fit the actual source buffer.
inline std::optional<uint32_t> DrawDiagnosticVertexOffset(
    int64_t vertex, uint32_t stride, uint32_t stream_offset,
    uint32_t element_offset, uint32_t bytes, uint32_t buffer_length) {
  if (vertex < 0 || !stride || !bytes ||
      uint64_t(vertex) > std::numeric_limits<uint64_t>::max() / stride)
    return std::nullopt;
  const uint64_t relative = uint64_t(vertex) * stride;
  const uint64_t prefix = uint64_t(stream_offset) + element_offset;
  if (relative > std::numeric_limits<uint64_t>::max() - prefix)
    return std::nullopt;
  const uint64_t offset = relative + prefix;
  if (offset > buffer_length || bytes > buffer_length - offset)
    return std::nullopt;
  return uint32_t(offset);
}

}  // namespace legodimensions::gpu_native
