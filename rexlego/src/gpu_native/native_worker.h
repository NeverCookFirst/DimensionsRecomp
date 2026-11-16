#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>

namespace legodimensions::gpu_native {
// Two recursive halves locked in order, recording (open command list) then registry (resource
// registries); a draw preparation scope takes only the registry half.
class NativeRecordingGate {
  std::recursive_mutex recording_, registry_;
  inline static thread_local unsigned recording_depth_ = 0, registry_depth_ = 0, depth_ = 0;
  inline static thread_local unsigned preparation_depth_ = 0, replay_only_depth_ = 0;
  inline static thread_local unsigned char modes_[64]{};
  // 0 = both halves, 1 = registry half only, 2 = recording half only.
  static bool RegistryOnly() { return preparation_depth_ != 0; }
  static unsigned char Mode() { return RegistryOnly() ? 1 : replay_only_depth_ ? 2 : 0; }
  void Push(unsigned char mode) {
    if (depth_ < 64) modes_[depth_] = mode;
    ++depth_;
  }
 public:
  static void EnterPreparation() { ++preparation_depth_; }
  static void LeavePreparation() { --preparation_depth_; }
  static bool InPreparation() { return preparation_depth_ != 0; }
  // How many nested holds of the registry half this thread has.
  static unsigned RegistryDepth() { return registry_depth_; }
  // Recorder tasks run behind every capture that could see their registry changes.
  static void EnterReplayOnly() { ++replay_only_depth_; }
  static void LeaveReplayOnly() { --replay_only_depth_; }
  void lock() {
    const unsigned char mode = Mode();
    Push(mode);
    if (mode != 1) { recording_.lock(); ++recording_depth_; }
    if (mode != 2) { registry_.lock(); ++registry_depth_; }
  }
  bool try_lock() {
    const unsigned char mode = Mode();
    if (mode != 1 && !recording_.try_lock()) return false;
    if (mode != 2 && !registry_.try_lock()) {
      if (mode != 1) recording_.unlock();
      return false;
    }
    Push(mode);
    if (mode != 1) ++recording_depth_;
    if (mode != 2) ++registry_depth_;
    return true;
  }
  void unlock() {
    --depth_;
    const unsigned char mode = depth_ < 64 ? modes_[depth_] : Mode();
    if (mode != 2) { --registry_depth_; registry_.unlock(); }
    if (mode != 1) { --recording_depth_; recording_.unlock(); }
  }
  bool HeldByCurrentThread() const { return recording_depth_ != 0 || registry_depth_ != 0; }
  // Replay owns only the open list.
  void LockReplay() { recording_.lock(); ++recording_depth_; }
  void UnlockReplay() { --recording_depth_; recording_.unlock(); }
};

// Full-preparation routing: kDeferred records into the coordinator's tape, kRecorder runs in order on the
// recorder, kRecorderBarrier also holds later preparation until it has executed.
enum class NativeTaskStage : unsigned char { kDeferred, kRecorder, kRecorderBarrier };
}

