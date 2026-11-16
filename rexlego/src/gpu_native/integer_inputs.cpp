#include "gpu_native/integer_inputs.h"
#include "gpu_native/device.h"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <tuple>
#include <utility>
#include <rex/logging.h>

namespace legodimensions::gpu_native {
namespace {
struct IntegerInputEntry { uint64_t hash; uint32_t spec, mask, offset, size; };
#include "gpu_native/integer_input_bank.inc"

// Keep in sync with tools/gpu-aot/build_integer_input_bank.py.
int IntegerInputMaskBit(const char* name, uint32_t index) {
  if (!std::strcmp(name, "TEXCOORD")) return index < 8 ? int(index) : -1;
  if (!std::strcmp(name, "TANGENT")) return index < 2 ? 8 + int(index) : -1;
  if (!std::strcmp(name, "BLENDWEIGHT")) return index == 0 ? 10 : -1;
  if (!std::strcmp(name, "COLOR")) return index < 2 ? 11 + int(index) : -1;
  if (!std::strcmp(name, "NORMAL")) return index == 0 ? 13 : -1;
  if (!std::strcmp(name, "BINORMAL")) return index == 0 ? 14 : -1;
  return -1;
}

bool Enabled() {
  static const bool enabled = [] {
    const char* value = std::getenv("LEGO_NATIVE_INTEGER_INPUTS");
    return !value || std::strcmp(value, "0") != 0;
  }();
  return enabled;
}

const IntegerInputEntry* Find(uint64_t hash, uint32_t spec, uint32_t mask) {
  for (const auto& entry : kIntegerInputEntries)
    if (entry.hash == hash && entry.spec == (spec & 1u) && entry.mask == mask) return &entry;
  return nullptr;
}

std::mutex g_mutex;
std::map<std::tuple<uint64_t, uint32_t, uint32_t>, std::unique_ptr<plume::RenderShader>> g_shaders;
std::set<std::pair<uint64_t, uint32_t>> g_reported;
}  // namespace

static uint32_t RawIntegerInputMask(std::span<const plume::RenderInputElement> inputs) {
  uint32_t mask = 0;
  for (const auto& input : inputs) {
    if (input.format != plume::RenderFormat::R8G8B8A8_UINT &&
        input.format != plume::RenderFormat::R8G8B8A8_SINT) continue;
    const int bit = IntegerInputMaskBit(input.semanticName, input.semanticIndex);
    if (bit >= 0) mask |= 1u << bit;
  }
  return mask;
}
uint32_t IntegerInputMask(std::span<const plume::RenderInputElement> inputs) {
  return Enabled() ? RawIntegerInputMask(inputs) : 0;
}

bool HasIntegerInputShader(uint64_t hash, uint32_t spec, uint32_t mask) {
  return mask && Find(hash, spec, mask) != nullptr;
}

plume::RenderShader* ResolveIntegerInputShader(uint64_t hash, uint32_t spec, uint32_t mask) {
  if (!mask) return nullptr;
  std::lock_guard lock(g_mutex);
  const auto key = std::make_tuple(hash, spec & 1u, mask);
  if (const auto found = g_shaders.find(key); found != g_shaders.end()) return found->second.get();
  const auto* entry = Find(hash, spec, mask);
  if (!entry) {
    // Evidence for the next variant: the declaration feeds UINT bytes to a
    // shader that was translated with float4 inputs for these semantics.
    if (g_reported.size() < 64 && g_reported.emplace(hash, mask).second)
      REXLOG_WARN("Native GPU: integer vertex inputs without a float-conversion variant "
                  "VS={:016X} spec={} mask={:04X}", hash, spec & 1u, mask);
    return nullptr;
  }
  auto* device = HostDevice::Device();
  if (!device) return nullptr;
  auto shader = device->createShader(kIntegerInputBlob + entry->offset, entry->size, "main",
                                     plume::RenderShaderFormat::DXIL);
  if (!shader) return nullptr;
  REXLOG_INFO("Native GPU: integer vertex input variant VS={:016X} spec={} mask={:04X}",
              hash, spec & 1u, mask);
  auto* result = shader.get();
  g_shaders.emplace(key, std::move(shader));
  return result;
}

bool HasIntegerInputShader(uint64_t hash, uint32_t spec, const VertexDeclarationView& declaration) {
  return HasIntegerInputShader(hash, spec,
      IntegerInputMask({declaration.elements, declaration.element_count}));
}

IntegerInputSelection SelectIntegerInputShader(uint64_t hash, uint32_t spec,
                                               const VertexDeclarationView& declaration) {
  thread_local std::array<plume::RenderInputElement, 64> elements;
  if (!declaration.elements || declaration.element_count > elements.size()) return {};
  if (!Enabled()) {
    // Inventory even with the conversion off: every shader that receives
    // integer bytes on a float4 input, so the next bank can cover all worlds.
    const uint32_t raw = RawIntegerInputMask({declaration.elements, declaration.element_count});
    if (raw) {
      std::lock_guard lock(g_mutex);
      if (g_reported.size() < 256 && g_reported.emplace(hash, raw).second)
        REXLOG_WARN("Native GPU: integer vertex inputs on float shader inputs VS={:016X} spec={} "
                    "mask={:04X} (conversion disabled)", hash, spec & 1u, raw);
    }
    return {};
  }
  const uint32_t mask = IntegerInputMask({declaration.elements, declaration.element_count});
  auto* shader = ResolveIntegerInputShader(hash, spec, mask);
  if (!shader) return {};
  // The variant reads int4: signed BYTE4 elements must sign-extend there.
  for (uint32_t e = 0; e < declaration.element_count; ++e) {
    elements[e] = declaration.elements[e];
    if (((declaration.signed_integer_elements >> e) & 1u) &&
        elements[e].format == plume::RenderFormat::R8G8B8A8_UINT &&
        IntegerInputMask({&elements[e], 1}))
      elements[e].format = plume::RenderFormat::R8G8B8A8_SINT;
  }
  {
    std::lock_guard lock(g_mutex);
    static std::set<std::pair<uint64_t, uint64_t>> logged;
    if (logged.size() < 32 && logged.emplace(hash, declaration.content_hash).second) {
      std::string types;
      for (uint32_t e = 0; e < declaration.element_count && declaration.guest_types; ++e) {
        char item[64];
        std::snprintf(item, sizeof(item), " %s%u=%08X", declaration.elements[e].semanticName,
                      declaration.elements[e].semanticIndex, declaration.guest_types[e]);
        types += item;
      }
      REXLOG_INFO("Native GPU: integer input pipeline VS={:016X} mask={:04X} signed={:X} types:{}",
                  hash, mask, declaration.signed_integer_elements, types);
    }
  }
  return {shader, elements.data()};
}

void ResetIntegerInputShaders() {
  std::lock_guard lock(g_mutex);
  g_shaders.clear();
}
}  // namespace legodimensions::gpu_native
