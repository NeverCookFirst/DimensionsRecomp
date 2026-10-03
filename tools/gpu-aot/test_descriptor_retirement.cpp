#include "../../rexlego/src/gpu_native/descriptor_retirement.h"
#include <cassert>
#include <vector>

using legodimensions::gpu_native::DescriptorRetirement;
int main() {
  DescriptorRetirement retirement(8);
  std::vector<uint32_t> released;
  const auto free = [&](uint32_t slot) { released.push_back(slot); };
  assert(retirement.Retire(3, 0)); // No GPU references: immediate reuse.
  assert(!retirement.Retire(4, 0b111));
  assert(!retirement.Retire(4, 0b111)); // Repeated release adds no tickets.
  retirement.CompleteFrame(2, free);
  retirement.CompleteFrame(0, free);
  assert(released.empty()); // One older list is still using the descriptor.
  retirement.CompleteFrame(1, free);
  assert((released == std::vector<uint32_t>{4}));
  retirement.CompleteFrame(1, free);
  assert(released.size() == 1);
  assert(!retirement.Retire(4, 0b001)); // Reuse followed by another retirement.
  assert(!retirement.Retire(5, 0b010));
  retirement.CompleteFrame(0, free);
  assert(released.back() == 4 && released.size() == 2);
  retirement.CompleteFrame(1, free);
  assert(released.back() == 5 && released.size() == 3);
}
