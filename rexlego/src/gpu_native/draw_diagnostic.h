#pragma once

#include <cstdint>
#include <array>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <unordered_set>
#include <algorithm>

namespace legodimensions::gpu_native {

// Each trigger selects the first following frame containing a draw. The
// recording lock serializes this CPU-only diagnostic with normal draw calls.
class DrawDiagnosticSession {
 public:
  static constexpr uint32_t kMaxDraws = 64;
  static constexpr uint32_t kMaxRequests = 4;
  static constexpr size_t kMaxBytes = 1024 * 1024;
  static constexpr size_t kMaxRecordBytes = kMaxBytes / kMaxDraws;

  explicit DrawDiagnosticSession(std::filesystem::path trigger = {}, bool geometry = false)
      : trigger_(std::move(trigger)), geometry_(geometry) {
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

  static std::optional<uint64_t> ParsePixelShader(std::string_view value) {
    if (!value.empty() && value.back() == '\n') value.remove_suffix(1);
    if (!value.empty() && value.back() == '\r') value.remove_suffix(1);
    if (!value.starts_with("ps=")) return std::nullopt;
    value.remove_prefix(3);
    if (value.starts_with("0x")) value.remove_prefix(2);
    if (value.empty() || value.size() > 16) return std::nullopt;
    uint64_t hash = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), hash, 16);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size())
      return std::nullopt;
    return hash;
  }

  static std::optional<std::array<uint64_t, 2>> ParseShaderPair(std::string_view value) {
    if (!value.empty() && value.back() == '\n') value.remove_suffix(1);
    if (!value.empty() && value.back() == '\r') value.remove_suffix(1);
    if (!value.starts_with("pair=")) return std::nullopt;
    value.remove_prefix(5);
    const auto comma = value.find(',');
    if (comma == std::string_view::npos) return std::nullopt;
    const auto first = ParsePixelShader("ps=" + std::string(value.substr(0, comma)));
    const auto second = ParsePixelShader("ps=" + std::string(value.substr(comma + 1)));
    if (!first || !second || *first == *second) return std::nullopt;
    return std::array<uint64_t, 2>{*first, *second};
  }

  uint32_t Begin(uint32_t frame, uint64_t pixel_shader = 0) {
    if (trigger_.empty() || (done_ && requests_ >= kMaxRequests)) return 0;
    if (done_) {
      // A completed request may be followed by another scene in the same
      // process. Do not overwrite its evidence or select the current partial
      // frame. Keep the same per-frame polling bound as initial arming.
      if (checked_ && checked_frame_ == frame) return 0;
      checked_ = true;
      checked_frame_ = frame;
      std::error_code error;
      if (!std::filesystem::is_regular_file(trigger_, error) || error) return 0;
      file_.close();
      armed_ = active_ = done_ = false;
      checked_ = false;
      draws_ = records_ = attempts_ = 0;
      written_ = 0;
      blob_bytes_ = 0;
      read_bytes_ = 0;
      blobs_.clear();
    }
    if (!armed_) {
      if (checked_ && checked_frame_ == frame) return 0;
      checked_ = true;
      checked_frame_ = frame;
      std::error_code error;
      if (!std::filesystem::is_regular_file(trigger_, error) || error) return 0;
      char text[64]{};
      std::ifstream request(trigger_, std::ios::binary);
      if (!request) return 0;
      request.read(text, sizeof(text));
      const bool valid_read = !request.bad() && request.gcount() < std::streamsize(sizeof(text));
      const std::string_view command(text, size_t(request.gcount()));
      const auto shader = valid_read ? ParsePixelShader(command) : std::nullopt;
      const auto pair = valid_read ? ParseShaderPair(command) : std::nullopt;
      const auto skip = valid_read ? ParseTrigger(command) : std::nullopt;
      request.close();  // Windows cannot remove an open trigger file.
      if (!std::filesystem::remove(trigger_, error) || error || (!skip && !shader && !pair)) return 0;
      ++requests_;
      output_ = trigger_.string() + (requests_ == 1 ? "" : "." + std::to_string(requests_))
          + ".draws.tsv";
      skip_ = skip.value_or(0);
      pixel_shader_filter_ = shader;
      shader_pair_ = pair;
      pair_draws_ = {};
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
    if (pixel_shader_filter_ && pixel_shader != *pixel_shader_filter_) return 0;
    if (shader_pair_) {
      const auto index = pixel_shader == (*shader_pair_)[0] ? 0u : 1u;
      if (pixel_shader != (*shader_pair_)[index] || pair_draws_[index] >= kMaxDraws / 2) return 0;
      ++pair_draws_[index];
    }
    return ++draws_;
  }

  void Write(std::string_view record) {
    if (!file_ || record.empty() || records_ >= draws_) return;
    ++records_;
    // Bound even malformed/unexpected records, rather than relying on the
    // expected number of declaration elements or their printed representation.
    const size_t limit = geometry_ ? 4 * kMaxBytes : kMaxBytes;
    if (record.size() > (geometry_ ? 4 * kMaxRecordBytes : kMaxRecordBytes) ||
        record.size() > limit - written_) {
      constexpr std::string_view message = "record_truncated\n";
      if (message.size() <= limit - written_) {
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
  bool geometry() const { return geometry_; }

  // Capture immutable raw guest bytes, never GPU upload memory. Keep failed
  // reads and exhausted budgets explicit so an incomplete capture cannot be
  // mistaken for an offline replay. Read through the protected small reader.
  template<class Reader>
  std::string SaveGeometry(uint32_t address, uint32_t size, Reader&& read) {
    constexpr size_t max_blob = 16 * 1024 * 1024, max_total = 64 * 1024 * 1024;
    if (!geometry_ || !active_ || done_) return "disabled";
    if (!address || !size || size > max_blob || uint64_t(address) + size > 0x100000000ull)
      return "invalid_or_oversize";
    if (size > 2 * max_total - read_bytes_) return "read_budget_exhausted";
    read_bytes_ += size;
    std::vector<uint8_t> bytes(size);
    uint64_t hash = 14695981039346656037ull;
    for (uint32_t offset = 0; offset < size;) {
      const uint32_t length = std::min<uint32_t>(768, size - offset);
      if (!read(address + offset, bytes.data() + offset, length)) return "unreadable";
      for (uint32_t i = 0; i < length; ++i) hash = (hash ^ bytes[offset+i]) * 1099511628211ull;
      offset += length;
    }
    const std::string name = std::to_string(hash) + "-" + std::to_string(size) + ".bin";
    const auto directory = std::filesystem::path(output_.string() + ".geometry");
    const auto path = directory / name;
    if (blobs_.contains(name)) return path.string();
    if (size > max_total - blob_bytes_) return "budget_exhausted";
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) return "write_failed";
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    file.close();
    if (!file) return "write_failed";
    blob_bytes_ += size;
    blobs_.insert(name);
    return path.string();
  }

 private:
  std::optional<std::array<uint64_t, 2>> shader_pair_;
  std::array<uint32_t, 2> pair_draws_{};
  std::filesystem::path trigger_, output_;
  std::ofstream file_;
  bool checked_ = false, armed_ = false, active_ = false, done_ = false;
  uint32_t checked_frame_ = 0, armed_frame_ = 0, selected_frame_ = 0;
  uint32_t draws_ = 0, records_ = 0;
  uint32_t requests_ = 0;
  uint32_t skip_ = 0, attempts_ = 0;
  std::optional<uint64_t> pixel_shader_filter_;
  size_t written_ = 0;
  bool geometry_ = false;
  size_t blob_bytes_ = 0;
  size_t read_bytes_ = 0;
  std::unordered_set<std::string> blobs_;
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
