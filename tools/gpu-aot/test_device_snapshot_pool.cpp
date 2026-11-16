#include "gpu_native/device_snapshot_pool.h"
#include "gpu_native/renderer_optimizations.h"
#include <atomic>
#include <cassert>
#include <cstdio>
#include <fstream>
#include <thread>

using namespace legodimensions::gpu_native;
struct Device { std::array<unsigned char, 512> bytes; };
int main(int argc, char** argv) {
  assert(argc == 2);
  _putenv_s("LEGO_NATIVE_RENDER_OPTIMIZATIONS_FILE", argv[1]);
  for (const auto [text, expected_mask] : {std::pair{"0", 0u}, {"7", 7u}, {"8", 7u}, {"0 trailing", 7u}, {"", 7u}, {"2", 2u}}) {
    { std::ofstream file(argv[1]); file << text; }
    for (unsigned i = 0; i < 60; ++i) PollRendererOptimizations();
    assert(RendererOptimizationMask().load() == expected_mask);
  }
  DeviceSnapshotPool<Device, 384, 4> pool;
  Device source{};
  source.bytes.fill(0x12);
  auto retained = pool.Capture(&source);
  assert(reinterpret_cast<uintptr_t>(retained.get()) % 64 == 0);
  for (size_t i = 384; i < 512; ++i) assert(retained->bytes[i] == 0);
  std::atomic<bool> finished{false};
  std::thread reader([held = retained, &finished] {
    while (!finished.load(std::memory_order_acquire))
      for (size_t i = 0; i < 384; ++i) assert(held->bytes[i] == 0x12);
  });
  std::array<std::shared_ptr<Device>, 13> pending;
  std::array<unsigned char, 13> expected{};
  for (unsigned step = 0; step < 100000; ++step) {
    for (size_t i = 0; i < pending.size(); ++i)
      if (pending[i]) assert(pending[i]->bytes[383] == expected[i]);
    const auto slot = step % pending.size();
    expected[slot] = static_cast<unsigned char>(step);
    source.bytes.fill(expected[slot]);
    pending[slot] = pool.Capture(&source);
    assert(pending[slot]->bytes[0] == expected[slot]);
    assert(pending[slot]->bytes[384] == 0);
  }
  finished.store(true, std::memory_order_release);
  reader.join();
  std::shared_ptr<Device> survives;
  {
    DeviceSnapshotPool<Device, 384, 4> temporary;
    survives = temporary.Capture(&source);
  }
  assert(survives->bytes[383] == source.bytes[383]);
  std::puts("PASS 100000 snapshots: immutable retained copies, concurrent reader, alignment, zero tail and pool destruction");
}
