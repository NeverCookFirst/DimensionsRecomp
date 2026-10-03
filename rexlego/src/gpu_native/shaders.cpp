#include "gpu_native/shaders.h"
#include "gpu_native/long_probe.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <plume_render_interface.h>
#include <rex/logging.h>
#include <rex/cvar.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#include <xxhash.h>

#include "gpu_native/d3d.h"
#include "gpu_native/device.h"
#include "gpu_native/shader_archive.h"
#include "gpu_native/pixel_shader_filter.h"
#include "cheat_menu.h"

namespace legodimensions::gpu_native {
namespace {

constexpr u32 kPixelShaderGuestSize = 40;
constexpr u32 kVertexShaderGuestSize = 872;
constexpr u32 kShaderFlag = 0x00100000;

struct ShaderResource {
  u32 guest_address = 0;
  ShaderStage stage = ShaderStage::kVertex;
  u64 hash = 0;
  u32 texture_mask = ~u32{0};
  const ShaderCacheEntry* cache_entry = nullptr;
  std::mutex shader_mutex;
  std::unordered_map<u32, std::unique_ptr<plume::RenderShader>> variants;
  bool owns_guest_memory = true;
  u32 physical_code_address = 0;
  u32 physical_code_size = 0;
};

std::mutex g_registry_mutex;
std::unordered_map<u32, std::shared_ptr<ShaderResource>> g_registry;
std::array<std::shared_ptr<ShaderResource>, 2> g_bound_shaders;
std::atomic<u32> g_shader_warning_count{0};

size_t StageIndex(ShaderStage stage) {
  return stage == ShaderStage::kVertex ? 0 : 1;
}

std::shared_ptr<ShaderResource> FindResource(u32 guest_address) {
  std::lock_guard lock(g_registry_mutex);
  const auto it = g_registry.find(guest_address);
  return it == g_registry.end() ? nullptr : it->second;
}

D3DResource* GuestHeader(u32 guest_address) {
  return REX_KERNEL_MEMORY()->TranslateVirtual<D3DResource*>(guest_address);
}

void DumpMissingShader(const ShaderContainer* container, size_t byte_length,
                       ShaderStage stage, u64 hash, const char* category = "");

bool ReadableGuestRange(u32 address, u32 size) {
  if (!size || uint64_t(address) + size > 0x100000000ull) return false;
  auto* memory = REX_KERNEL_MEMORY();
  for (uint64_t at = address; at < uint64_t(address) + size;) {
    auto* heap = memory->LookupHeap(static_cast<u32>(at));
    rex::memory::HeapAllocationInfo info{};
    if (!heap || !heap->QueryRegionInfo(static_cast<u32>(at), &info) ||
        !(info.state & rex::memory::kMemoryAllocationCommit) ||
        !(info.protect & rex::memory::kMemoryProtectRead)) return false;
    at = (at & ~uint64_t(4095)) + 4096;
  }
  return true;
}

std::shared_ptr<ShaderResource> AdoptShader(u32 guest_address,
                                           ShaderStage expected_stage) {
  if (const auto existing = FindResource(guest_address)) {
    return existing;
  }
  if (!guest_address) {
    return nullptr;
  }
  auto* memory = REX_KERNEL_MEMORY();
  const auto* bytes = memory->TranslateVirtual<const uint8_t*>(guest_address);
  const auto* header = reinterpret_cast<const D3DResource*>(bytes);
  const u32 common = header->common;
  // Game-owned XDK shaders use the bare resource type (6/7), while resources
  // allocated by our create hooks also carry kShaderFlag.
  if ((common & 0xFu) != static_cast<u32>(expected_stage)) {
    if (g_shader_warning_count.fetch_add(1, std::memory_order_relaxed) < 16) {
      const auto* words = reinterpret_cast<const be_u32*>(bytes);
      REXLOG_WARN(
          "Native GPU: cannot adopt {} shader 0x{:08X}, header "
          "{:08X} {:08X} {:08X} {:08X} {:08X} {:08X} {:08X} {:08X} "
          "{:08X} {:08X}",
          expected_stage == ShaderStage::kVertex ? "vertex" : "pixel",
          guest_address, static_cast<u32>(words[0]),
          static_cast<u32>(words[1]), static_cast<u32>(words[2]),
          static_cast<u32>(words[3]), static_cast<u32>(words[4]),
          static_cast<u32>(words[5]), static_cast<u32>(words[6]),
          static_cast<u32>(words[7]), static_cast<u32>(words[8]),
          static_cast<u32>(words[9]));
    }
    return nullptr;
  }
  const size_t pointer_offset =
      expected_stage == ShaderStage::kVertex ? 0x20 : 0x18;
  const u32 container_address =
      *reinterpret_cast<const be_u32*>(bytes + pointer_offset);
  if (!container_address) {
    return nullptr;
  }
  // XDK placement objects retain the copied physical microcode, not the
  // original ShaderContainer. Associate it with the independently indexed
  // AOT entry before creating any host object.
  const auto* microcode =
      memory->TranslateVirtual<const uint8_t*>(container_address);
  const ShaderCacheEntry* cache_entry = FindShaderByMicrocode(
      microcode, expected_stage == ShaderStage::kPixel ? 1u : 0u);
  if (!cache_entry) {
    // Diagnostic only, opt-in. Bound distinct objects rather than bind calls:
    // repeated draws must not exhaust the capture budget on the first pair.
    static const bool capture_enabled = [] {
      char* value = nullptr;
      size_t length = 0;
      _dupenv_s(&value, &length, "LEGO_DUMP_MISSING_SHADERS");
      const bool enabled = value && length > 1;
      std::free(value);
      return enabled;
    }();
    static std::mutex capture_mutex;
    static std::unordered_set<u32> captured_objects;
    bool capture_object = false;
    if (capture_enabled) {
      std::lock_guard lock(capture_mutex);
      capture_object = captured_objects.size() < 256 &&
                       captured_objects.insert(guest_address).second;
    }
    if (capture_object) {
      DumpMissingShader(reinterpret_cast<const ShaderContainer*>(microcode),
          std::min<size_t>(2048, 4096 - (container_address & 4095)),
          expected_stage, XXH3_64bits(microcode, 8), "placement-raw");
      // XGSet*ShaderHeader copies the virtual section immediately after the
      // SDK object (TU23 83FB7618 / 83FB74F0). Validate both ranges before
      // reconstructing a container from the separately allocated sections.
      const u32 header_address = guest_address +
          (expected_stage == ShaderStage::kVertex ? kVertexShaderGuestSize
                                                  : kPixelShaderGuestSize);
      const auto* virtual_header =
          memory->TranslateVirtual<const ShaderContainer*>(header_address);
      if (ReadableGuestRange(header_address, sizeof(ShaderContainer))) {
        const u32 virtual_size = virtual_header->virtual_size;
        const u32 physical_size = virtual_header->physical_size;
        if ((u32(virtual_header->flags) & 0xFFFFFFFEu) == 0x102A1100u &&
            virtual_size >= sizeof(ShaderContainer) && virtual_size <= 65536 &&
            physical_size >= 8 && physical_size <= 65536 &&
            ReadableGuestRange(header_address, virtual_size) &&
            ReadableGuestRange(container_address, physical_size)) {
          // Preserve the exact two sections, with an explicit archive marker.
          // This is a capture, not a substitute shader or a guessed binding.
          std::vector<uint8_t> capture(virtual_size + 4 + physical_size);
          std::memcpy(capture.data(), virtual_header, virtual_size);
          std::memcpy(capture.data() + virtual_size,
                      reinterpret_cast<const uint8_t*>(virtual_header) + 8, 4);
          std::memcpy(capture.data() + virtual_size + 4, microcode, physical_size);
          DumpMissingShader(reinterpret_cast<const ShaderContainer*>(capture.data()),
              capture.size(), expected_stage,
              XXH3_64bits(capture.data(), capture.size()), "placement-containers");
        }
      }
    }
    if (g_shader_warning_count.fetch_add(1, std::memory_order_relaxed) < 16) {
      const auto* words = reinterpret_cast<const be_u32*>(microcode);
      REXLOG_WARN("Native GPU: placement {} shader 0x{:08X} backing "
                  "0x{:08X} prefix=0x{:016X} words={:08X} {:08X} "
                  "{:08X} {:08X} missing from AOT index",
                  expected_stage == ShaderStage::kVertex ? "vertex" : "pixel",
                  guest_address, container_address, XXH3_64bits(microcode, 8),
                  static_cast<u32>(words[0]), static_cast<u32>(words[1]),
                  static_cast<u32>(words[2]), static_cast<u32>(words[3]));
    }
    return nullptr;
  }
  auto resource = std::make_shared<ShaderResource>();
  resource->guest_address = guest_address;
  resource->stage = expected_stage;
  resource->hash = cache_entry->hash;
  resource->texture_mask = FindShaderTextureMask(resource->hash);
  resource->cache_entry = cache_entry;
  resource->owns_guest_memory = false;
  resource->physical_code_address = container_address;
  for (size_t i = 0; i < g_shaderMicrocodeEntryCount; ++i) {
    const auto& entry = g_shaderMicrocodeEntries[i];
    if (entry.containerHash == resource->hash &&
        entry.stage == (expected_stage == ShaderStage::kPixel ? 1u : 0u))
      resource->physical_code_size = std::max(resource->physical_code_size, entry.microcodeSize);
  }
  {
    std::lock_guard lock(g_registry_mutex);
    const auto [it, inserted] = g_registry.emplace(guest_address, resource);
    if (!inserted) {
      return it->second;
    }
  }
  REXLOG_INFO("Native GPU: adopted {} shader 0x{:08X}, hash=0x{:016X}",
              expected_stage == ShaderStage::kVertex ? "vertex" : "pixel",
              guest_address, resource->hash);
  return resource;
}

void DumpMissingShader(const ShaderContainer* container, size_t byte_length,
                       ShaderStage stage, u64 hash, const char* category) {
  char* root_buffer = nullptr;
  size_t root_length = 0;
  if (_dupenv_s(&root_buffer, &root_length, "LEGO_DUMP_MISSING_SHADERS") != 0 ||
      !root_buffer || root_length <= 1) {
    std::free(root_buffer);
    return;
  }
  std::unique_ptr<char, decltype(&std::free)> root_owner(root_buffer,
                                                          &std::free);
  const char* root = root_owner.get();
  std::error_code error;
  const std::filesystem::path directory = std::filesystem::path(root) / category;
  std::filesystem::create_directories(directory, error);
  if (error) {
    REXLOG_WARN("Native GPU: cannot create shader dump directory '{}': {}",
                root, error.message());
    return;
  }
  char name[48];
  std::snprintf(name, sizeof(name), "%s-%016llX.bin",
                stage == ShaderStage::kVertex ? "vs" : "ps",
                static_cast<unsigned long long>(hash));
  const auto path = directory / name;
  if (std::filesystem::exists(path, error)) {
    return;
  }
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output.write(reinterpret_cast<const char*>(container),
               static_cast<std::streamsize>(byte_length));
  if (!output) {
    REXLOG_WARN("Native GPU: failed to dump missing shader '{}'", path.string());
  } else {
    REXLOG_INFO("Native GPU: dumped missing shader '{}'", path.string());
  }
}

}  // namespace

bool ShouldSkipBoundPixelShader() {
  // Called under the recording lock; rebuild only when the user changes it.
  static std::string previous;
  static std::unordered_set<u64> containers;
  auto list = rex::cvar::GetFlagByName("skip_pixel_shaders");
  // Native mode may not link the Xenos command processor that registers its
  // skip list cvar. The game's Graphics toggle itself is always registered.
  if (rex::cvar::GetFlagByName("depth_of_field") == "false") {
    if (!list.empty()) list += ',';
    list += cheats::kDofPixelShaderHash;
  }
  if (list != previous) {
    previous = list;
    containers.clear();
    const auto hashes = ParsePixelShaderFilter(list);
    for (size_t i = 0; i < g_shaderMicrocodeEntryCount; ++i) {
      const auto& entry = g_shaderMicrocodeEntries[i];
      if (entry.stage == 1 && hashes.contains(entry.microcodeHash))
        containers.insert(entry.containerHash);
    }
    REXLOG_INFO("Native GPU: pixel shader filter matches {} AOT containers", containers.size());
  }
  return containers.contains(BoundShaderHash(ShaderStage::kPixel));
}

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
    DumpMissingShader(container, byte_length, stage, hash);
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
  auto* bytes = memory->TranslateVirtual<uint8_t*>(guest_address);
  if (stage == ShaderStage::kVertex) {
    *reinterpret_cast<be_u32*>(bytes + 0x20) = function.guest_address();
  } else {
    *reinterpret_cast<be_u32*>(bytes + 0x18) = function.guest_address();
  }

