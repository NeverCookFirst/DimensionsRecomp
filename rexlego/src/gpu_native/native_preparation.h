#pragma once

#include <windows.h>
#include <condition_variable>
#include <deque>
#include <functional>
#include <future>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace legodimensions::gpu_native {

// Jobs retain captured state and serialize mutable resource access.
class NativePreparationQueue {
  struct Job { std::function<void()> run; size_t bytes; };
  std::mutex mutex_;
  std::condition_variable ready_;
  std::deque<Job> jobs_;
  std::vector<std::thread> workers_;
  size_t bytes_ = 0;
  bool stopping_ = false;
  void Run(unsigned index) {
    const auto name = L"Native GPU prepare " + std::to_wstring(index + 1);
    SetThreadDescription(GetCurrentThread(), name.c_str());
    for (;;) {
      Job job;
      {
        std::unique_lock lock(mutex_);
        ready_.wait(lock, [&] { return stopping_ || !jobs_.empty(); });
        if (jobs_.empty()) return;
        job = std::move(jobs_.front()); jobs_.pop_front();
      }
      job.run();
      { std::lock_guard lock(mutex_); bytes_ -= job.bytes; }
    }
  }
  void Stop() {
    { std::lock_guard lock(mutex_); stopping_ = true; }
    ready_.notify_all();
    for (auto& worker : workers_) if (worker.joinable()) worker.join();
  }
 public:
  explicit NativePreparationQueue(unsigned count) {
    try { for (unsigned i = 0; i < count; ++i) workers_.emplace_back([this, i] { Run(i); }); }
    catch (...) { Stop(); throw; }
  }
  ~NativePreparationQueue() { Stop(); }
  void Shutdown() { Stop(); }
  // Runs inline when the queue is full, like Post, but returns no future.
  void PostDetached(std::function<void()> work, size_t bytes) {
    {
      std::lock_guard lock(mutex_);
      if (!stopping_ && !workers_.empty() && jobs_.size() < 4096 &&
          bytes <= (256u << 20) && bytes_ <= (256u << 20) - bytes) {
        jobs_.push_back({std::move(work), bytes});
        bytes_ += bytes;
        ready_.notify_one();
        return;
      }
    }
    work();
  }
  template<class Work>
  std::future<void> Post(Work work, size_t bytes) {
    auto task = std::make_shared<std::packaged_task<void()>>(std::move(work));
    auto ready = task->get_future();
    {
      std::lock_guard lock(mutex_);
      if (!stopping_ && !workers_.empty() && jobs_.size() < 4096 &&
          bytes <= (256u << 20) && bytes_ <= (256u << 20) - bytes) {
        jobs_.push_back({[task] { (*task)(); }, bytes});
        bytes_ += bytes;
        ready_.notify_one();
        return ready;
      }
    }
    (*task)();
    return ready;
  }
};

} // namespace legodimensions::gpu_native
