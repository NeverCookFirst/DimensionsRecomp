#include "gpu_native/float_blend_indices.h"
#include "gpu_native/device.h"
#include <algorithm>
#include <map>
#include <memory>
#include <mutex>
#include <utility>
#include <tuple>

namespace legodimensions::gpu_native {
namespace {
// kind 0: normalized material indices; kind 1: verified position precision.
struct FloatBlendIndicesEntry { uint64_t hash; uint32_t spec, offset, size, kind; };
#include LEGO_FLOAT_BLEND_INDICES_METADATA
extern "C" const unsigned char g_floatBlendIndicesBlob[];
using Key = std::tuple<uint64_t, uint32_t, uint32_t>;
std::map<Key, std::unique_ptr<plume::RenderShader>> g_shaders;
std::mutex g_shaders_mutex;
const FloatBlendIndicesEntry* Find(Key key) {
  const auto found = std::lower_bound(std::begin(g_float_blend_indices_entries),
      std::end(g_float_blend_indices_entries), key,
      [](const auto& entry, const auto& value) { return Key{entry.hash, entry.spec, entry.kind} < value; });
  return found != std::end(g_float_blend_indices_entries) &&
      Key{found->hash, found->spec, found->kind} == key ? found : nullptr;
}
}
ShaderBytecode FindFloatBlendIndicesDxil(uint64_t hash, uint32_t spec) {
  const auto* entry = Find({hash, spec & 1u, 0});
  return entry ? ShaderBytecode{g_floatBlendIndicesBlob + entry->offset, entry->size} : ShaderBytecode{};
}
ShaderBytecode FindPositionPrecisionDxil(uint64_t hash, uint32_t spec) {
  const auto* entry = Find({hash, spec & 1u, 1});
  return entry ? ShaderBytecode{g_floatBlendIndicesBlob + entry->offset, entry->size} : ShaderBytecode{};
}
namespace {
plume::RenderShader* ResolveVariant(uint64_t hash, uint32_t spec, uint32_t kind) {
  // Independent PSO preparation workers can resolve the same variant or insert
  // different variants concurrently. The per-PSO gates do not protect this map.
  std::lock_guard lock(g_shaders_mutex);
  const Key key{hash, spec & 1u, kind};
  if (const auto found = g_shaders.find(key); found != g_shaders.end()) return found->second.get();
  const auto code = kind ? FindPositionPrecisionDxil(hash, spec) : FindFloatBlendIndicesDxil(hash, spec);
  auto* device = HostDevice::Device();
  if (!code || !device) return nullptr;
  auto shader = device->createShader(code.data, code.size, "main", plume::RenderShaderFormat::DXIL);
  if (!shader) return nullptr;
  auto* result = shader.get();
  g_shaders.emplace(key, std::move(shader));
  return result;
}
}
plume::RenderShader* ResolveFloatBlendIndicesShader(uint64_t hash, uint32_t spec) {
  return ResolveVariant(hash, spec, 0);
}
plume::RenderShader* ResolvePositionPrecisionShader(uint64_t hash, uint32_t spec) {
  return ResolveVariant(hash, spec, 1);
}
// Called with recording serialized, before destroying the host device.
void ResetFloatBlendIndicesShaders() {
  std::lock_guard lock(g_shaders_mutex);
  g_shaders.clear();
}
} // namespace legodimensions::gpu_native
