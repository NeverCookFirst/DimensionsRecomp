#include "gpu_native/vertex_layout_plan.h"
#include <cassert>
#include <random>
#include <vector>

using namespace legodimensions::gpu_native;
struct Element { uint32_t slotIndex, alignedByteOffset, format; };
struct Declaration {
  std::array<Element, 80> elements{};
  uint32_t element_count = 0;
  uint64_t reversed_byte_elements = 0, half_swapped_elements = 0, half_swapped_wide_elements = 0;
};
int main() {
  std::mt19937_64 random(0x83FB9F10);
  for (unsigned trial = 0; trial < 20000; ++trial) {
    Declaration declaration;
    declaration.element_count = random() % 81;
    declaration.reversed_byte_elements = random();
    declaration.half_swapped_elements = random();
    declaration.half_swapped_wide_elements = random();
    for (auto& e : declaration.elements)
      e = {uint32_t(trial % 3 ? random() % 16 : 0), uint32_t(random() % 64) * 4, uint32_t(random() % 5) * 4};
    if (trial % 7 == 0) declaration.elements[0].alignedByteOffset = UINT32_MAX - 3;
    if (trial % 11 == 0) declaration.elements[0].slotIndex = 16;
    auto plan = PrepareVertexLayout(declaration, [](uint32_t format) { return format; });
    bool valid = true;
    for (uint32_t e = 0; e < declaration.element_count; ++e)
      valid &= declaration.elements[e].slotIndex < 16;
    assert(plan.valid == valid);
    if (!valid) continue;
    for (uint32_t slot = 0; slot < 16; ++slot) {
      std::vector<uint32_t> fields;
      bool active = false, widths_valid = true;
      uint64_t minimum = 0;
      for (uint32_t e = 0; e < declaration.element_count; ++e) {
        const auto& element = declaration.elements[e];
        if (element.slotIndex != slot) continue;
        active = true;
        widths_valid &= element.format != 0;
        minimum = std::max(minimum, uint64_t(element.alignedByteOffset) + element.format);
        if (e < 64 && (declaration.reversed_byte_elements & (uint64_t{1} << e)))
          fields.push_back(element.alignedByteOffset);
      }
      for (uint32_t e = 0; e < std::min(declaration.element_count, 64u); ++e) {
        if (declaration.elements[e].slotIndex != slot || !(declaration.half_swapped_elements & (uint64_t{1} << e))) continue;
        const uint32_t count = declaration.half_swapped_wide_elements & (uint64_t{1} << e) ? 2 : 1;
        for (uint32_t d = 0; d < count && fields.size() < 64; ++d)
          fields.push_back((declaration.elements[e].alignedByteOffset + 4 * d) | kHalfSwapField);
      }
      std::sort(fields.begin(), fields.end());
      fields.erase(std::unique(fields.begin(), fields.end()), fields.end());
      const auto& stream = plan.streams[slot];
      assert(active == stream.active && widths_valid == stream.widths_valid && minimum == stream.minimum_stride);
      assert(std::equal(fields.begin(), fields.end(), stream.Reversal().begin(), stream.Reversal().end()));
    }
  }
}
