#include <cassert>
#include <cstdint>
#include <iostream>
#include <vector>
#include "gpu_native/instanced_fetch.h"
using namespace legodimensions::gpu_native;

struct Element {
  const char* semanticName;
  uint32_t semanticIndex;
  uint32_t slotIndex;
};

int main() {
  // Captured LOTR/Chima grass layout: template stream 0, per-instance stream 1,
  // synthetic POSITION1..4 in slot 15.
  const std::vector<Element> grass{
      {"POSITION", 0, 0}, {"NORMAL", 0, 0}, {"COLOR", 0, 0}, {"TEXCOORD", 0, 0},
      {"TANGENT", 0, 1}, {"COLOR", 1, 1}, {"TEXCOORD", 2, 1}, {"TEXCOORD", 1, 1},
      {"TEXCOORD", 3, 1}, {"BLENDINDICES", 0, 1}, {"BLENDWEIGHT", 0, 1}, {"TANGENT", 1, 1},
      {"POSITION", 1, 15}, {"POSITION", 2, 15}};
  const auto plan = PlanInstancedFetch(60.0f, 0, 120, std::span<const Element>(grass));
  assert(plan && plan.vertices == 60 && plan.instances == 2 && plan.first_instance == 0);
  assert(plan.instance_slots == 2u);
  assert(!plan.head_count && !plan.tail_count && plan.last_instance() == 1);
  const auto later = PlanInstancedFetch(60.0f, 180, 240, std::span<const Element>(grass));
  assert(later && later.first_instance == 3 && later.instances == 4 && later.last_instance() == 6);
  // 30 + 60 + 60 + 15: partial head (template 30..59 of instance 0), two whole
  // instances, partial tail (template 0..14 of instance 3).
  const auto split = PlanInstancedFetch(60.0f, 30, 165, std::span<const Element>(grass));
  assert(split && split.first_instance == 0 && split.head_start == 30 && split.head_count == 30);
  assert(split.instances == 2 && split.tail_count == 15 && split.last_instance() == 3);
  // Short draw inside one instance.
  const auto inner = PlanInstancedFetch(60.0f, 63, 9, std::span<const Element>(grass));
  assert(inner && inner.first_instance == 1 && inner.head_start == 3 && inner.head_count == 9);
  assert(!inner.instances && !inner.tail_count && inner.last_instance() == 1);
  // Triangle boundaries off the instance grid, non-integer or tiny N stay unchanged.
  assert(!PlanInstancedFetch(60.0f, 0, 130, std::span<const Element>(grass)));
  assert(!PlanInstancedFetch(60.0f, 31, 120, std::span<const Element>(grass)));
  assert(!PlanInstancedFetch(60.5f, 0, 121, std::span<const Element>(grass)));
  assert(!PlanInstancedFetch(0.0f, 0, 120, std::span<const Element>(grass)));
  // A stream mixing both index kinds cannot be expressed as instancing.
  auto mixed = grass;
  mixed[4].slotIndex = 0;
  assert(!PlanInstancedFetch(60.0f, 0, 120, std::span<const Element>(mixed)));

  // Non-sequential lists are de-indexed: Chima N=85 with a shuffled list.
  const uint32_t shuffled[] = {0, 86, 1, 170, 85, 2};
  const auto expand = PlanInstancedFetchExpand(85.0f, shuffled, 6, std::span<const Element>(grass));
  assert(expand && expand.expand && expand.expand_count == 6 && expand.vertices == 85);
  assert(expand.template_slots == 1u && expand.instance_slots == 2u);
  assert(!PlanInstancedFetchExpand(85.0f, shuffled, 5, std::span<const Element>(grass)));
  assert(!PlanInstancedFetchExpand(85.0f, shuffled, 6, std::span<const Element>(mixed)));
  // Gather: template records by v % N, instance records by v / N.
  std::vector<uint8_t> source(4 * 200);
  for (uint32_t r = 0; r < 200; ++r) source[r * 4] = uint8_t(r);
  std::vector<uint8_t> out(6 * 4);
  assert(GatherInstancedFetchRecords(out.data(), source.data(), source.size(), 4, shuffled, 6, 85, false));
  assert(out[0] == 0 && out[4] == 1 && out[8] == 1 && out[12] == 0 && out[16] == 0 && out[20] == 2);
  assert(GatherInstancedFetchRecords(out.data(), source.data(), source.size(), 4, shuffled, 6, 85, true));
  assert(out[0] == 0 && out[4] == 1 && out[8] == 0 && out[12] == 2 && out[16] == 1 && out[20] == 0);
  assert(!GatherInstancedFetchRecords(out.data(), source.data(), 8, 4, shuffled, 6, 85, true));

  // Big-endian guest index lists.
  const uint8_t seq16[] = {0x00, 0x3C, 0x00, 0x3D, 0x00, 0x3E};
  uint32_t first = 0;
  assert(SequentialGuestIndices(seq16, 3, 2, first) && first == 60);
  const uint8_t gap16[] = {0x00, 0x00, 0x00, 0x02};
  assert(!SequentialGuestIndices(gap16, 2, 2, first));
  const uint8_t seq32[] = {0, 0, 1, 0, 0, 0, 1, 1};
  assert(SequentialGuestIndices(seq32, 2, 4, first) && first == 256);
  std::cout << "PASS instanced fetch planning and sequential index detection\n";
}
