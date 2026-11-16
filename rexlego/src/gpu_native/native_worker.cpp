#include "gpu_native/native_worker.h"
#include "gpu_native/pool_copy.h"
#include "gpu_native/native_recording.h"
#include "gpu_native/draw_batch.h"
#include "gpu_native/fsr3_settings.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <rex/logging.h>

namespace legodimensions::gpu_native {
namespace {

using Clock = std::chrono::steady_clock;
// Bounded so a stalled worker throttles the guest instead of growing forever.
constexpr size_t kMaxQueuedTasks = 1u << 18;

struct Task {
  std::function<void()> run;
  bool present = false;
  bool preserve_draw_batch = false;
  bool capture = false;
  NativeTaskStage stage = NativeTaskStage::kDeferred;
  unsigned long long sequence = 0;
  std::function<void()> prepare;
  bool pool_copy = false;
};

std::mutex g_queue_mutex;
// Staged draw captures, flushed ahead of every other submission, drain, throttle and stop so no task
// overtakes an earlier draw; lock order is g_stage_mutex, then g_queue_mutex.
std::mutex g_stage_mutex;
std::vector<std::function<void()>> g_staged_captures;
constexpr size_t kStagedCaptureRun = 16;
std::condition_variable g_queue_ready;     // tasks available / stop
std::condition_variable g_queue_progress;  // a task finished
std::deque<Task> g_queue;
bool g_stop = false;
bool g_executing = false;                  // guarded by g_queue_mutex
unsigned g_pending_presents = 0;           // guarded by g_queue_mutex
std::thread g_thread, g_recorder_thread;
std::deque<Task> g_record_queue;
std::condition_variable g_record_ready;
size_t g_record_pending = 0;
// Recorder sequence numbers: submitted under g_queue_mutex, executed under g_recorder_mutex.
std::atomic<unsigned long long> g_recorder_submitted{0}, g_recorder_barrier{0}, g_recorder_executed{0};
std::mutex g_recorder_mutex;
std::condition_variable g_recorder_progress;
std::atomic<bool> g_started{false};
DWORD g_worker_thread_id = 0;
thread_local bool t_running_task = false;
thread_local bool t_on_recorder = false;
std::atomic<unsigned long long> g_noncapture_tasks{0};
thread_local int t_inline_scope = 0;

std::mutex g_stats_mutex;
NativeWorkerStats g_stats;
std::unordered_map<void*, unsigned long long> g_drain_callers;
Clock::time_point g_last_caller_report = Clock::now();

void RecordDrain(void* caller, Clock::duration elapsed) {
  std::lock_guard lock(g_stats_mutex);
  ++g_stats.drains;
  g_stats.wait_us += std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count();
  if (caller) ++g_drain_callers[caller];
  const auto now = Clock::now();
  if (now - g_last_caller_report < std::chrono::seconds(10) || g_drain_callers.empty()) return;
  g_last_caller_report = now;
  // Return addresses of LockRecording callers that forced a drain; resolve
  // them with legodimensions.map to pick the next hooks to convert.
  std::vector<std::pair<void*, unsigned long long>> top(g_drain_callers.begin(), g_drain_callers.end());
  std::sort(top.begin(), top.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
  const auto module = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
  std::string text;
  for (size_t i = 0; i < top.size() && i < 12; ++i) {
    char item[64];
    std::snprintf(item, sizeof(item), " +0x%llx:%llu",
                  static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(top[i].first) - module),
                  top[i].second);
    text += item;
  }
  REXLOG_INFO("Native worker drains by caller (exe RVA:count, last 10 s):{}", text);
  g_drain_callers.clear();
}

void FinishTask(bool present) {
  std::lock_guard lock(g_queue_mutex);
  g_executing = false;
  if (present && g_pending_presents) --g_pending_presents;
}

// Runs every queued task on this thread. The caller holds the recording lock,
// so the worker cannot be inside (or between popping and running) a task.
void RunQueuedInline() {
  unsigned long long ran = 0;
  for (;;) {
    Task task;
    {
      std::lock_guard lock(g_queue_mutex);
      if (g_queue.empty()) break;
      task = std::move(g_queue.front());
      g_queue.pop_front();
      g_executing = true;
    }
    t_running_task = true;
    if (!task.capture) g_noncapture_tasks.fetch_add(1, std::memory_order_relaxed);
    if (!task.preserve_draw_batch) FlushNativeDrawBatch();
    task.run();
    t_running_task = false;
    FinishTask(task.present);
    ++ran;
  }
  t_running_task = true;
  FlushNativeDrawBatch();
  t_running_task = false;
  g_queue_progress.notify_all();
  std::lock_guard lock(g_stats_mutex);
  g_stats.executed += ran;
  g_stats.inline_runs += ran;
}

void FinishRecorderTask(const Task& task) {
  {
    std::lock_guard lock(g_recorder_mutex);
    g_recorder_executed.store(task.sequence, std::memory_order_release);
  }
  g_recorder_progress.notify_all();
  {
    std::lock_guard lock(g_queue_mutex);
    --g_record_pending;
    if (task.present && g_pending_presents) --g_pending_presents;
  }
  g_queue_progress.notify_all();
}

// The next batch prepares while this one replays; any other task in between ends the lookahead so each task
// sees everything queued before it.
void RecorderMain() {
  t_on_recorder = true;
  SetThreadDescription(GetCurrentThread(), L"Native GPU recording");
  for (;;) {
    std::vector<Task> pending;
    {
      std::unique_lock lock(g_queue_mutex);
      g_record_ready.wait(lock, [] { return g_stop || !g_record_queue.empty(); });
      if (g_record_queue.empty()) return;
      pending.push_back(std::move(g_record_queue.front()));
      g_record_queue.pop_front();
    }
    t_running_task = true;
    struct ReplayOnly {
      bool active;
      explicit ReplayOnly(bool on) : active(on) { if (active) NativeRecordingGate::EnterReplayOnly(); }
      ~ReplayOnly() { if (active) NativeRecordingGate::LeaveReplayOnly(); }
    };
    if (pending.back().prepare) {
      ReplayOnly scope(!pending.back().present);
      pending.back().prepare();
      for (;;) {
        Task next;
        {
          // Never prepare under the queue mutex: prepare takes the recording gate,
          // and a guest hook may hold that gate while it submits to this queue.
          std::lock_guard lock(g_queue_mutex);
          if (pending.size() >= 2 || g_record_queue.empty() || !g_record_queue.front().prepare) break;
          next = std::move(g_record_queue.front());
          g_record_queue.pop_front();
        }
        { ReplayOnly scope(!next.present); next.prepare(); }
        pending.push_back(std::move(next));
      }
    }
    for (auto& task : pending) {
      // Present keeps both halves: it reads the front buffer record against later deferred tasks.
      { ReplayOnly scope(!task.present && !NativeFsr3Enabled()); task.run(); }
      FinishRecorderTask(task);
    }
    t_running_task = false;
  }
}

// Runs captures, records deferred tasks into the batch and forwards recorder tasks; never waits on the recorder
// or the preparation workers.
void CoordinatorMain() {
  SetThreadDescription(GetCurrentThread(), L"Native GPU preparation");
  for (;;) {
    {
      std::unique_lock lock(g_queue_mutex);
      g_queue_ready.wait(lock, [] { return g_stop || !g_queue.empty(); });
      if (g_queue.empty() && g_stop) return;
      g_executing = true;
    }
    unsigned long long ran = 0;
    t_running_task = true;
    bool idle = false;
    for (unsigned i = 0; i < 512; ++i) {
      Task task;
      {
        std::lock_guard lock(g_queue_mutex);
        if (g_queue.empty()) { idle = true; break; }
        task = std::move(g_queue.front());
        g_queue.pop_front();
      }
      // A present may publish a different render extent or temporal mode. Its
      // barrier must complete before deciding where the next frame runs.
      NativeRecorderWaitExecuted(NativeRecorderBarrier());
      if (NativeFsr3Enabled()) {
        // Temporal camera/history, MRT attachments and the FFX context form one
        // ordered frame state. They cannot be prepared on independent tapes.
        // Keep guest submission asynchronous, but execute this frame's hooks
        // together on the recorder. The non-temporal parallel path is unchanged.
        NativeRecorderSubmit([run = std::move(task.run), pool_copy = task.pool_copy,
                              preserve = task.preserve_draw_batch] {
          if (!pool_copy) FlushPoolCopyBatch();
          if (!preserve) FlushNativeDrawBatch();
          run();
        }, task.present, task.present || task.stage == NativeTaskStage::kRecorderBarrier);
        ++ran;
        continue;
      }
      // Any other task may read the caches a pool copy batch still has to invalidate.
      if (!task.pool_copy) FlushPoolCopyBatch();
      if (!task.capture) g_noncapture_tasks.fetch_add(1, std::memory_order_relaxed);
      if (task.capture) task.run();
      else if (task.stage == NativeTaskStage::kDeferred && !task.present) RecordDeferredNativeTask(task.run);
      else {
        FlushFullDrawBatch();
        NativeRecorderSubmit(std::move(task.run), task.present, task.stage == NativeTaskStage::kRecorderBarrier);
      }
      ++ran;
    }
    // No pool copy flush on idle: a defrag burst stays one batch until a task that reads the caches.
    if (idle) FlushFullDrawBatchIdle();
    t_running_task = false;
    {
      std::lock_guard lock(g_queue_mutex);
      g_executing = false;
    }
    g_queue_progress.notify_all();
    std::lock_guard lock(g_stats_mutex);
    g_stats.executed += ran;
  }
}

void WorkerMain() {
  if (NativeFullDrawPreparationEnabled()) { CoordinatorMain(); return; }
  SetThreadDescription(GetCurrentThread(), NativePrepareEnabled() ? L"Native GPU preparation" : L"Native GPU recording");
  for (;;) {
    {
      std::unique_lock lock(g_queue_mutex);
      g_queue_ready.wait(lock, [] { return g_stop || !g_queue.empty(); });
      if (g_queue.empty() && g_stop) return;
    }
    // Take the recording lock BEFORE popping: a guest thread that holds it may
    // otherwise run later tasks inline while this one is still in hand.
    std::unique_lock recording(NativeRecordingMutex());
    unsigned long long ran = 0;
    // A short batch amortizes lock traffic while keeping guest drains prompt.
    for (int i = 0, limit = NativeParallelDrawsEnabled() ? 512 : 32; i < limit; ++i) {
      Task task;
      {
        std::lock_guard lock(g_queue_mutex);
        if (g_queue.empty()) break;
        task = std::move(g_queue.front());
        g_queue.pop_front();
        g_executing = true;
      }
      t_running_task = true;
      if (!task.preserve_draw_batch) FlushNativeDrawBatch();
      task.run();
      t_running_task = false;
      FinishTask(task.present);
      ++ran;
    }
    FlushNativeDrawBatch();
    recording.unlock();
    g_queue_progress.notify_all();
    if (ran) {
      std::lock_guard lock(g_stats_mutex);
      g_stats.executed += ran;
    }
  }
}

}  // namespace

NativeInlineScope::NativeInlineScope() {
  if (NativeFullDrawPreparationEnabled() && !t_inline_scope) NativeWorkerDrain();
  ++t_inline_scope;
}
NativeInlineScope::~NativeInlineScope() { --t_inline_scope; }

NativeReplayLock::NativeReplayLock() { NativeRecordingMutex().LockReplay(); }
NativeReplayLock::~NativeReplayLock() { NativeRecordingMutex().UnlockReplay(); }

bool NativeWorkerShouldQueue() {
  return NativeWorkerEnabled() && t_inline_scope == 0 && !t_running_task && !OnNativeWorkerThread();
}

NativeRecordingGate& NativeRecordingMutex() {
  static NativeRecordingGate mutex;
  return mutex;
}

bool NativeWorkerEnabled() {
  static const bool enabled = [] {
    const char* value = std::getenv("LEGO_NATIVE_THREADED");
    const bool on = value && std::strcmp(value, "1") == 0;
    REXLOG_INFO("Native GPU: threaded recording {}", on ? "ON (LEGO_NATIVE_THREADED=1)" : "off");
    return on;
  }();
  return enabled;
}

bool NativeWorkerStarted() {
  return g_started.load(std::memory_order_acquire);
}

bool OnNativeWorkerThread() {
  return t_running_task || (g_started.load(std::memory_order_acquire) && GetCurrentThreadId() == g_worker_thread_id);
}
bool OnNativeRecorderThread() { return t_on_recorder; }

void StartNativeWorker() {
  if (!NativeWorkerEnabled()) return;
  static std::once_flag once;
  std::call_once(once, [] {
    {
      std::lock_guard lock(g_queue_mutex);
      g_stop = false;
    }
    if (NativeFullDrawPreparationEnabled()) g_recorder_thread = std::thread(RecorderMain);
    g_thread = std::thread(WorkerMain);
    g_worker_thread_id = GetThreadId(g_thread.native_handle());
    SetThreadPriority(g_thread.native_handle(), THREAD_PRIORITY_ABOVE_NORMAL);
    g_started.store(true, std::memory_order_release);
  });
}

// Caller holds g_stage_mutex.
void FlushStagedCapturesLocked() {
  if (g_staged_captures.empty()) return;
  const size_t count = g_staged_captures.size();
  std::function<void()> run = [captures = std::move(g_staged_captures)]() mutable {
    for (auto& capture : captures) capture();
  };
  g_staged_captures = {};
  g_staged_captures.reserve(kStagedCaptureRun);
  {
    // No wait for space here: callers apply backpressure after releasing g_stage_mutex.
    std::lock_guard lock(g_queue_mutex);
    g_queue.push_back({std::move(run), false, true, true, NativeTaskStage::kDeferred});
  }
  g_queue_ready.notify_one();
  std::lock_guard stats(g_stats_mutex);
  g_stats.submitted += count;
}

// Bounded so a stalled worker throttles the guest; waited for with no other lock held.
void WaitForQueueSpace() {
  std::unique_lock lock(g_queue_mutex);
  g_queue_progress.wait(lock, [] { return g_queue.size() < kMaxQueuedTasks; });
}

void FlushStagedCaptures() {
  std::lock_guard stage(g_stage_mutex);
  FlushStagedCapturesLocked();
}

bool StagedCapturesEnabled() {
  static const bool enabled = [] {
    const char* value = std::getenv("LEGO_NATIVE_STAGED_CAPTURES");
    return !value || value[0] != '0';
  }();
  return enabled;
}

void NativeWorkerSubmitCapture(std::function<void()> task) {
  if (!StagedCapturesEnabled()) {
    NativeWorkerSubmit(std::move(task), false, true, true);
    return;
  }
  StartNativeWorker();
  bool flushed = false;
  {
    std::lock_guard stage(g_stage_mutex);
    g_staged_captures.push_back(std::move(task));
    if (g_staged_captures.size() >= kStagedCaptureRun) {
      FlushStagedCapturesLocked();
      flushed = true;
    }
  }
  if (flushed) WaitForQueueSpace();
}

void NativeWorkerSubmit(std::function<void()> task, bool present, bool preserve_draw_batch, bool capture,
                        NativeTaskStage stage) {
  StartNativeWorker();
  WaitForQueueSpace();
  std::lock_guard staging(g_stage_mutex);
  FlushStagedCapturesLocked();
  {
    std::lock_guard lock(g_queue_mutex);
    g_queue.push_back({std::move(task), present, preserve_draw_batch, capture, stage});
    if (present) ++g_pending_presents;
  }
  g_queue_ready.notify_one();
  std::lock_guard lock(g_stats_mutex);
  ++g_stats.submitted;
}

void NativeWorkerSubmitPoolCopy(std::function<void()> task) {
  StartNativeWorker();
  WaitForQueueSpace();
  std::lock_guard staging(g_stage_mutex);
  FlushStagedCapturesLocked();
  {
    std::lock_guard lock(g_queue_mutex);
    Task item{std::move(task), false, true, true, NativeTaskStage::kDeferred};
    item.pool_copy = true;
    g_queue.push_back(std::move(item));
  }
  g_queue_ready.notify_one();
  std::lock_guard lock(g_stats_mutex);
  ++g_stats.submitted;
}

void NativeRecorderSubmit(std::function<void()> task, bool present, bool barrier, std::function<void()> prepare) {
  {
    std::lock_guard lock(g_queue_mutex);
    const auto sequence = g_recorder_submitted.fetch_add(1, std::memory_order_relaxed) + 1;
    if (barrier) g_recorder_barrier.store(sequence, std::memory_order_release);
    g_record_queue.push_back({std::move(task), present, false, false, NativeTaskStage::kRecorder, sequence, std::move(prepare)});
    ++g_record_pending;
  }
  g_record_ready.notify_one();
}

unsigned long long NativeNonCaptureTasks() { return g_noncapture_tasks.load(std::memory_order_acquire); }

bool NativeWorkerExchangeRunningTask(bool running) {
  return std::exchange(t_running_task, running);
}

void NativeNameCurrentThread(const wchar_t* name) { SetThreadDescription(GetCurrentThread(), name); }

unsigned long long NativeRecorderBarrier() {
  return g_recorder_barrier.load(std::memory_order_acquire);
}

void NativeRecorderWaitExecuted(unsigned long long sequence) {
  if (!sequence || g_recorder_executed.load(std::memory_order_acquire) >= sequence) return;
  std::unique_lock lock(g_recorder_mutex);
  g_recorder_progress.wait(lock, [sequence] {
    return g_recorder_executed.load(std::memory_order_acquire) >= sequence || g_stop;
  });
}

bool NativeQueuedResolveEnabled() {
  static const bool enabled = [] {
    const char* value = std::getenv("LEGO_NATIVE_QUEUED_RESOLVE");
    const bool selected = !value || std::strcmp(value, "0") != 0;
    REXLOG_INFO("Native resolve/clear: {}", selected ? "queued" : "inline drain");
    return selected;
  }();
  return enabled;
}

namespace {
bool EnvSwitch(const char* name, const char* label) {
  const char* value = std::getenv(name);
  const bool selected = !value || std::strcmp(value, "0") != 0;
  REXLOG_INFO("Native {}: {}", label, selected ? "queued" : "inline drain");
  return selected;
}
}  // namespace

bool NativeQueuedGetEnabled() {
  static const bool enabled = EnvSwitch("LEGO_NATIVE_QUEUED_GET", "GetRenderTarget");
  return enabled;
}

bool NativeDeferredReleaseEnabled() {
  static const bool enabled = EnvSwitch("LEGO_NATIVE_DEFERRED_RELEASE", "Release");
  return enabled;
}

bool NativeQueuedSurfaceClearEnabled() {
  static const bool enabled = EnvSwitch("LEGO_NATIVE_QUEUED_SURFACE_CLEAR", "surface clear");
  return enabled;
}

void NativeWorkerDrain(void* caller) {
  if (!g_started.load(std::memory_order_acquire) || t_running_task || OnNativeWorkerThread()) return;
  FlushStagedCaptures();
  if (NativeFullDrawPreparationEnabled()) {
    if (t_inline_scope || NativeRecordingMutex().HeldByCurrentThread()) return;
    const auto start = Clock::now();
    std::unique_lock lock(g_queue_mutex);
    const auto empty = [] { return g_queue.empty() && !g_executing && !g_record_pending && !NativeDrawBatchPending(); };
    if (empty()) return;
    bool completed = false;
    g_queue.push_back({[&completed] {
      std::lock_guard completion_lock(g_queue_mutex);
      completed = true;
      g_queue_progress.notify_all();
    }, false, false, false, NativeTaskStage::kRecorder});
    g_queue_ready.notify_one();
    g_queue_progress.wait(lock, [&completed] { return completed; });
    lock.unlock();
    RecordDrain(caller, Clock::now() - start);
    return;
  }
  {
    std::lock_guard lock(g_queue_mutex);
    if (g_queue.empty() && !g_executing && !NativeDrawBatchPending()) return;
  }
  const auto start = Clock::now();
  auto& recording = NativeRecordingMutex();
  for (;;) {
    if (recording.try_lock()) {
      // Either this thread already owned the lock (nested hook) or it was free:
      // the worker is outside any task, so the remaining tasks run here in order.
      RunQueuedInline();
      recording.unlock();
      break;
    }
    std::unique_lock lock(g_queue_mutex);
    if (g_queue_progress.wait_for(lock, std::chrono::microseconds(200),
            [] { return g_queue.empty() && !g_executing && !NativeDrawBatchPending(); }))
      break;
  }
  RecordDrain(caller, Clock::now() - start);
}

void NativeWorkerDrainCapture(std::function<void()> body) {
  if (!NativeFullDrawPreparationEnabled() || !g_started.load(std::memory_order_acquire) ||
      t_running_task || OnNativeWorkerThread() || t_inline_scope) {
    if (body) body();
    NativeWorkerDrain();
    return;
  }
  FlushStagedCaptures();
  const auto start = Clock::now();
  std::unique_lock lock(g_queue_mutex);
  bool completed = false;
  g_queue.push_back({[&completed, &body] {
    if (body) body();
    std::lock_guard completion_lock(g_queue_mutex);
    completed = true;
    g_queue_progress.notify_all();
  }, false, true, true});
  g_queue_ready.notify_one();
  g_queue_progress.wait(lock, [&completed] { return completed; });
  lock.unlock();
  std::lock_guard stats(g_stats_mutex);
  g_stats.wait_us += std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start).count();
}

