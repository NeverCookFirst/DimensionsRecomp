#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <exception>
#include <functional>
#include <future>
#include <initializer_list>
#include <memory>
#include <span>
#include "gpu_native/preparation_footprint.h"

namespace legodimensions::gpu_native {
// Exact set of the guest addresses a batch or chunk may touch; past three quarters full it matches every address.
struct PreparationDependencyFilter {
  static constexpr unsigned kSlots = 512;
  std::array<unsigned, kSlots> slots{};
  unsigned count = 0;
  bool overflow = false;
  PreparationPhysicalFilter physical;

  static unsigned Slot(unsigned address) { return (address * 0x9E3779B1u) >> 23; }

  void Add(unsigned address) {
    if (!address || overflow) return;
    unsigned slot = Slot(address);
    while (slots[slot] && slots[slot] != address) slot = (slot + 1) & (kSlots - 1);
    if (slots[slot]) return;
    if (count >= kSlots * 3 / 4) { overflow = true; return; }
    slots[slot] = address;
    ++count;
  }

  bool Matches(unsigned address) const {
    if (!address) return false;
    if (overflow) return true;
    for (unsigned slot = Slot(address); slots[slot]; slot = (slot + 1) & (kSlots - 1))
      if (slots[slot] == address) return true;
    return false;
  }

  bool Matches(std::initializer_list<unsigned> addresses) const {
    for (const unsigned address : addresses) if (Matches(address)) return true;
    return false;
  }

  template <class Bindings>
  void AddBindings(const Bindings& bindings) {
    for (const unsigned texture : bindings.textures) Add(texture);
    for (const unsigned target : bindings.render_targets) Add(target);
    Add(bindings.depth_stencil);
  }
};
static_assert((PreparationDependencyFilter::kSlots & (PreparationDependencyFilter::kSlots - 1)) == 0);
static_assert(PreparationDependencyFilter::kSlots == 1u << (32 - 23));

// A posted chunk run by whichever thread claims it first, so a waiter can run it instead of blocking behind the queue.
struct PreparationChunkTask {
  std::atomic<bool> claimed{false};
  std::promise<void> done;
  std::function<void()> run;
  bool TryRun() {
    if (claimed.exchange(true, std::memory_order_acq_rel)) return false;
    try { run(); done.set_value(); } catch (...) { done.set_exception(std::current_exception()); }
    return true;
  }
};

struct PreparationDependencyJob {
  std::shared_future<void> ready;
  PreparationDependencyFilter dependencies;
  std::shared_ptr<PreparationChunkTask> task;
};

inline bool WaitPreparationDependencies(std::span<const PreparationDependencyJob> jobs,
    std::initializer_list<unsigned> addresses, bool per_chunk) {
  bool waited = false;
  for (const auto& job : jobs) {
    if (per_chunk && !job.dependencies.Matches(addresses)) continue;
    job.ready.wait();
    waited = true;
  }
  return waited;
}
}
