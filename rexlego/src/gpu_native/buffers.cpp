#include "gpu_native/buffers.h"

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

namespace legodimensions::gpu_native {
namespace {

struct BufferResource {
  u32 guest_address = 0;
  u32 mirror_address = 0;
  u32 length = 0;
  u32 format = 0;
  BufferKind kind = BufferKind::kVertex;
  std::mutex upload_mutex;
  std::unique_ptr<plume::RenderBuffer> buffer;
};

std::mutex g_buffers_mutex;
std::unordered_map<u32, std::shared_ptr<BufferResource>> g_buffers;

D3DBuffer* GuestBuffer(u32 guest_address) {
  auto* memory = REX_KERNEL_MEMORY();
  return reinterpret_cast<D3DBuffer*>(memory->virtual_membase() + guest_address);
}

std::shared_ptr<BufferResource> FindBuffer(u32 guest_address) {
  std::lock_guard lock(g_buffers_mutex);
  const auto it = g_buffers.find(guest_address);
  return it == g_buffers.end() ? nullptr : it->second;
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
  const auto resource = FindBuffer(guest_address);
  if (!resource || resource->kind != kind || offset > resource->length ||
      (size != 0 && size > resource->length - offset)) {
    return 0;
  }
  return resource->mirror_address + offset;
}

plume::RenderBuffer* ResolveBufferResource(u32 guest_address, BufferKind kind) {
  const auto resource = FindBuffer(guest_address);
  if (!resource || resource->kind != kind) {
    return nullptr;
  }
  std::lock_guard upload_lock(resource->upload_mutex);
  if (!resource->buffer) {
    auto* device = HostDevice::Device();
    if (!device) {
      return nullptr;
    }
    const auto desc = kind == BufferKind::kIndex
                          ? plume::RenderBufferDesc::IndexBuffer(
                                resource->length, plume::RenderHeapType::UPLOAD)
                          : plume::RenderBufferDesc::VertexBuffer(
                                resource->length, plume::RenderHeapType::UPLOAD);
    resource->buffer = device->createBuffer(desc);
    if (!resource->buffer) {
      REXLOG_ERROR("Native GPU: failed to allocate {} buffer ({} bytes)",
                   kind == BufferKind::kIndex ? "index" : "vertex",
                   resource->length);
      return nullptr;
    }
    resource->buffer->setName(kind == BufferKind::kIndex ? "LEGO guest index buffer"
                                                          : "LEGO guest vertex buffer");
  }

  void* mapped = resource->buffer->map();
  if (!mapped) {
    return nullptr;
  }
  const auto* source =
      REX_KERNEL_MEMORY()->virtual_membase() + resource->mirror_address;
  const u32 element_size =
      kind == BufferKind::kIndex && resource->format == 1 ? 2 : 4;
  ByteSwapElements(mapped, source, resource->length, element_size);
  resource->buffer->unmap();
  return resource->buffer.get();
}

BufferResourceView ResolveBufferResourceView(u32 guest_address,
                                             BufferKind kind) {
  const auto resource = FindBuffer(guest_address);
  if (!resource || resource->kind != kind) {
    return {};
  }
  auto* buffer = ResolveBufferResource(guest_address, kind);
  return {buffer, resource->length, resource->format};
}

bool IsNativeBuffer(u32 guest_address) {
  std::lock_guard lock(g_buffers_mutex);
  return g_buffers.contains(guest_address);
}

u32 NativeBufferType(u32 guest_address) {
  const auto resource = FindBuffer(guest_address);
  return resource ? static_cast<u32>(resource->kind) : 0;
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
  memory->SystemHeapFree(released->mirror_address);
  memory->SystemHeapFree(guest_address);
  return 0;
}

void ResetBufferResources() {
  std::vector<std::pair<u32, u32>> allocations;
  {
    std::lock_guard lock(g_buffers_mutex);
    allocations.reserve(g_buffers.size());
    for (const auto& [guest_address, resource] : g_buffers) {
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
