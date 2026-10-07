// Counts guest draw binding requests; equal requests are not a D3D skip proof.
#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <utility>

namespace legodimensions::gpu_native {

struct StateCallCounts {
  uint64_t draws = 0, dropped_draws = 0, recordings = 0;
  uint64_t layouts = 0, equal_layouts = 0;
  uint64_t pipelines = 0, equal_pipelines = 0;
  std::array<uint64_t, 4> descriptors{}, equal_descriptors{};
};

class StateCallLedger {
 public:
  static constexpr uint32_t kMaxDraws = 4096;
  bool BeginDraw(const void* commands, uint64_t serial) {
    if (counts_.draws >= kMaxDraws) {
      ++counts_.dropped_draws;
      return false;
    }
    ++counts_.draws;
    if (commands != commands_ || serial != serial_) {
      ++counts_.recordings;
      commands_ = commands;
      serial_ = serial;
      layout_ = pipeline_ = nullptr;
      descriptors_.fill(nullptr);
    }
    return true;
  }
  void Layout(const void* layout) {
    ++counts_.layouts;
    if (layout_ && layout_ == layout) ++counts_.equal_layouts;
    // A changed root signature invalidates its table comparisons.
    if (layout_ != layout) descriptors_.fill(nullptr);
    layout_ = layout;
  }
  void Descriptor(const void* descriptor, uint32_t slot) {
    if (slot >= descriptors_.size()) return;
    ++counts_.descriptors[slot];
    if (descriptors_[slot] && descriptors_[slot] == descriptor)
      ++counts_.equal_descriptors[slot];
    descriptors_[slot] = descriptor;
  }
  void Pipeline(const void* pipeline) {
    ++counts_.pipelines;
    if (pipeline_ && pipeline_ == pipeline) ++counts_.equal_pipelines;
    pipeline_ = pipeline;
  }
  const StateCallCounts& Counts() const { return counts_; }
  void Reset() { *this = {}; }

 private:
  StateCallCounts counts_;
  const void* commands_ = nullptr;
  uint64_t serial_ = 0;
  const void* layout_ = nullptr;
  const void* pipeline_ = nullptr;
  std::array<const void*, 4> descriptors_{};
};

// One finite capture per process. Optional trigger arms the next present
// interval; polling and file output occur only at the guest present hook.
class StateCallCapture {
 public:
  static constexpr uint32_t kMaxFrames = 120;
  StateCallCapture(std::filesystem::path output, std::filesystem::path trigger)
      : output_(std::move(output)), trigger_(std::move(trigger)),
        armed_(!output_.empty() && trigger_.empty()) {}
  StateCallLedger* BeginDraw(const void* commands, uint64_t serial) {
    if (!armed_ || !commands || !serial || exhausted_) return nullptr;
    return ledger_.BeginDraw(commands, serial) ? &ledger_ : nullptr;
  }
  void Present(uint32_t frame) {
    if (output_.empty() || exhausted_) return;
    if (!armed_) {
      std::error_code error;
      if (!std::filesystem::is_regular_file(trigger_, error) || error) return;
      if (!std::filesystem::remove(trigger_, error) || error) return;
      armed_ = true;
      return;
    }
    if (!file_.is_open()) {
      // A diagnostic cannot overwrite existing evidence.
      std::error_code error;
      if (std::filesystem::exists(output_, error) || error) {
        exhausted_ = true;
        return;
      }
      file_.open(output_, std::ios::out);
      if (!file_) { exhausted_ = true; return; }
      file_ << "# scope=guest_draw_requests;equal=adjacent_guest_request;"
               "unobserved_helper_compute_ray_or_external_calls_not_measured;"
               "max_frames=120;max_draws_per_frame=4096\n"
               "frame,draws,dropped_draws,recordings,layout_calls,layout_equal,"
               "pso_calls,pso_equal,table0_calls,table0_equal,table1_calls,table1_equal,"
               "table2_calls,table2_equal,table3_calls,table3_equal\n";
    }
    const auto& count = ledger_.Counts();
    file_ << frame << ',' << count.draws << ',' << count.dropped_draws << ','
          << count.recordings << ',' << count.layouts << ',' << count.equal_layouts
          << ',' << count.pipelines << ',' << count.equal_pipelines;
    for (size_t i = 0; i < count.descriptors.size(); ++i)
      file_ << ',' << count.descriptors[i] << ',' << count.equal_descriptors[i];
    file_ << '\n';
    file_.flush();
    ledger_.Reset();
    if (!file_ || ++frames_ >= kMaxFrames) {
      exhausted_ = true;
      file_.close();
    }
  }

 private:
  std::filesystem::path output_, trigger_;
  StateCallLedger ledger_;
  std::ofstream file_;
  uint32_t frames_ = 0;
  bool armed_ = false, exhausted_ = false;
};

}  // namespace legodimensions::gpu_native
