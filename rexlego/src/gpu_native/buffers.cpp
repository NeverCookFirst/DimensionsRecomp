#include "gpu_native/buffers.h"
#include "gpu_native/buffer_header.h"
#include "gpu_native/memory_watch.h"

#include <cstring>
#include <cstdlib>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

#include <plume_render_interface.h>
// Use the game's configured ISA for bulk hashing (AVX2 on x86-64-v3),
// retaining XXH3's exact hash and every-use content validation.
#define XXH_INLINE_ALL
#include <xxhash.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include "gpu_native/d3d.h"
#include "gpu_native/device.h"
#include "gpu_native/textures.h"

namespace legodimensions::gpu_native {
namespace {

struct BufferResource {
  u32 guest_address = 0;
  u32 mirror_address = 0;
  u32 length = 0;
  u32 format = 0;
  BufferKind kind = BufferKind::kVertex;
  bool owns_guest_memory = true;
  std::mutex upload_mutex;
  std::shared_ptr<plume::RenderBuffer> buffer;
  u64 content_hash = 0;
  u64 byte_order_hash = 0;
  CpuMemoryStamp cpu_stamp;
  // Immutable conversions of the SAME contents for alternating declarations.
  std::unordered_map<u64, std::shared_ptr<plume::RenderBuffer>> variants;
  std::unordered_map<u64, std::shared_ptr<BufferResource>> windows;
  u64 window_cache_bytes = 0;
  u64 window_use_serial = 0;
  u64 last_window_use = 0;
};

std::mutex g_buffers_mutex;
std::unordered_map<u32, std::shared_ptr<BufferResource>> g_buffers;
BufferUploadTiming g_buffer_timing;  // Serialized by LockRecording.

D3DBuffer* GuestBuffer(u32 guest_address) {
  auto* memory = REX_KERNEL_MEMORY();
  return memory->TranslateVirtual<D3DBuffer*>(guest_address);
}

std::shared_ptr<BufferResource> FindBuffer(u32 guest_address) {
  std::lock_guard lock(g_buffers_mutex);
  const auto it = g_buffers.find(guest_address);
  return it == g_buffers.end() ? nullptr : it->second;
}

std::shared_ptr<BufferResource> AdoptBuffer(u32 address, BufferKind kind) {
  if (!address || !HostDevice::IsReady()) return nullptr;
  auto existing = FindBuffer(address);
  if (existing && existing->owns_guest_memory) return existing;
  const auto* header = GuestBuffer(address);
  const auto info = DecodePlacementBuffer(header->resource.common,
      header->fetch_lo, header->fetch_hi, kind == BufferKind::kIndex);
  if (!info.address) return nullptr;
  if (existing && existing->kind == kind && existing->mirror_address == info.address &&
      existing->length == info.size && existing->format == info.index_format)
    return existing;
  auto resource = std::make_shared<BufferResource>();
  resource->guest_address = address;
  resource->mirror_address = info.address;
  resource->length = info.size;
  resource->format = info.index_format;
  resource->kind = kind;
  resource->owns_guest_memory = false;
  {
    std::lock_guard lock(g_buffers_mutex);
    auto& entry = g_buffers[address];
    if (entry != existing) return entry;  // Another caller adopted/replaced it.
    entry = resource;
  }
  if (existing) HostDevice::RetireResource(std::move(existing));
  static std::atomic<u32> adopted_logs{0};
  if (adopted_logs.fetch_add(1) < 32)
    REXLOG_INFO("Native GPU: adopted {} buffer 0x{:08X} data=0x{:08X} bytes={} index_format={}",
        kind == BufferKind::kIndex ? "index" : "vertex", address,
        info.address, info.size, info.index_format);
  return resource;
}

void ByteSwapElements(void* destination, const void* source, size_t size,
                      u32 element_size) {
  auto* dst = static_cast<u8*>(destination);
  const auto* src = static_cast<const u8*>(source);
  size_t offset = 0;
  if (element_size == 2) {
    for (; offset + 2 <= size; offset += 2) {
      dst[offset + 0] = src[offset + 1];
      dst[offset + 1] = src[offset + 0];
    }
  } else {
    for (; offset + 4 <= size; offset += 4) {
      dst[offset + 0] = src[offset + 3];
      dst[offset + 1] = src[offset + 2];
      dst[offset + 2] = src[offset + 1];
      dst[offset + 3] = src[offset + 0];
    }
  }
  if (offset != size) {
    std::memcpy(dst + offset, src + offset, size - offset);
  }
}

}  // namespace

BufferUploadTiming ConsumeBufferUploadTiming() {
  auto recording = HostDevice::LockRecording();
  return std::exchange(g_buffer_timing, {});
}

u32 InvalidatePoolCopyBuffers(PhysicalCopyRange destination) {
  auto recording = HostDevice::LockRecording();
  std::vector<std::shared_ptr<BufferResource>> retired;
  {
    std::lock_guard lock(g_buffers_mutex);
    for (auto it = g_buffers.begin(); it != g_buffers.end();) {
      const auto& resource = it->second;
      if (!resource->owns_guest_memory &&
          (GuestRangeOverlapsPoolCopy(resource->mirror_address, resource->length, destination) ||
           GuestRangeOverlapsPoolCopy(resource->guest_address, sizeof(D3DBuffer), destination))) {
        static std::atomic<u32> logs{0};
        if (logs.fetch_add(1, std::memory_order_relaxed) < 32)
          REXLOG_INFO("Native GPU: pool copy invalidates {} buffer {:08X} data={:08X} bytes={}",
              resource->kind == BufferKind::kIndex ? "index" : "vertex",
              resource->guest_address, resource->mirror_address, resource->length);
        retired.push_back(std::move(it->second));
        it = g_buffers.erase(it);
      } else ++it;
    }
  }
  const u32 count = u32(retired.size());
  for (auto& resource : retired) HostDevice::RetireResource(std::move(resource));
  return count;
}

u32 CreateBufferResource(u32 length, u32 /*usage*/, u32 format, u32 /*pool*/,
                         BufferKind kind) {
  if (!length || !HostDevice::IsReady()) {
    return 0;
  }
  auto* memory = REX_KERNEL_MEMORY();
  const u32 guest_address = memory->SystemHeapAlloc(sizeof(D3DBuffer), 0x10);
  if (!guest_address) {
    return 0;
  }
  const u32 mirror_address = memory->SystemHeapAlloc(length, 0x20);
  if (!mirror_address) {
    memory->SystemHeapFree(guest_address);
    return 0;
  }
  memory->Zero(guest_address, sizeof(D3DBuffer));
  memory->Zero(mirror_address, length);

  auto* guest = GuestBuffer(guest_address);
  guest->resource.common = static_cast<u32>(kind);
  guest->resource.reference_count = 1;
  guest->fetch_lo = mirror_address;
  guest->fetch_hi = length;

  auto resource = std::make_shared<BufferResource>();
  resource->guest_address = guest_address;
  resource->mirror_address = mirror_address;
  resource->length = length;
  resource->format = format;
  resource->kind = kind;
  {
    std::lock_guard lock(g_buffers_mutex);
    g_buffers.emplace(guest_address, std::move(resource));
  }
  return guest_address;
}

u32 LockBufferResource(u32 guest_address, u32 offset, u32 size, u32 /*flags*/,
                       BufferKind kind) {
  const auto resource = AdoptBuffer(guest_address, kind);
  if (!resource || resource->kind != kind || offset > resource->length ||
      (size != 0 && size > resource->length - offset)) {
    return 0;
  }
  return resource->mirror_address + offset;
}

plume::RenderBuffer* ResolveBufferContents(const std::shared_ptr<BufferResource>& resource,
    u32 guest_address, BufferKind kind, const VertexByteOrder& byte_order) {
  if (!resource || resource->kind != kind) {
    static std::atomic<u32> missing_logs{0};
    if (guest_address && missing_logs.fetch_add(1) < 8) {
      const auto* words = REX_KERNEL_MEMORY()->TranslateVirtual<const be_u32*>(guest_address);
      REXLOG_WARN("Native GPU: missing {} buffer 0x{:08X} header={:08X} {:08X} {:08X} {:08X} {:08X} {:08X} {:08X} {:08X}",
          static_cast<u32>(kind), guest_address, u32(words[0]), u32(words[1]), u32(words[2]),
          u32(words[3]), u32(words[4]), u32(words[5]), u32(words[6]), u32(words[7]));
    }
    return nullptr;
  }
  std::lock_guard upload_lock(resource->upload_mutex);
  if (!byte_order.reversed_elements.empty() &&
      (byte_order.stream_offset > resource->length || byte_order.stride < 4)) return nullptr;
  const auto conversion_order = CanonicalVertexByteOrder(byte_order);
  const u64 byte_order_hash = conversion_order.reversed_elements.empty() ? 0 :
      XXH3_64bits_withSeed(conversion_order.reversed_elements.data(),
          conversion_order.reversed_elements.size_bytes(),
          (u64(conversion_order.stream_offset) << 32) | conversion_order.stride);
  const auto* source =
      REX_KERNEL_MEMORY()->TranslateVirtual<const u8*>(resource->mirror_address);
  const bool timing = NativeTextureTimingEnabled();
  const auto hash_start = timing ? std::chrono::steady_clock::now()
                                : std::chrono::steady_clock::time_point{};
  const bool watch_enabled = CpuBufferMemoryWatchEnabled();
  const bool watch_hit = watch_enabled && !resource->variants.empty() &&
      CpuMemoryUnchanged(resource->cpu_stamp);
  static const bool audit_watch = std::getenv("LEGO_NATIVE_AUDIT_BUFFER_WATCH") != nullptr;
  CpuMemoryStamp next_stamp;
  u64 content_hash = resource->content_hash;
  if (!watch_hit || audit_watch) {
    // Arm before hashing/conversion. A concurrent write invalidates this
    // stamp, so a mixed snapshot cannot remain cached as permanently clean.
    if (watch_enabled) {
      const CpuMemorySpan span{resource->mirror_address, resource->length};
      next_stamp = WatchCpuMemory({&span, 1});
    }
    content_hash = XXH3_64bits(source, resource->length);
    if (timing) g_buffer_timing.hashed_bytes += resource->length;
    if (watch_hit && audit_watch) {
      if (timing) ++g_buffer_timing.watch_audits;
      if (content_hash != resource->content_hash) {
        if (timing) ++g_buffer_timing.watch_mismatches;
        REXLOG_ERROR("Native buffer watch: stale clean stamp guest={:08X} data={:08X} bytes={}",
            guest_address, resource->mirror_address, resource->length);
      }
    }
  } else if (timing) ++g_buffer_timing.watch_hits;
  if (timing) {
    ++g_buffer_timing.calls;
    g_buffer_timing.hash_ms += std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - hash_start).count();
  }
  if (resource->content_hash != content_hash) {
    for (auto& [key, version] : resource->variants) HostDevice::RetireResource(std::move(version));
    resource->variants.clear();
    resource->buffer.reset();
  } else if (const auto it = resource->variants.find(byte_order_hash); it != resource->variants.end()) {
    if (!watch_hit || audit_watch) resource->cpu_stamp = std::move(next_stamp);
    resource->buffer = it->second;
    return it->second.get();
  }
  // Never overwrite an UPLOAD buffer referenced by already recorded draws.
  // CPU changes get a new version; the previous one survives its frame fence.
  std::shared_ptr<plume::RenderBuffer> next_buffer;
  {
    auto* device = HostDevice::Device();
    if (!device) {
      return nullptr;
    }
    const auto desc = kind == BufferKind::kIndex
                          ? plume::RenderBufferDesc::IndexBuffer(
                                resource->length, plume::RenderHeapType::UPLOAD)
                          : plume::RenderBufferDesc::VertexBuffer(
                                resource->length, plume::RenderHeapType::UPLOAD);
    next_buffer = device->createBuffer(desc);
    if (!next_buffer) {
      REXLOG_ERROR("Native GPU: failed to allocate {} buffer ({} bytes)",
                   kind == BufferKind::kIndex ? "index" : "vertex",
                   resource->length);
      return nullptr;
    }
    next_buffer->setName(kind == BufferKind::kIndex ? "LEGO guest index buffer"
                                                          : "LEGO guest vertex buffer");
  }