  auto resource = std::make_shared<ShaderResource>();
  resource->guest_address = guest_address;
  resource->stage = stage;
  resource->hash = hash;
  resource->texture_mask = FindShaderTextureMask(hash);
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
    resource = AdoptShader(guest_address, stage);
    if (!resource || resource->stage != stage) {
      LongProbeEvent("unrecognized_shader", true, "guest=", guest_address,
          "stage=", static_cast<u32>(stage));
      {
        std::lock_guard lock(g_registry_mutex);
        g_bound_shaders[StageIndex(stage)].reset();
      }
      if (g_shader_warning_count.fetch_add(1, std::memory_order_relaxed) < 32) {
        REXLOG_WARN("Native GPU: unrecognized {} shader resource 0x{:08X}",
                    stage == ShaderStage::kVertex ? "vertex" : "pixel",
                    guest_address);
      }
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

void InvalidatePlacementShader(u32 guest_address) {
  auto recording = HostDevice::LockRecording();
  std::shared_ptr<ShaderResource> previous;
  {
    std::lock_guard lock(g_registry_mutex);
    const auto it = g_registry.find(guest_address);
    if (it == g_registry.end() || it->second->owns_guest_memory) return;
    previous = std::move(it->second);
    g_registry.erase(it);
  }
  static std::atomic<u32> logs{0};
  if (logs.fetch_add(1) < 16)
    REXLOG_INFO("Native GPU: re-registered placement shader {:08X}, previous hash={:016X}",
                guest_address, previous->hash);
  HostDevice::RetireResource(std::move(previous));
}

u32 InvalidatePoolCopyShaders(PhysicalCopyRange destination) {
  auto recording = HostDevice::LockRecording();
  std::vector<std::shared_ptr<ShaderResource>> retired;
  std::array<u32, 2> rebind{};
  {
    std::lock_guard lock(g_registry_mutex);
    for (auto it = g_registry.begin(); it != g_registry.end();) {
      const auto& resource = it->second;
      const u32 header_size = resource->stage == ShaderStage::kVertex ?
          kVertexShaderGuestSize : kPixelShaderGuestSize;
      if (!resource->owns_guest_memory &&
          (GuestRangeOverlapsPoolCopy(resource->physical_code_address, resource->physical_code_size, destination) ||
           GuestRangeOverlapsPoolCopy(resource->guest_address, header_size, destination))) {
        const size_t stage = StageIndex(resource->stage);
        if (g_bound_shaders[stage] == resource) rebind[stage] = resource->guest_address;
        retired.push_back(std::move(it->second));
        it = g_registry.erase(it);
      } else ++it;
    }
  }
  const u32 count = u32(retired.size());
  for (auto& resource : retired) HostDevice::RetireResource(std::move(resource));
  if (rebind[0]) BindShader(ShaderStage::kVertex, rebind[0]);
  if (rebind[1]) BindShader(ShaderStage::kPixel, rebind[1]);
  return count;
}

u32 BoundShaderTextureMask(ShaderStage stage) {
  std::lock_guard lock(g_registry_mutex);
  const auto& resource = g_bound_shaders[StageIndex(stage)];
  return resource ? resource->texture_mask : 0;
}

u64 BoundShaderHash(ShaderStage stage) {
  std::lock_guard lock(g_registry_mutex);
  const auto& resource = g_bound_shaders[StageIndex(stage)];
  return resource ? resource->hash : 0;
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
  auto recording = HostDevice::LockRecording();
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
  if (released->owns_guest_memory) {
    REX_KERNEL_MEMORY()->SystemHeapFree(guest_address);
  }
  return 0;
}

void ResetShaderResources() {
  std::vector<u32> guest_addresses;
  {
    std::lock_guard lock(g_registry_mutex);
    g_bound_shaders = {};
    guest_addresses.reserve(g_registry.size());
    for (const auto& [guest_address, resource] : g_registry) {
      if (resource->owns_guest_memory) {
        guest_addresses.push_back(guest_address);
      }
    }
    g_registry.clear();
  }
  for (const u32 guest_address : guest_addresses) {
    REX_KERNEL_MEMORY()->SystemHeapFree(guest_address);
  }
}

}  // namespace legodimensions::gpu_native
