#pragma once

#include <plume_d3d12.h>
#include <array>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>

namespace legodimensions::gpu_native {

// Optional diagnostic, serialized by HostDevice's recording/state locks.
// Same-list bottom-of-pipe elapsed time is not GPU utilization or frame latency.
// Each slot owns its query/readback storage until its existing submission fence
// completes. This class never submits, signals, waits, or polls a fence itself.
class GpuSubmissionTimestamps {
 public:
  static constexpr unsigned kMaxSlots = 12;
  static constexpr unsigned kNoImage = ~0u;

  static std::unique_ptr<GpuSubmissionTimestamps> Create(
      plume::RenderDevice* device, plume::RenderCommandQueue* queue,
      unsigned slots, const char* path, const char* fingerprint) noexcept {
    if (!path || !*path || !fingerprint || slots == 0 || slots > kMaxSlots) return nullptr;
    try {
      auto result = std::make_unique<GpuSubmissionTimestamps>();
      auto* native_queue = static_cast<plume::D3D12CommandQueue*>(queue);
      if (!device || !native_queue || !native_queue->d3d ||
          FAILED(native_queue->d3d->GetTimestampFrequency(&result->frequency_)) ||
          result->frequency_ == 0) return nullptr;
      result->count_ = slots;
      result->fingerprint_ = fingerprint;
      for (unsigned slot = 0; slot < slots; ++slot) {
        auto& pool = result->slots_[slot].pool;
        pool = device->createQueryPool(2);
        if (!pool || pool->getCount() != 2) return nullptr;
        auto* native = static_cast<plume::D3D12QueryPool*>(pool.get());
        if (!native->d3d || !native->readbackBuffer ||
            !static_cast<plume::D3D12Buffer*>(native->readbackBuffer.get())->d3d)
          return nullptr;
      }
      result->file_.open(path);
      if (!result->file_) return nullptr;
      constexpr const char* header =
          "build_fingerprint,source_frame,slot,recording,submission,submit_kind,sync_reason,image,present_result,completion_frame,start_ticks,end_ticks,frequency_hz,queue_elapsed_ms,status\n";
      result->file_ << header;
      if (!result->file_) return nullptr;
      result->bytes_ = std::strlen(header);
      result->enabled_ = true;
      return result;
    } catch (...) {
      // Diagnostics must never turn allocation/file failure into title failure.
      return nullptr;
    }
  }

  UINT64 Frequency() const noexcept { return frequency_; }

  void Begin(plume::RenderCommandList* commands, unsigned slot,
             unsigned long long source_frame) noexcept {
    if (!enabled_ || slot >= count_) return;
    auto& sample = slots_[slot];
    // Reuse is legal only after Complete consumed this exact slot's old pair.
    if (sample.pending || sample.recording) { enabled_ = false; return; }
    sample.source_frame = source_frame;
    sample.number = ++recording_number_;
    sample.submission = 0;
    sample.kind = "unsubmitted";
    sample.sync_reason = sample.image = kNoImage;
    sample.present_result = -1;
    sample.recording = true;
    commands->writeTimestamp(sample.pool.get(), 0);
  }

  void End(plume::RenderCommandList* commands, unsigned slot,
           const char* kind, unsigned sync_reason = kNoImage,
           unsigned image = kNoImage) noexcept {
    if (slot >= count_) return;
    auto& sample = slots_[slot];
    // Finish any already-written start even if a later diagnostic failure
    // disabled new recordings. Never resolve an uninitialized query pair.
    if (!sample.recording) return;
    commands->writeTimestamp(sample.pool.get(), 1);
    sample.kind = kind;
    sample.sync_reason = sync_reason;
    sample.image = image;
    sample.recording = false;
    sample.pending = true;
  }

  void Submitted(unsigned slot, unsigned long long submission) noexcept {
    if (slot < count_ && slots_[slot].pending)
      slots_[slot].submission = submission;
  }

  void PresentResult(unsigned slot, bool presented) noexcept {
    if (slot < count_ && slots_[slot].pending)
      slots_[slot].present_result = presented ? 1 : 0;
  }