  void* mapped = next_buffer->map();
  if (!mapped) {
    return nullptr;
  }
  const u32 element_size =
      kind == BufferKind::kIndex && resource->format == 1 ? 2 : 4;
  // UPLOAD heaps can be write-combined. Read/modify/write there (the packed
  // vertex swizzle below) is extremely slow. Transform in cacheable CPU RAM,
  // then perform a sequential write-only copy into the new GPU version.
  std::vector<u8> converted;
  void* conversion_target = mapped;
  if (kind == BufferKind::kVertex && !byte_order.reversed_elements.empty()) {
    converted.resize(resource->length);
    conversion_target = converted.data();
  }
  ByteSwapElements(conversion_target, source, resource->length, element_size);
  if (timing) g_buffer_timing.converted_bytes += resource->length;
  if (kind == BufferKind::kVertex &&
      !ApplyVertexByteOrder(static_cast<u8*>(conversion_target), resource->length, conversion_order)) {
    next_buffer->unmap();
    REXLOG_WARN("Native GPU: rejected invalid packed vertex layout stride={}",
                byte_order.stride);
    return nullptr;
  }
  if (!converted.empty()) std::memcpy(mapped, converted.data(), converted.size());
  next_buffer->unmap();
  if (resource->variants.size() >= 8) {
    auto old = resource->variants.begin();
    HostDevice::RetireResource(std::move(old->second));
    resource->variants.erase(old);
  }
  resource->variants.emplace(byte_order_hash, next_buffer);
  resource->buffer = std::move(next_buffer);
  resource->content_hash = content_hash;
  resource->byte_order_hash = byte_order_hash;
  if (!watch_hit || audit_watch) resource->cpu_stamp = std::move(next_stamp);
  return resource->buffer.get();
}