#ifdef LEGO_NATIVE_WORKER_STUB
// Offline fixtures compile copied hook bodies without the worker thread:
// threading is off, so every call takes the original inline route.
#include <intrin.h>
namespace legodimensions::gpu_native {
inline bool NativeWorkerEnabled() { return false; }
inline bool OnNativeWorkerThread() { return false; }
inline bool OnNativeRecorderThread() { return false; }
inline bool NativeWorkerStarted() { return false; }
inline void StartNativeWorker() {}
inline void NativeWorkerDrain(void* = nullptr) {}
inline void NativeWorkerSubmit(std::function<void()> task, bool = false, bool = false, bool = false,
                               NativeTaskStage = NativeTaskStage::kDeferred) { task(); }
inline void NativeWorkerSubmitPoolCopy(std::function<void()> task) { task(); }
inline void NativeWorkerSubmitCapture(std::function<void()> task) { task(); }
inline void NativeRecorderSubmit(std::function<void()> task, bool = false, bool = false,
                                 std::function<void()> prepare = {}) { if (prepare) prepare(); task(); }
inline unsigned long long NativeRecorderBarrier() { return 0; }
inline void NativeRecorderWaitExecuted(unsigned long long) {}
inline bool NativeWorkerExchangeRunningTask(bool) { return false; }
inline unsigned long long NativeNonCaptureTasks() { return 0; }
inline void NativeNameCurrentThread(const wchar_t*) {}
inline void NativeWorkerDrainCapture(std::function<void()> body = {}) { if (body) body(); }
inline void NativeWorkerThrottleFrames(unsigned) {}
inline void StopNativeWorker() {}
struct NativeWorkerStats {
  unsigned long long submitted = 0, executed = 0, drains = 0, inline_runs = 0,
      wait_us = 0, throttle_us = 0;
};
inline NativeWorkerStats ConsumeNativeWorkerStats() { return {}; }
class NativeInlineScope {};
class NativeReplayLock {};
inline bool NativeWorkerShouldQueue() { return false; }
inline bool NativeQueuedResolveEnabled() { return false; }
inline bool NativeQueuedGetEnabled() { return false; }
inline bool NativeDeferredReleaseEnabled() { return false; }
inline bool NativeQueuedSurfaceClearEnabled() { return false; }
inline NativeRecordingGate& NativeRecordingMutex() {
  static NativeRecordingGate mutex;
  return mutex;
}
}  // namespace legodimensions::gpu_native
// Fixtures compile copied bodies outside the renderer namespace too.
using legodimensions::gpu_native::NativeWorkerEnabled;
using legodimensions::gpu_native::OnNativeWorkerThread;
using legodimensions::gpu_native::StartNativeWorker;
using legodimensions::gpu_native::NativeWorkerDrain;
using legodimensions::gpu_native::NativeWorkerSubmit;
using legodimensions::gpu_native::NativeWorkerThrottleFrames;
using legodimensions::gpu_native::StopNativeWorker;
using legodimensions::gpu_native::ConsumeNativeWorkerStats;
using legodimensions::gpu_native::NativeWorkerShouldQueue;
using legodimensions::gpu_native::NativeRecordingMutex;
using legodimensions::gpu_native::NativeRecordingGate;
using legodimensions::gpu_native::NativeInlineScope;
using legodimensions::gpu_native::NativeTaskStage;
#else
namespace legodimensions::gpu_native {

// Threaded native recording (Phase B). The guest render thread used to
// translate every D3D call to D3D12 inline, under the TT engine lock that the
// game-update thread waits on. With LEGO_NATIVE_THREADED=1 the hooks keep their
// guest-visible effects inline (device shadow writes, return values) and queue
// the host work in submission order; one worker thread replays it under the
// recording lock. Guest resources are then consumed later, exactly like the
// Xbox GPU consumes them; the game's own fences and BlockUntil* calls (which
// drain this queue) keep that safe.
//
// Every HostDevice::LockRecording() on another thread drains the queue first,
// so any hook not explicitly converted keeps its old ordering. If the caller
// already holds the recording lock, pending tasks run inline on that thread.
bool NativeWorkerEnabled();
bool OnNativeWorkerThread();
bool OnNativeRecorderThread();
bool NativeWorkerStarted();
void StartNativeWorker();
// Waits for (or, when the recording lock is already held here, runs) every
// queued task. No-op on the worker itself and when threading is off.
void NativeWorkerDrain(void* caller = nullptr);
// `present` marks a Swap task for NativeWorkerThrottleFrames.
void NativeWorkerSubmit(std::function<void()> task, bool present = false, bool preserve_draw_batch = false,
                        bool capture = false, NativeTaskStage stage = NativeTaskStage::kDeferred);
// A capture-thread task that joins the open pool copy batch instead of flushing it.
void NativeWorkerSubmitPoolCopy(std::function<void()> task);
// A draw capture: staged and queued with its neighbours as one task, in submission order.
void NativeWorkerSubmitCapture(std::function<void()> task);
// Full-preparation recorder queue in submission order; the next task's `prepare` may run before this task's `run`.
void NativeRecorderSubmit(std::function<void()> task, bool present = false, bool barrier = false,
                          std::function<void()> prepare = {});
// Sequence of the latest barrier task submitted to the recorder; zero when none.
unsigned long long NativeRecorderBarrier();
// Blocks until the recorder has executed `sequence`.
void NativeRecorderWaitExecuted(unsigned long long sequence);
// Tasks other than draw captures run so far, in either the coordinator or an inline drain.
unsigned long long NativeNonCaptureTasks();
// Sets whether this thread counts as running a queued task and returns the previous value.
bool NativeWorkerExchangeRunningTask(bool running);
void NativeNameCurrentThread(const wchar_t* name);
// Full preparation: runs `body` on the coordinator behind everything queued and waits for it, without the
// recorder; otherwise runs `body` then NativeWorkerDrain.
void NativeWorkerDrainCapture(std::function<void()> body = {});
// Bounds how far the worker may lag: called at Swap, waits until at most
// `frames` earlier presents are still queued.
void NativeWorkerThrottleFrames(unsigned frames);
void StopNativeWorker();

struct NativeWorkerStats {
  unsigned long long submitted = 0, executed = 0, drains = 0, inline_runs = 0,
      wait_us = 0, throttle_us = 0;
};
NativeWorkerStats ConsumeNativeWorkerStats();

// Marks a hook that holds the recording lock while it calls guest code (pool
// copy). Threaded hooks reached from inside run inline instead of queueing, so
// they keep their position relative to the enclosing hook's own host work.
class NativeInlineScope {
 public:
  NativeInlineScope();
  ~NativeInlineScope();
  NativeInlineScope(const NativeInlineScope&) = delete;
  NativeInlineScope& operator=(const NativeInlineScope&) = delete;
};
// Holds the recording half of the gate for a tape replay.
class NativeReplayLock {
 public:
  NativeReplayLock();
  ~NativeReplayLock();
  NativeReplayLock(const NativeReplayLock&) = delete;
  NativeReplayLock& operator=(const NativeReplayLock&) = delete;
};
// True when a hook should queue its host work: threading is on, this is a
// guest thread and no NativeInlineScope is active on it.
bool NativeWorkerShouldQueue();
// LEGO_NATIVE_QUEUED_RESOLVE (default 1): Resolve and Clear queue like draws instead of draining the worker.
bool NativeQueuedResolveEnabled();
// LEGO_NATIVE_QUEUED_GET (default 1): GetRenderTarget/GetDepthStencilSurface read the device shadow.
bool NativeQueuedGetEnabled();
// LEGO_NATIVE_DEFERRED_RELEASE (default 1): a final Release queues the destruction behind the draws.
bool NativeDeferredReleaseEnabled();
// LEGO_NATIVE_QUEUED_SURFACE_CLEAR (default 1): a recycled surface's clear queues instead of draining.
bool NativeQueuedSurfaceClearEnabled();

// The recording mutex shared with HostDevice::LockRecording.
NativeRecordingGate& NativeRecordingMutex();

}  // namespace legodimensions::gpu_native
#endif  // LEGO_NATIVE_WORKER_STUB