  void Complete(unsigned slot, unsigned long long completed,
                unsigned long long target,
                unsigned long long completion_frame) noexcept {
    if (slot >= count_) return;
    auto& sample = slots_[slot];
    if (!sample.pending || !sample.submission) return;
    if (completed == ~0ull) {
      enabled_ = false;
      Write(slot, completion_frame, nullptr, "device_removed");
      sample.pending = false;
      return;
    }
    if (!target || completed < target) return;
    auto* pool = static_cast<plume::D3D12QueryPool*>(sample.pool.get());
    auto* buffer = static_cast<plume::D3D12Buffer*>(pool->readbackBuffer.get());
    const D3D12_RANGE read_range{0, 2 * sizeof(UINT64)};
    void* mapped = nullptr;
    const HRESULT status = buffer->d3d->Map(0, &read_range, &mapped);
    UINT64 ticks[2]{};
    if (FAILED(status) || !mapped) {
      if (SUCCEEDED(status)) {
        const D3D12_RANGE no_writes{0, 0};
        buffer->d3d->Unmap(0, &no_writes);
      }
      enabled_ = false;
      Write(slot, completion_frame, nullptr, "map_failed");
    } else {
      std::memcpy(ticks, mapped, sizeof(ticks));
      const D3D12_RANGE no_writes{0, 0};
      buffer->d3d->Unmap(0, &no_writes);
      if (ticks[1] < ticks[0]) enabled_ = false;
      Write(slot, completion_frame, ticks,
            ticks[1] < ticks[0] ? "reversed_ticks" : "ok");
    }
    sample.pending = false;
  }

  void Shutdown(unsigned long long completion_frame) noexcept {
    enabled_ = false;
    for (unsigned slot = 0; slot < count_; ++slot) {
      auto& sample = slots_[slot];
      if (sample.pending || sample.recording) {
        Write(slot, completion_frame, nullptr,
              sample.pending ? "unverified_shutdown" : "unsubmitted_shutdown");
        sample.pending = sample.recording = false;
      }
    }
    file_.flush();
  }

 private:
  struct Sample {
    std::unique_ptr<plume::RenderQueryPool> pool;
    unsigned long long source_frame = 0, number = 0, submission = 0;
    const char* kind = "unsubmitted";
    unsigned sync_reason = kNoImage, image = kNoImage;
    int present_result = -1;
    bool recording = false, pending = false;
  };
  std::array<Sample, kMaxSlots> slots_;
  UINT64 frequency_ = 0;
  unsigned count_ = 0;
  unsigned long long recording_number_ = 0, rows_ = 0, bytes_ = 0;
  const char* fingerprint_ = "";
  std::ofstream file_;
  bool enabled_ = false;

  void Write(unsigned slot, unsigned long long frame, const UINT64* ticks,
             const char* status) noexcept {
    // Fixed stack formatting and a bounded buffered file: no per-row heap
    // allocation, no writer thread, no unbounded log growth at high kick rates.
    if (!file_ || rows_ >= 200000) { enabled_ = false; return; }
    auto& sample = slots_[slot];
    char start[32]{}, end[32]{}, elapsed[48]{};
    if (ticks) {
      std::snprintf(start, sizeof(start), "%llu", static_cast<unsigned long long>(ticks[0]));
      std::snprintf(end, sizeof(end), "%llu", static_cast<unsigned long long>(ticks[1]));
      if (ticks[1] >= ticks[0])
        std::snprintf(elapsed, sizeof(elapsed), "%.9f",
            static_cast<double>(ticks[1] - ticks[0]) * 1000.0 / static_cast<double>(frequency_));
    }
    char row[512];
    const int length = std::snprintf(row, sizeof(row),
        "%s,%llu,%u,%llu,%llu,%s,%u,%u,%d,%llu,%s,%s,%llu,%s,%s\n",
        fingerprint_, sample.source_frame, slot, sample.number, sample.submission,
        sample.kind, sample.sync_reason, sample.image, sample.present_result,
        frame, start, end, static_cast<unsigned long long>(frequency_), elapsed, status);
    if (length <= 0 || static_cast<unsigned>(length) >= sizeof(row) ||
        bytes_ + static_cast<unsigned>(length) > 64ull * 1024 * 1024) {
      enabled_ = false;
      return;
    }
    file_.write(row, length);
    bytes_ += static_cast<unsigned>(length);
    if (++rows_ % 120 == 0) file_.flush();
    if (!file_) enabled_ = false;
  }
};
}  // namespace legodimensions::gpu_native
