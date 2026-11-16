#define NOMINMAX
#include "gpu_native/native_preparation.h"
#include <array>
#include <atomic>
#include <cassert>
#include <cstdio>
#include <latch>
#include <set>
#include <stdexcept>

using namespace legodimensions::gpu_native;

int main() {
  NativePreparationQueue queue(3);
  std::latch started(3), release(1);
  std::array<DWORD, 3> ids{};
  std::vector<std::future<void>> done;
  for (unsigned i = 0; i < ids.size(); ++i)
    done.push_back(queue.Post([&, i] {
      ids[i] = GetCurrentThreadId(); started.count_down(); release.wait();
    }, 64));
  started.wait();
  assert(std::set<DWORD>(ids.begin(), ids.end()).size() == 3);
  std::atomic<unsigned> completed{0};
  for (unsigned i = 0; i < 512; ++i)
    done.push_back(queue.Post([&] { ++completed; }, 1024 * 1024));
  assert(completed > 0);
  release.count_down();
  for (auto& result : done) result.get();
  assert(completed == 512);
  auto failure = queue.Post([] { throw std::runtime_error("fixture"); }, 0);
  bool rejected = false;
  try { failure.get(); } catch (const std::runtime_error&) { rejected = true; }
  assert(rejected);
  queue.Post([&] { ++completed; }, 0).get();
  assert(completed == 513);
  std::puts("PASS three concurrent workers, bounded fallback, completion and failure propagation");
}
