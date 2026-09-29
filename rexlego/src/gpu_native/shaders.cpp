#include "gpu_native/shaders.h"

#include <array>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

#include <plume_render_interface.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include "gpu_native/d3d.h"
#include "gpu_native/device.h"
#include "gpu_native/shader_archive.h"

namespace legodimensions::gpu_native {
namespace {

constexpr u32 kPixelShaderGuestSize = 40;
constexpr u32 kVertexShaderGuestSize = 872;
constexpr u32 kShaderFlag = 0x00100000;

struct ShaderResource {
  u32 guest_address = 0;
  ShaderStage stage = ShaderStage::kVertex;
  u64 hash = 0;
  const ShaderCacheEntry* cache_entry = nullptr;
  std::mutex shader_mutex;
  std::unordered_map<u32, std::unique_ptr<plume::RenderShader>> variants;
};

std::mutex g_registry_mutex;
std::unordered_map<u32, std::shared_ptr<ShaderResource>> g_registry;
std::array<std::shared_ptr<ShaderResource>, 2> g_bound_shaders;

size_t StageIndex(ShaderStage stage) {
  return stage == ShaderStage::kVertex ? 0 : 1;
}

std::shared_ptr<ShaderResource> FindResource(u32 guest_address) {
  std::lock_guard lock(g_registry_mutex);
  const auto it = g_registry.find(guest_address);
  return it == g_registry.end() ? nullptr : it->second;
}

D3DResource* GuestHeader(u32 guest_address) {
  auto* memory = REX_KERNEL_MEMORY();
  return reinterpret_cast<D3DResource*>(memory->virtual_membase() + guest_address);
}

}  // namespace

u32 CreateShaderResource(mapped_u32 function, ShaderStage stage) {
  if (!function || !HostDevice::IsReady()) {
    return 0;
  }

  const auto* container = reinterpret_cast<const ShaderContainer*>(function.host_address());
  const size_t byte_length = ShaderContainerByteLength(container);
  // Corrupt sizes must never turn a guest pointer into an unbounded host read.
  if (byte_length < sizeof(ShaderContainer) || byte_length > 4 * 1024 * 1024) {
    REXLOG_ERROR("Native GPU: rejected {} shader container of {} bytes",
                 stage == ShaderStage::kVertex ? "vertex" : "pixel", byte_length);
    return 0;
  }

  const u64 hash = HashShaderContainer(container);
  const ShaderCacheEntry* cache_entry = FindShader(hash);
  if (!cache_entry) {
    REXLOG_WARN("Native GPU: shader not present in AOT cache, hash=0x{:016X}", hash);
  }

  auto* memory = REX_KERNEL_MEMORY();
  const u32 allocation_size =
      stage == ShaderStage::kVertex ? kVertexShaderGuestSize : kPixelShaderGuestSize;
  const u32 guest_address = memory->SystemHeapAlloc(allocation_size, 0x10);
  if (!guest_address) {
    return 0;
  }
  memory->Zero(guest_address, allocation_size);

  auto* header = GuestHeader(guest_address);
  header->common = kShaderFlag | static_cast<u32>(stage);
  header->reference_count = 1;
  header->base_flush = 0xFFFF0000u;

  // Preserve the two SDK-private backing-pointer fields observed in TU23.
  auto* bytes = memory->virtual_membase() + guest_address;
  if (stage == ShaderStage::kVertex) {
    *reinterpret_cast<be_u32*>(bytes + 0x20) = function.guest_address();
  } else {
    *reinterpret_cast<be_u32*>(bytes + 0x18) = function.guest_address();
  }

  auto resource = std::make_shared<ShaderResource>();
  resource->guest_address = guest_address;
  resource->stage = stage;
  resource->hash = hash;
  resource->cache_entry = cache_entry;
  {
    std::lock_guard lock(g_registry_mutex);
    g_registry.emplace(guest_address, std::move(resource));
  }
  return guest_address;
}

bool BindShader(ShaderStage stage, u32 guest_address) {
  std::shared_ptr<ShaderResource> resource;
  if (guest_address) {
    resource = FindResource(guest_address);
    if (!resource || resource->stage != stage) {
      REXLOG_WARN("Native GPU: unrecognized {} shader resource 0x{:08X}",
                  stage == ShaderStage::kVertex ? "vertex" : "pixel", guest_address);
      return false;
    }
  }
  std::lock_guard lock(g_registry_mutex);
  g_bound_shaders[StageIndex(stage)] = std::move(resource);
  return true;
}

plume::RenderShader* ResolveBoundShader(ShaderStage stage, u32 spec_constants) {
  std::shared_ptr<ShaderResource> resource;
  {
    std::lock_guard lock(g_registry_mutex);
    resource = g_bound_shaders[StageIndex(stage)];
  }
  if (!resource || !resource->cache_entry) {
    return nullptr;
  }

  const u32 masked = spec_constants & resource->cache_entry->specConstantsMask;
  std::lock_guard shader_lock(resource->shader_mutex);
  const auto existing = resource->variants.find(masked);
  if (existing != resource->variants.end()) {
    return existing->second.get();
  }

  const ShaderBytecode bytecode = FindDxil(resource->hash, masked);
  auto* device = HostDevice::Device();
  if (!bytecode || !device) {
    return nullptr;
  }
  auto shader = device->createShader(bytecode.data, bytecode.size, "main",
                                     plume::RenderShaderFormat::DXIL);
  if (!shader) {
    REXLOG_ERROR("Native GPU: D3D12 shader creation failed, hash=0x{:016X}",
                 resource->hash);
    return nullptr;
  }
  shader->setName(stage == ShaderStage::kVertex ? "LEGO guest vertex shader"
                                                : "LEGO guest pixel shader");
  auto [it, inserted] = resource->variants.emplace(masked, std::move(shader));
  return it->second.get();
}

u32 BoundShaderAddress(ShaderStage stage) {
  std::lock_guard lock(g_registry_mutex);
  const auto& resource = g_bound_shaders[StageIndex(stage)];
  return resource ? resource->guest_address : 0;
}

bool IsNativeShader(u32 guest_address) {
  std::lock_guard lock(g_registry_mutex);
  return g_registry.contains(guest_address);
}

u32 AddRefNativeShader(u32 guest_address) {
  std::lock_guard lock(g_registry_mutex);
  if (!g_registry.contains(guest_address)) {
    return 0;
  }
  auto* header = GuestHeader(guest_address);
  const u32 count = static_cast<u32>(header->reference_count) + 1;
  header->reference_count = count;
  return count;
}

u32 ReleaseNativeShader(u32 guest_address) {
  std::shared_ptr<ShaderResource> released;
  {
    std::lock_guard lock(g_registry_mutex);
    const auto it = g_registry.find(guest_address);
    if (it == g_registry.end()) {
      return ~u32{0};
    }
    auto* header = GuestHeader(guest_address);
    const u32 old_count = static_cast<u32>(header->reference_count);
    const u32 count = old_count > 0 ? old_count - 1 : 0;
    header->reference_count = count;
    if (count != 0) {
      return count;
    }
    released = std::move(it->second);
    g_registry.erase(it);
  }
  HostDevice::RetireResource(released);
  REX_KERNEL_MEMORY()->SystemHeapFree(guest_address);
  return 0;
}

void ResetShaderResources() {
  std::vector<u32> guest_addresses;
  {
    std::lock_guard lock(g_registry_mutex);
    g_bound_shaders = {};
    guest_addresses.reserve(g_registry.size());
    for (const auto& [guest_address, resource] : g_registry) {
      guest_addresses.push_back(guest_address);
    }
    g_registry.clear();
  }
  for (const u32 guest_address : guest_addresses) {
    REX_KERNEL_MEMORY()->SystemHeapFree(guest_address);
  }
}

}  // namespace legodimensions::gpu_native
