#include "gpu_native/pipeline_creation_gate.h"

#include <array>
#include <atomic>
#include <barrier>
#include <cassert>
#include <chrono>
#include <future>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

using legodimensions::gpu_native::PipelineCreationGate;
using namespace std::chrono_literals;

int main() {
  PipelineCreationGate<int> gate;
  // Independent keys must reach the driver at the same time. A global
  // compilation lock cannot satisfy this rendezvous (CTest bounds the wait).
  {
    std::barrier start(3);
    std::array<std::future<void>, 3> jobs;
    for (int key = 0; key < 3; ++key) {
      jobs[key] = std::async(std::launch::async, [&, key] {
        auto lease = gate.Acquire(key);
        start.arrive_and_wait();
      });
    }
    for (auto& job : jobs) job.get();
  }
  // Many concurrent misses for one key share one immutable result. Include
  // a deliberate failed attempt before the successful publication.
  {
    std::atomic<int> calls{0};
    int published = 0;
    std::barrier start(24);
    std::vector<std::future<int>> jobs;
    for (int worker = 0; worker < 24; ++worker) {
      jobs.push_back(std::async(std::launch::async, [&] {
        start.arrive_and_wait();
        for (;;) {
          try {
            auto lease = gate.Acquire(42);
            if (!published) {
              if (++calls == 1) throw std::runtime_error("driver failure");
              std::this_thread::sleep_for(5ms);
              published = 0x1234;
            }
            return published;
          } catch (const std::runtime_error&) {}
        }
      }));
    }
    for (auto& job : jobs) {
      assert(job.wait_for(5s) == std::future_status::ready);
      assert(job.get() == 0x1234);
    }
    assert(calls == 2);
  }
  // Repeated contention and lease moves preserve mutual exclusion per key.
  {
    std::array<int, 17> values{};
    std::vector<std::thread> workers;
    for (int thread = 0; thread < 12; ++thread) {
      workers.emplace_back([&] {
        for (int i = 0; i < 17000; ++i) {
          auto lease = gate.Acquire(i % values.size());
          auto moved = std::move(lease);
          ++values[i % values.size()];
        }
      });
    }
    for (auto& worker : workers) worker.join();
    for (auto value : values) assert(value == 12000);
  }
  gate.ClearAfterDrain();
  { auto lease = gate.Acquire(42); }
  gate.ClearAfterDrain();
  std::cout << "pipeline creation: independent overlap, duplicate suppression, "
               "failure recovery, contention and teardown passed\n";
}