void NativeWorkerThrottleFrames(unsigned frames) {
  if (!g_started.load(std::memory_order_acquire) || OnNativeWorkerThread()) return;
  FlushStagedCaptures();
  const auto start = Clock::now();
  std::unique_lock lock(g_queue_mutex);
  g_queue_progress.wait(lock, [frames] { return g_pending_presents <= frames; });
  lock.unlock();
  std::lock_guard stats(g_stats_mutex);
  g_stats.throttle_us += std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start).count();
}

void StopNativeWorker() {
  if (!g_started.load(std::memory_order_acquire)) return;
  FlushStagedCaptures();
  NativeWorkerDrain();
  {
    std::lock_guard lock(g_queue_mutex);
    g_stop = true;
  }
  g_queue_ready.notify_all();
  g_record_ready.notify_all();
  g_recorder_progress.notify_all();
  if (g_thread.joinable()) g_thread.join();
  if (g_recorder_thread.joinable()) g_recorder_thread.join();
  if (NativeFullDrawPreparationEnabled()) StopNativePreparation();
  g_started.store(false, std::memory_order_release);
}

NativeWorkerStats ConsumeNativeWorkerStats() {
  std::lock_guard lock(g_stats_mutex);
  const auto stats = g_stats;
  g_stats = {};
  return stats;
}

}  // namespace legodimensions::gpu_native
