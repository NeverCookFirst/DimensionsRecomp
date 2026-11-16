#include "gpu_native/preparation_dependencies.h"

#include <cassert>
#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <vector>

using namespace std::chrono_literals;
using namespace legodimensions::gpu_native;

struct Bindings {
  std::array<unsigned, 32> textures{};
  std::array<unsigned, 4> render_targets{};
  unsigned depth_stencil = 0;
};

PreparationDependencyFilter Filter(unsigned address) {
  PreparationDependencyFilter result;
  result.Add(address);
  return result;
}

void CheckBindings() {
  Bindings bindings;
  unsigned address = 0x80001000;
  for (auto& texture : bindings.textures) { texture = address; address += 64; }
  for (auto& target : bindings.render_targets) { target = address; address += 64; }
  bindings.depth_stencil = address;
  PreparationDependencyFilter filter;
  filter.AddBindings(bindings);
  for (unsigned texture : bindings.textures) assert(filter.Matches(texture));
  for (unsigned target : bindings.render_targets) assert(filter.Matches(target));
  assert(filter.Matches(bindings.depth_stencil));
  assert(!filter.Matches(0u));
  const auto before = filter.count;
  filter.Add(0);
  assert(filter.count == before);
  assert(!filter.Matches(address + 4096));
}

void CheckWait(bool per_chunk) {
  constexpr unsigned related = 0x80001000;
  unsigned unrelated = related + 64;
  while (Filter(unrelated).Matches(related)) unrelated += 64;
  std::promise<void> finish_related, finish_unrelated, started;
  std::vector<PreparationDependencyJob> jobs{
      {finish_related.get_future().share(), Filter(related)},
      {finish_unrelated.get_future().share(), Filter(unrelated)}};
  auto waiting = std::async(std::launch::async, [&] {
    started.set_value();
    return WaitPreparationDependencies(jobs, {related}, per_chunk);
  });
  started.get_future().wait();
  assert(waiting.wait_for(50ms) == std::future_status::timeout);
  finish_related.set_value();
  if (per_chunk) {
    assert(waiting.wait_for(5s) == std::future_status::ready);
  } else {
    assert(waiting.wait_for(50ms) == std::future_status::timeout);
  }
  finish_unrelated.set_value();
  assert(waiting.wait_for(5s) == std::future_status::ready);
  assert(waiting.get());
}

void CheckMultipleAddresses() {
  constexpr unsigned first = 0x80001000, second = 0x90002200;
  std::promise<void> finish_first, finish_second, started;
  std::vector<PreparationDependencyJob> jobs{
      {finish_first.get_future().share(), Filter(first)},
      {finish_second.get_future().share(), Filter(second)}};
  auto waiting = std::async(std::launch::async, [&] {
    started.set_value();
    return WaitPreparationDependencies(jobs, {0, first, second}, true);
  });
  started.get_future().wait();
  finish_first.set_value();
  assert(waiting.wait_for(50ms) == std::future_status::timeout);
  finish_second.set_value();
  assert(waiting.wait_for(5s) == std::future_status::ready);
  assert(waiting.get());
}

void CheckCollision() {
  // Two addresses that probe the same slot stay distinct.
  std::array<unsigned, PreparationDependencyFilter::kSlots> seen{};
  unsigned original = 0, collision = 0;
  for (unsigned address = 1; !collision; ++address) {
    const unsigned slot = PreparationDependencyFilter::Slot(address);
    if (seen[slot]) { original = seen[slot]; collision = address; }
    else seen[slot] = address;
  }
  assert(original != collision);
  assert(!Filter(original).Matches(collision));
  PreparationDependencyFilter both;
  both.Add(original);
  both.Add(collision);
  assert(both.Matches(original) && both.Matches(collision) && both.count == 2);
  // Past three quarters full every address matches, the conservative answer.
  PreparationDependencyFilter full;
  for (unsigned i = 1; i <= PreparationDependencyFilter::kSlots; ++i) full.Add(i * 64);
  assert(full.overflow && full.Matches(0x12345678u));
}

void CheckNoMatchAndException() {
  constexpr unsigned address = 0x80001000;
  std::promise<void> finish;
  std::array<PreparationDependencyJob, 1> jobs{{{finish.get_future().share(), Filter(address)}}};
  assert(!WaitPreparationDependencies(jobs, {}, true));
  assert(!WaitPreparationDependencies(jobs, {0}, true));
  unsigned unrelated = address + 64;
  while (jobs[0].dependencies.Matches(unrelated)) unrelated += 64;
  assert(!WaitPreparationDependencies(jobs, {unrelated}, true));
  finish.set_exception(std::make_exception_ptr(std::runtime_error("preparation failed")));
  assert(WaitPreparationDependencies(jobs, {address}, true));
  bool threw = false;
  try { jobs[0].ready.get(); } catch (const std::runtime_error&) { threw = true; }
  assert(threw);
}

int main() {
  CheckBindings();
  CheckWait(true);
  CheckWait(false);
  CheckMultipleAddresses();
  CheckCollision();
  CheckNoMatchAndException();
  std::puts("PASS production dependency filters, related and unrelated futures, broad fallback, multi-address waits, collisions and exception retention");
}
