#include "gpu_native/resource_registry.h"
#include <cassert>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

using namespace legodimensions::gpu_native;
struct Resource { uint32_t version = 0; };
int main() {
  ResourceRegistry<Resource> registry;
  std::mutex mutex;
  assert(!registry.Lookup(0, mutex));
  assert(!registry.Lookup(0x1000, mutex));
  auto original = std::make_shared<Resource>(1);
  registry.emplace(0x1000, original);
  assert(registry.Lookup(0x1000, mutex) == original);
  assert(registry.Lookup(0x1000, mutex) == original);
  auto replacement = std::make_shared<Resource>(2);
  registry[0x1000] = replacement;
  assert(registry.Lookup(0x1000, mutex) == replacement);
  registry.erase(registry.find(0x1000));
  assert(!registry.Lookup(0x1000, mutex));
  registry.emplace(0x1000, original);
  assert(registry.Lookup(0x1000, mutex) == original);
  registry.clear();
  assert(!registry.Lookup(0x1000, mutex));
  registry.emplace(0x1000, replacement);
  assert(registry.Lookup(0x1000, mutex) == replacement);
  ResourceRegistry<Resource> other;
  other.emplace(0x1000, original);
  assert(other.Lookup(0x1000, mutex) == original);
  assert(registry.Lookup(0x1000, mutex) == replacement);
  std::weak_ptr<Resource> lifetime = replacement;
  registry.clear(); replacement.reset();
  assert(lifetime.expired());
  alignas(ResourceRegistry<Resource>) std::array<uint8_t, sizeof(ResourceRegistry<Resource>)> storage;
  auto* reused = new (storage.data()) ResourceRegistry<Resource>;
  reused->emplace(0x1000, original);
  assert(reused->Lookup(0x1000, mutex) == original);
  reused->~ResourceRegistry<Resource>();
  reused = new (storage.data()) ResourceRegistry<Resource>;
  reused->emplace(0x1000, std::make_shared<Resource>(3));
  assert(reused->Lookup(0x1000, mutex)->version == 3);
  reused->~ResourceRegistry<Resource>();
  std::atomic<uint32_t> published{0}, observed{0};
  std::thread reader([&] {
    for (uint32_t step = 1; step <= 20000; ++step) {
      while (published.load(std::memory_order_acquire) != step) std::this_thread::yield();
      for (unsigned repeat = 0; repeat < 4; ++repeat) {
        auto found = registry.Lookup(0x1000, mutex);
        if (step % 3 == 0) assert(!found);
        else assert(found && found->version == step);
      }
      observed.store(step, std::memory_order_release);
    }
  });
  for (uint32_t step = 1; step <= 20000; ++step) {
    {
      std::lock_guard lock(mutex);
      if (step % 3 == 0) registry.clear();
      else registry[0x1000] = std::make_shared<Resource>(step);
    }
    published.store(step, std::memory_order_release);
    while (observed.load(std::memory_order_acquire) != step) std::this_thread::yield();
  }
  reader.join();
  registry.clear();
  for (uint32_t i = 0; i < 64; ++i) registry.emplace(0x80001000 + i * 256, std::make_shared<Resource>(i));
  uint64_t sum = 0;
  const auto start = std::chrono::steady_clock::now();
  for (uint32_t draw = 0; draw < 100000; ++draw)
    for (uint32_t slot = 0; slot < 16; ++slot) sum += registry.Lookup(0x80001000 + slot * 256, mutex)->version;
  const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
  assert(sum == 12000000);
  std::printf("PASS registry reuse, replacement, erase, reset, expired owner, registry address reuse and 20000 cross-thread publications; lookup replay %.3f ms\n", ms);
}