plume::RenderBuffer* ResolveBufferResource(u32 guest_address, BufferKind kind,
                                         const VertexByteOrder& byte_order) {
  return ResolveBufferContents(AdoptBuffer(guest_address, kind), guest_address, kind, byte_order);
}

BufferResourceView InspectBufferResource(u32 guest_address, BufferKind kind) {
  const auto resource = AdoptBuffer(guest_address, kind);
  if (!resource || resource->kind != kind) return {};
  return {nullptr, resource->length, resource->format, resource->mirror_address};
}

BufferResourceView ResolveBufferResourceWindow(u32 guest_address, BufferKind kind,
    u32 offset, u32 length, const VertexByteOrder& byte_order) {
  const auto parent = AdoptBuffer(guest_address, kind);
  if (!parent || parent->kind != kind || !length || offset > parent->length ||
      length > parent->length - offset) return {};
  // Keep DWORD endian conversion aligned with the original whole buffer.
  const u32 alignment = kind == BufferKind::kIndex && parent->format == 1 ? 2 : 4;
  if (offset % alignment || length % alignment) return {};
  const u64 key = (u64(offset) << 32) | length;
  std::shared_ptr<BufferResource> resource;
  {
    std::lock_guard lock(parent->upload_mutex);
    auto it = parent->windows.find(key);
    if (it != parent->windows.end()) resource = it->second;
    else {
      resource = std::make_shared<BufferResource>();
      resource->guest_address = guest_address;
      resource->mirror_address = parent->mirror_address + offset;
      resource->length = length;
      resource->format = parent->format;
      resource->kind = kind;
      resource->owns_guest_memory = false;
      // A shared mesh pool has far more than 64 draw ranges. Evicting begin()
      // repeatedly discarded newly inserted ranges and allocated GPU buffers
      // for them again on every frame. Bound bytes as well as entry count and
      // retire the least recently used range instead.
      const u64 budget = std::clamp<u64>(parent->length, 256 * 1024, 16 * 1024 * 1024);
      while (!parent->windows.empty() && (parent->windows.size() >= 4096 ||
             parent->window_cache_bytes + length > budget)) {
        auto old = std::min_element(parent->windows.begin(), parent->windows.end(),
            [](const auto& a, const auto& b) {
              return a.second->last_window_use < b.second->last_window_use;
            });
        parent->window_cache_bytes -= old->second->length;
        HostDevice::RetireResource(std::move(old->second));
        parent->windows.erase(old);
      }
      parent->window_cache_bytes += length;
      parent->windows.emplace(key, resource);
    }
    resource->last_window_use = ++parent->window_use_serial;
  }
  auto* buffer = ResolveBufferContents(resource, guest_address, kind, byte_order);
  return {buffer, resource->length, resource->format, resource->mirror_address,
          resource->content_hash, resource->byte_order_hash};
}

