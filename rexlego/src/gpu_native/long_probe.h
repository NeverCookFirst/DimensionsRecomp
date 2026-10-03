// Opt-in long-run diagnostics. No guest or rendering state is modified here.
#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>

namespace legodimensions::gpu_native {
inline const std::filesystem::path& LongProbeDirectory() {
  static const std::filesystem::path path = [] {
    char* value = nullptr; size_t length = 0;
    _dupenv_s(&value, &length, "LEGO_NATIVE_TRACE_DIR");
    std::filesystem::path result(value ? value : "");
    std::free(value); return result;
  }();
  return path;
}
inline bool LongProbeEnabled() { return !LongProbeDirectory().empty(); }
inline std::atomic<uint32_t> g_probe_frame{0};
inline std::atomic<bool> g_probe_capture_requested{false};
inline double LongProbeMilliseconds() {
  static const auto start = std::chrono::steady_clock::now();
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now()-start).count();
}
inline void WriteLongProbeEvent(std::string_view kind, bool anomaly, std::string_view payload);
template<class... T>
inline void LongProbeEvent(std::string_view kind, bool anomaly, const T&... values) {
  if (!LongProbeEnabled()) return;
  std::ostringstream payload;
  ((payload << values << ' '), ...);
  WriteLongProbeEvent(kind, anomaly, payload.str());
}
inline void WriteLongProbeEvent(std::string_view kind, bool anomaly, std::string_view payload) {
  struct Writer { std::mutex mutex; std::ofstream file; uint64_t rows=0;
    Writer() {
      std::error_code error; std::filesystem::create_directories(LongProbeDirectory(),error);
      file.open(LongProbeDirectory()/"events.tsv",std::ios::out|std::ios::app);
      file << "elapsed_ms\tframe\tevent\tanomaly_candidate\tdetails\n";
      std::ofstream start(LongProbeDirectory()/"trace-start.json");
      start << "{\"unix_ms\":" << std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch()).count() << "}";
    }
  };
  static Writer writer;
  std::lock_guard lock(writer.mutex);
  writer.file << std::fixed << std::setprecision(3) << LongProbeMilliseconds() << '\t'
      << g_probe_frame.load() << '\t' << kind << '\t' << anomaly << '\t' << payload << '\n';
  if (anomaly || ++writer.rows % 256 == 0 || kind=="frame") writer.file.flush();
  if (anomaly) g_probe_capture_requested.store(true);
}
inline bool LongProbeOnce(uint64_t key) {
  static std::mutex mutex; static std::unordered_set<uint64_t> seen;
  std::lock_guard lock(mutex); return seen.insert(key).second;
}
template<class Words> inline std::string LongProbeHex(const Words& words) {
  std::ostringstream out; out << std::hex << std::setfill('0');
  for (auto word : words) out << std::setw(8) << uint32_t(word) << ',';
  return out.str();
}
inline std::string SaveLongProbeConstants(std::span<const uint8_t> bytes) {
  static std::atomic<uint64_t> sequence{0};
  static std::mutex budget_mutex;
  static uint32_t budget_frame=~0u,count=0;
  {
    std::lock_guard lock(budget_mutex);
    const auto frame=g_probe_frame.load();
    if(frame!=budget_frame) { budget_frame=frame; count=0; }
    if(count++>=128) {
      if(count==129) LongProbeEvent("constants_frame_budget",false,"frame=",frame,"limit=128");
      return {};
    }
  }
  const auto name="constants-f"+std::to_string(g_probe_frame.load())+"-"+
      std::to_string(sequence.fetch_add(1))+".bin";
  std::error_code error; std::filesystem::create_directories(LongProbeDirectory(),error);
  std::ofstream file(LongProbeDirectory()/name,std::ios::binary);
  file.write(reinterpret_cast<const char*>(bytes.data()),bytes.size());
  return file ? name : "WRITE_FAILED";
}
} // namespace legodimensions::gpu_native
