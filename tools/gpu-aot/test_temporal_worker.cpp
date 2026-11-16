// Production worker threads with a CPU command-tape boundary, no game or GPU.
#include "gpu_native/native_worker.h"
#include "gpu_native/visual_effects.h"
#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>
#include <chrono>
using namespace legodimensions::gpu_native;
namespace {
std::atomic<bool> temporal{false};
std::atomic<unsigned> expected{0}, parallel_draws{0}, temporal_draws{0};
std::atomic<bool> batch_pending{false};
std::thread::id recorder;
std::vector<std::function<void()>> tape;
void Check(unsigned number) {
  if (expected.fetch_add(1) != number) std::exit(23);
}
void Draw(unsigned number, bool should_be_temporal) {
  if (temporal.load() != should_be_temporal) std::exit(24);
  const auto effects=g_native_visual_effects.load();
  if(effects!=(should_be_temporal?0x80000004u:0x80000001u))std::exit(27);
  if (temporal.load()) {
    if (std::this_thread::get_id() != recorder || !OnNativeRecorderThread()) std::exit(25);
    std::lock_guard lock(NativeRecordingMutex());
    if (!NativeRecordingGate::RegistryDepth()) std::exit(26);
    ++temporal_draws; Check(number);
  } else {
    ++parallel_draws; tape.push_back([number,effects] {
      if(g_native_visual_effects.load()!=effects)std::exit(28);
      Check(number);
    });batch_pending=true;
  }
}
}
namespace legodimensions::gpu_native {
bool NativeFullDrawPreparationEnabled() { return true; }
bool NativePrepareEnabled() { return true; }
bool NativeParallelDrawsEnabled() { return !temporal.load(); }
unsigned NativePreparationWorkerCount() { return 3; }
void StopNativePreparation() {}
bool NativeFsr3Enabled() { return temporal.load(); }
bool NativeDrawBatchPending() { return batch_pending.load(); }
void FlushPoolCopyBatch() {}
void FlushNativeDrawBatch() {}
void FlushFullDrawBatch() {
  if (tape.empty()) return;
  auto pending = std::move(tape); tape.clear();batch_pending=false;
  NativeRecorderSubmit([pending = std::move(pending)] { for (auto& f : pending) f(); });
}
void FlushFullDrawBatchIdle() { FlushFullDrawBatch(); }
void RecordDeferredNativeTask(std::function<void()>& task) { tape.push_back(task); }
}
int main() {
  _putenv_s("LEGO_NATIVE_THREADED", "1");
  PublishNativeVisualEffects({true,NativeCelShadingMode::kOriginal});
  unsigned number = 0;
  NativeWorkerSubmit([] { recorder = std::this_thread::get_id(); }, false, false, false, NativeTaskStage::kRecorderBarrier);
  for (unsigned cycle = 0; cycle < 24; ++cycle) {
    for (unsigned i = 0; i < 32; ++i) {
      const unsigned n = number++;
      NativeWorkerSubmitCapture([n] { Draw(n, false); });
    }
    const unsigned enable = number++;
    NativeWorkerSubmit([enable] {
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
      Check(enable); PublishNativeVisualEffects({false,NativeCelShadingMode::kOff});temporal = true;
    }, true, false, false, NativeTaskStage::kRecorderBarrier);
    for (unsigned i = 0; i < 128; ++i) {
      const unsigned n = number++;
      if (i % 7) NativeWorkerSubmitCapture([n] { Draw(n, true); });
      else NativeWorkerSubmit([n] { Draw(n, true); });
    }
    const unsigned disable = number++;
    NativeWorkerSubmit([disable] {
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
      Check(disable); PublishNativeVisualEffects({true,NativeCelShadingMode::kOriginal});temporal = false;
    }, true, false, false, NativeTaskStage::kRecorderBarrier);
  }
  NativeWorkerDrain(); StopNativeWorker();
  assert(expected == number && parallel_draws == 24 * 32 && temporal_draws == 24 * 128);
  std::printf("PASS production worker: %u ordered tasks, 48 mode transitions, parallel=%u temporal=%u\n",
      expected.load(), parallel_draws.load(), temporal_draws.load());
}
