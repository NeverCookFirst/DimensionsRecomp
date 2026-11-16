#pragma once
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <span>

namespace legodimensions::gpu_native {
// Xbox 360 manual instancing. TT's grass vertex shaders compute their own
// vfetch indices from the vertex index (r0.x on Xenos):
//   template stream (POSITION0, TEXCOORD0, NORMAL0) at (index mod N),
//   per-instance stream (every other input)          at (index div N),
// with N = VS constant c93.x. Verified from the archived microcode (vfetch
// source registers r0.x / r3.y). The translated shaders read IA inputs at the
// plain index, so these draws fetched far past the 60-vertex template:
// Chima spikes/sheets, LOTR stretched geometry and "rejected vertex fetch"
// (VS 500E11F5871DD2AA/A6B647C1BCEB33F9). For a sequential index list the
// mapping is exactly D3D12 instancing: N vertices x count/N instances with
// the per-instance stream stepped once per instance.
// F77D99A4104EA49B / 1DAAB5453FCD1821 use a different, predicated index
// formula and are not covered here.
inline bool InstancedFetchShader(uint64_t hash) {
  static const bool enabled = [] {
    const char* value = std::getenv("LEGO_NATIVE_INSTANCED_FETCH");
    return !value || std::strcmp(value, "0") != 0;
  }();
  return enabled && (hash == 0x500E11F5871DD2AAull || hash == 0xA6B647C1BCEB33F9ull ||
                     hash == 0xAC79D352DE7484A0ull);
}

// VS float constant register holding N.
constexpr uint32_t kInstancedFetchCountRegister = 93;

// A draw starting inside an instance is split into a partial first instance,
// whole instances and a partial last instance (at most three host draws).
struct InstancedFetchPlan {
  uint32_t vertices = 0;        // N, vertices per instance
  uint32_t first_instance = 0;  // instance of the first vertex
  uint32_t head_start = 0;      // template vertex of the first vertex
  uint32_t head_count = 0;      // vertices of a partial first instance
  uint32_t instances = 0;       // whole instances after the head
  uint32_t tail_count = 0;      // vertices of a partial last instance
  uint32_t instance_slots = 0;  // input slots stepped once per instance (PSO only when !expand)
  // Non-sequential index lists cannot become instancing. The draw is then
  // de-indexed instead: every used stream is gathered on the CPU so record j
  // holds the template/instance record of guest vertex expand_vertices[j].
  bool expand = false;
  uint32_t template_slots = 0;
  const uint32_t* expand_vertices = nullptr;
  uint32_t expand_count = 0;
  explicit operator bool() const { return vertices != 0; }
  uint32_t last_instance() const {
    return first_instance + (head_count ? 1 : 0) + instances + (tail_count ? 1 : 0) - 1;
  }
};

inline bool InstancedFetchTemplateInput(const char* name, uint32_t index) {
  return index == 0 && (!std::strcmp(name, "POSITION") || !std::strcmp(name, "TEXCOORD") ||
                        !std::strcmp(name, "NORMAL"));
}
// Inputs these shaders fetch with the per-instance index.
inline bool InstancedFetchInstanceInput(const char* name, uint32_t index) {
  if (!std::strcmp(name, "TANGENT")) return index <= 1;
  if (!std::strcmp(name, "TEXCOORD")) return index >= 1 && index <= 3;
  if (!std::strcmp(name, "COLOR")) return index == 1;
  return index == 0 && (!std::strcmp(name, "BLENDINDICES") || !std::strcmp(name, "BLENDWEIGHT"));
}

// Template and per-instance input slots; false when a slot mixes both.
template <class Element>
inline bool ClassifyInstancedFetchSlots(std::span<const Element> elements,
                                        uint32_t& template_slots, uint32_t& instance_slots) {
  template_slots = instance_slots = 0;
  if constexpr (!requires(const Element& e) { e.semanticName; e.semanticIndex; e.slotIndex; }) {
    return false;
  } else {
    for (const auto& element : elements) {
      if (element.slotIndex >= 32) return false;
      const uint32_t bit = 1u << element.slotIndex;
      if (InstancedFetchTemplateInput(element.semanticName, element.semanticIndex)) template_slots |= bit;
      else if (InstancedFetchInstanceInput(element.semanticName, element.semanticIndex)) instance_slots |= bit;
    }
    return template_slots && instance_slots && !(template_slots & instance_slots);
  }
}

// De-indexing plan for any (index-list order) draw of these shaders.
template <class Element>
inline InstancedFetchPlan PlanInstancedFetchExpand(float n_value, const uint32_t* vertices,
                                                   uint32_t count, std::span<const Element> elements) {
  if (!(n_value >= 1.0f && n_value <= 65536.0f) || !vertices || !count || count % 3) return {};
  const uint32_t n = uint32_t(n_value);
  if (float(n) != n_value) return {};
  InstancedFetchPlan plan;
  if (!ClassifyInstancedFetchSlots(elements, plan.template_slots, plan.instance_slots)) return {};
  plan.vertices = n;
  plan.expand = true;
  plan.expand_vertices = vertices;
  plan.expand_count = count;
  return plan;
}

// out[j] = record (template ? v % n : v / n) of `source`, v = vertices[j].
// Returns false when a record lies outside source_bytes.
inline bool GatherInstancedFetchRecords(uint8_t* out, const uint8_t* source, uint64_t source_bytes,
                                        uint32_t stride, const uint32_t* vertices, uint32_t count,
                                        uint32_t n, bool per_instance) {
  if (!stride || !n) return false;
  for (uint32_t j = 0; j < count; ++j) {
    const uint64_t record = per_instance ? vertices[j] / n : vertices[j] % n;
    const uint64_t offset = record * stride;
    if (offset + stride > source_bytes) return false;
    std::memcpy(out + uint64_t(j) * stride, source + offset, stride);
  }
  return true;
}

// `first` is the guest vertex index of the first vertex of a sequential draw.
template <class Element>
inline InstancedFetchPlan PlanInstancedFetch(float n_value, uint32_t first, uint32_t count,
                                             std::span<const Element> elements) {
  if constexpr (!requires(const Element& e) { e.semanticName; e.semanticIndex; e.slotIndex; }) {
    return {};
  } else {
  if (!(n_value >= 3.0f && n_value <= 65536.0f)) return {};
  const uint32_t n = uint32_t(n_value);
  // Triangle-list boundaries must coincide with instance boundaries.
  if (float(n) != n_value || n % 3 || !count || count % 3 || (first % n) % 3) return {};
  uint32_t template_slots = 0, instance_slots = 0;
  for (const auto& element : elements) {
    if (element.slotIndex >= 32) return {};
    const uint32_t bit = 1u << element.slotIndex;
    if (InstancedFetchTemplateInput(element.semanticName, element.semanticIndex)) template_slots |= bit;
    else if (InstancedFetchInstanceInput(element.semanticName, element.semanticIndex)) instance_slots |= bit;
  }
  // Each stream must be fetched with one index only.
  if (!template_slots || !instance_slots || (template_slots & instance_slots)) return {};
  InstancedFetchPlan plan;
  plan.vertices = n;
  plan.first_instance = first / n;
  plan.head_start = first % n;
  plan.head_count = plan.head_start ? (count < n - plan.head_start ? count : n - plan.head_start) : 0;
  plan.instances = (count - plan.head_count) / n;
  plan.tail_count = (count - plan.head_count) % n;
  plan.instance_slots = instance_slots;
  return plan;
  }
}

// True when the big-endian guest index list is first, first+1, ... (TT draws
// its instanced grass with a sequential list).
inline bool SequentialGuestIndices(const uint8_t* data, uint32_t count, uint32_t element_size,
                                   uint32_t& first) {
  if (!data || !count || (element_size != 2 && element_size != 4)) return false;
  const auto read = [&](uint32_t i) -> uint32_t {
    const uint8_t* p = data + size_t(i) * element_size;
    return element_size == 2 ? (uint32_t(p[0]) << 8 | p[1])
                             : (uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]);
  };
  first = read(0);
  for (uint32_t i = 1; i < count; ++i)
    if (read(i) != first + i) return false;
  return true;
}
}  // namespace legodimensions::gpu_native