BufferResourceView ResolveBufferResourceView(u32 guest_address,
                                             BufferKind kind,
                                             const VertexByteOrder& byte_order) {
  auto* buffer = ResolveBufferResource(guest_address, kind, byte_order);
  const auto resource = FindBuffer(guest_address);
  if (!resource || resource->kind != kind) {
    return {};
  }
  return {buffer, resource->length, resource->format, resource->mirror_address,
          resource->content_hash, resource->byte_order_hash};
}

bool IsNativeBuffer(u32 guest_address) {
  std::lock_guard lock(g_buffers_mutex);
  return g_buffers.contains(guest_address);
}

u32 NativeBufferType(u32 guest_address) {
  const auto resource = FindBuffer(guest_address);
  // Placement headers keep the TU23 types 1/2. In particular XGOffsetResource
  // uses type 1 to update +24, whereas type 6 would update +32 instead.
  // Only our simplified, owned headers need a registry type override.
  return resource && resource->owns_guest_memory ? static_cast<u32>(resource->kind) : 0;
}

u32 AddRefNativeBuffer(u32 guest_address) {
  std::lock_guard lock(g_buffers_mutex);
  if (!g_buffers.contains(guest_address)) {
    return 0;
  }
  auto* header = &GuestBuffer(guest_address)->resource;
  const u32 count = static_cast<u32>(header->reference_count) + 1;
  header->reference_count = count;
  return count;
}

