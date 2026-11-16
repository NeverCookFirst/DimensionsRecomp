#include "gpu_native/instanced_constants.h"
#include "gpu_native/device.h"
#include <algorithm>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <utility>

namespace legodimensions::gpu_native {
namespace {
using Key = std::pair<uint64_t, uint32_t>;
#ifdef LEGO_INSTANCED_CONSTANTS_METADATA
struct InstancedConstantsEntry { uint64_t hash; uint32_t spec, offset, size; };
#include LEGO_INSTANCED_CONSTANTS_METADATA
extern "C" const unsigned char g_instancedConstantsBlob[];
ShaderBytecode Find(Key key) {
  const auto found = std::lower_bound(std::begin(g_instanced_constants_entries),
      std::end(g_instanced_constants_entries), key,
      [](const auto& entry, const auto& value) { return Key{entry.hash, entry.spec} < value; });
  return found != std::end(g_instanced_constants_entries) && Key{found->hash, found->spec} == key
      ? ShaderBytecode{g_instancedConstantsBlob + found->offset, found->size} : ShaderBytecode{};
}
#else
// Without a generated bank every lookup misses and draws stay non-instanced.
ShaderBytecode Find(Key) { return {}; }
#endif
std::mutex g_mutex;
std::map<Key, std::unique_ptr<plume::RenderShader>> g_shaders;
}
// VS specialization only uses SPEC_CONSTANT_R11G11B10_NORMAL; the bank rejects wider masks.
ShaderBytecode FindInstancedConstantsDxil(uint64_t hash, uint32_t spec) {
  return Find({hash, spec & 1u});
}
plume::RenderShader* ResolveInstancedConstantsShader(uint64_t hash, uint32_t spec) {
  const Key key{hash, spec & 1u};
  std::lock_guard lock(g_mutex);
  if (const auto found = g_shaders.find(key); found != g_shaders.end()) return found->second.get();
  const auto code = Find(key);
  auto* device = HostDevice::Device();
  if (!code || !device) return nullptr;
  auto shader = device->createShader(code.data, code.size, "main", plume::RenderShaderFormat::DXIL);
  if (!shader) return nullptr;
  auto* result = shader.get();
  g_shaders.emplace(key, std::move(shader));
  return result;
}
// Called with recording serialized, before destroying the host device.
void ResetInstancedConstantsShaders() {
  std::lock_guard lock(g_mutex);
  g_shaders.clear();
}
} // namespace legodimensions::gpu_native