u32 ReleaseNativeBuffer(u32 guest_address) {
  auto recording = HostDevice::LockRecording();
  std::shared_ptr<BufferResource> released;
  {
    std::lock_guard lock(g_buffers_mutex);
    const auto it = g_buffers.find(guest_address);
    if (it == g_buffers.end()) {
      return ~u32{0};
    }
    auto* header = &GuestBuffer(guest_address)->resource;
    const u32 old_count = static_cast<u32>(header->reference_count);
    const u32 count = old_count > 0 ? old_count - 1 : 0;
    header->reference_count = count;
    if (count != 0) {
      return count;
    }
    released = std::move(it->second);
    g_buffers.erase(it);
  }
  auto* memory = REX_KERNEL_MEMORY();
  HostDevice::RetireResource(released);
  if (released->owns_guest_memory) {
    memory->SystemHeapFree(released->mirror_address);
    memory->SystemHeapFree(guest_address);
  }
  return 0;
}

void ResetBufferResources() {
  std::vector<std::pair<u32, u32>> allocations;
  {
    std::lock_guard lock(g_buffers_mutex);
    allocations.reserve(g_buffers.size());
    for (const auto& [guest_address, resource] : g_buffers) {
      if (resource->owns_guest_memory)
        allocations.emplace_back(guest_address, resource->mirror_address);
    }
    g_buffers.clear();
  }
  auto* memory = REX_KERNEL_MEMORY();
  for (const auto [guest_address, mirror_address] : allocations) {
    memory->SystemHeapFree(mirror_address);
    memory->SystemHeapFree(guest_address);
  }
}

}  // namespace legodimensions::gpu_native
