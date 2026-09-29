#include "gpu_native/textures.h"

#include <algorithm>
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
#include "gpu_native/format.h"

namespace legodimensions::gpu_native {
namespace {

constexpr u32 kPitchAlignment = 0x100;

struct TextureResource {
  u32 guest_address = 0;
  u32 mirror_address = 0;
  u32 mirror_size = 0;
  u32 width = 0;
  u32 height = 0;
  u32 depth = 1;
  u32 levels = 1;
  u32 guest_format = 0;
  u32 d3d_type = 3;
  plume::RenderFormat format = plume::RenderFormat::UNKNOWN;
  bool surface = false;
  std::mutex mutex;
  std::unique_ptr<plume::RenderTexture> texture;
  std::unique_ptr<plume::RenderTextureView> view;
  std::unordered_map<u32, std::unique_ptr<plume::RenderFramebuffer>>
      framebuffers;
  u32 descriptor_index = ~u32{0};
};

std::mutex g_textures_mutex;
std::unordered_map<u32, std::shared_ptr<TextureResource>> g_textures;

template <typename T>
T* GuestAt(u32 guest_address) {
  return reinterpret_cast<T*>(REX_KERNEL_MEMORY()->virtual_membase() +
                              guest_address);
}

std::shared_ptr<TextureResource> FindTexture(u32 guest_address) {
  std::lock_guard lock(g_textures_mutex);
  const auto it = g_textures.find(guest_address);
  return it == g_textures.end() ? nullptr : it->second;
}

void InvalidateFramebufferReferences(u32 guest_address) {
  std::vector<std::shared_ptr<TextureResource>> resources;
  {
    std::lock_guard lock(g_textures_mutex);
    resources.reserve(g_textures.size());
    for (const auto& [address, resource] : g_textures) {
      resources.push_back(resource);
    }
  }
  for (const auto& resource : resources) {
    std::lock_guard lock(resource->mutex);
    resource->framebuffers.erase(guest_address);
  }
}

u32 AlignUp(u32 value, u32 alignment) {
  return (value + alignment - 1) & ~(alignment - 1);
}

u32 MipDimension(u32 value, u32 level) {
  return std::max(1u, value >> std::min(level, 31u));
}

u32 FullMipCount(u32 width, u32 height, u32 depth) {
  u32 largest = std::max({width, height, depth});
  u32 count = 0;
  do {
    ++count;
    largest >>= 1;
  } while (largest);
  return count;
}

u32 RowPitch(plume::RenderFormat format, u32 width) {
  if (IsBlockCompressedFormat(format)) {
    return AlignUp(std::max(1u, (width + 3) / 4) * FormatBlockBytes(format),
                   kPitchAlignment);
  }
  const u32 texel_size = plume::RenderFormatSize(format);
  return texel_size ? AlignUp(width * texel_size, kPitchAlignment) : 0;
}

u32 RowCount(plume::RenderFormat format, u32 height) {
  return IsBlockCompressedFormat(format) ? std::max(1u, (height + 3) / 4)
                                         : height;
}

u32 LevelSize(const TextureResource& resource, u32 level) {
  const u32 width = MipDimension(resource.width, level);
  const u32 height = MipDimension(resource.height, level);
  const u32 depth = resource.d3d_type ==
                            static_cast<u32>(D3DResourceType::kVolumeTexture)
                        ? MipDimension(resource.depth, level)
                        : 1;
  return RowPitch(resource.format, width) * RowCount(resource.format, height) *
         depth;
}

u32 LevelOffset(const TextureResource& resource, u32 level) {
  u32 offset = 0;
  for (u32 i = 0; i < level; ++i) {
    offset += LevelSize(resource, i);
  }
  return offset;
}

bool EnsureMirror(TextureResource& resource) {
  if (resource.mirror_address) {
    return true;
  }
  u32 size = 0;
  for (u32 level = 0; level < resource.levels; ++level) {
    size += LevelSize(resource, level);
  }
  if (!size) {
    return false;
  }
  auto* memory = REX_KERNEL_MEMORY();
  resource.mirror_address = memory->SystemHeapAlloc(size, 0x100);
  if (!resource.mirror_address) {
    return false;
  }
  resource.mirror_size = size;
  memory->Zero(resource.mirror_address, size);
  return true;
}

std::unique_ptr<plume::RenderTexture> CreateHostTexture(
    const TextureResource& resource, u32 usage, u32 multi_sample) {
  auto* device = HostDevice::Device();
  if (!device || resource.format == plume::RenderFormat::UNKNOWN) {
    return nullptr;
  }

  const bool volume = resource.d3d_type ==
                      static_cast<u32>(D3DResourceType::kVolumeTexture);
  const bool cube = resource.d3d_type ==
                    static_cast<u32>(D3DResourceType::kCubeTexture);
  plume::RenderTextureDesc desc;
  desc.dimension = volume ? plume::RenderTextureDimension::TEXTURE_3D
                          : plume::RenderTextureDimension::TEXTURE_2D;
  desc.width = resource.width;
  desc.height = resource.height;
  desc.depth = volume ? resource.depth : 1;
  desc.mipLevels = resource.levels;
  desc.arraySize = cube ? 6 : 1;
  desc.format = resource.format;
  desc.multisampling.sampleCount = plume::RenderSampleCount::COUNT_1;
  if (cube) {
    desc.flags |= plume::RenderTextureFlag::CUBE;
  }
  if (IsDepthFormat(resource.format)) {
    desc.flags |= plume::RenderTextureFlag::DEPTH_TARGET;
  } else if (resource.surface || usage != 0 ||
             IsRenderTargetFormat(resource.format)) {
    desc.flags |= plume::RenderTextureFlag::RENDER_TARGET;
  }
  desc.committed = resource.surface || IsDepthFormat(resource.format) ||
                   usage != 0 || multi_sample != 0;
  return device->createTexture(desc);
}

u32 AllocateResource(const std::shared_ptr<TextureResource>& resource,
                     u32 usage, u32 multi_sample) {
  if (!resource->width || !resource->height || !resource->levels ||
      resource->format == plume::RenderFormat::UNKNOWN) {
    return 0;
  }
  auto* memory = REX_KERNEL_MEMORY();
  const u32 guest_size = resource->surface ? sizeof(D3DSurface)
                                           : sizeof(D3DTexture);
  resource->guest_address = memory->SystemHeapAlloc(guest_size, 0x10);
  if (!resource->guest_address) {
    return 0;
  }
  memory->Zero(resource->guest_address, guest_size);
  resource->texture = CreateHostTexture(*resource, usage, multi_sample);
  if (!resource->texture) {
    memory->SystemHeapFree(resource->guest_address);
    resource->guest_address = 0;
    return 0;
  }
  resource->texture->setName(resource->surface ? "LEGO guest surface"
                                               : "LEGO guest texture");
  if (!IsDepthFormat(resource->format)) {
    plume::RenderTextureViewDesc view_desc;
    view_desc.format = resource->format;
    view_desc.mipLevels = resource->levels;
    if (resource->d3d_type ==
        static_cast<u32>(D3DResourceType::kVolumeTexture)) {
      view_desc.dimension = plume::RenderTextureViewDimension::TEXTURE_3D;
    } else if (resource->d3d_type ==
               static_cast<u32>(D3DResourceType::kCubeTexture)) {
      view_desc.dimension = plume::RenderTextureViewDimension::TEXTURE_CUBE;
    } else {
      view_desc.dimension = plume::RenderTextureViewDimension::TEXTURE_2D;
    }
    resource->view = resource->texture->createTextureView(view_desc);
    if (resource->view) {
      resource->descriptor_index = HostDevice::RegisterTexture(
          resource->texture.get(), resource->view.get());
    }
  }

  if (resource->surface) {
    auto* guest = GuestAt<D3DSurface>(resource->guest_address);
    guest->resource.common = static_cast<u32>(D3DResourceType::kSurface);
    guest->resource.reference_count = 1;
    guest->format = resource->guest_format;
    guest->size_bits = ((resource->width - 1) << 18) |
                       ((resource->height - 1) << 3);
  } else {
    auto* guest = GuestAt<D3DTexture>(resource->guest_address);
    guest->resource.common = static_cast<u32>(D3DResourceType::kTexture);
    guest->resource.reference_count = 1;
  }
  {
    std::lock_guard lock(g_textures_mutex);
    g_textures.emplace(resource->guest_address, resource);
  }
  return resource->guest_address;
}

}  // namespace

u32 CreateTextureResource(u32 width, u32 height, u32 depth, u32 levels,
                          u32 usage, u32 guest_format, u32 /*pool*/,
                          u32 d3d_type) {
  if (!HostDevice::IsReady()) {
    return 0;
  }
  auto resource = std::make_shared<TextureResource>();
  resource->width = width;
  resource->height = height;
  resource->depth = std::max(1u, depth);
  resource->levels = levels ? levels
                            : FullMipCount(width, height, resource->depth);
  resource->guest_format = guest_format;
  resource->d3d_type = d3d_type == 17 || d3d_type == 18 ? d3d_type : 3;
  resource->format = ConvertGuestTextureFormat(guest_format);
  return AllocateResource(resource, usage, 0);
}

u32 CreateSurfaceResource(u32 width, u32 height, u32 guest_format,
                          u32 multi_sample, u32 /*parameters*/) {
  if (!HostDevice::IsReady()) {
    return 0;
  }
  auto resource = std::make_shared<TextureResource>();
  resource->width = width;
  resource->height = height;
  resource->depth = 1;
  resource->levels = 1;
  resource->guest_format = guest_format;
  resource->d3d_type = static_cast<u32>(D3DResourceType::kSurface);
  resource->format = ConvertGuestTextureFormat(guest_format);
  resource->surface = true;
  return AllocateResource(resource, 1, multi_sample);
}

bool LockTextureResource(u32 guest_address, u32 level, u32 locked_rect,
                         u32 /*rect*/, u32 /*flags*/) {
  const auto resource = FindTexture(guest_address);
  if (!resource) {
    return false;
  }
  if (locked_rect) {
    auto* locked = GuestAt<D3DLockedRect>(locked_rect);
    locked->pitch = 0;
    locked->bits = 0;
  }
  if (resource->surface || level >= resource->levels || !locked_rect) {
    return false;
  }
  std::lock_guard lock(resource->mutex);
  if (!EnsureMirror(*resource)) {
    return false;
  }
  auto* locked = GuestAt<D3DLockedRect>(locked_rect);
  locked->pitch = RowPitch(resource->format,
                           MipDimension(resource->width, level));
  locked->bits = resource->mirror_address + LevelOffset(*resource, level);
  return true;
}

bool DescribeTextureResource(u32 guest_address, u32 level, u32 desc_address) {
  const auto resource = FindTexture(guest_address);
  if (!resource) {
    return false;
  }
  if (!desc_address || resource->surface || level >= resource->levels) {
    return false;
  }
  auto* desc = GuestAt<D3DSurfaceDesc>(desc_address);
  desc->format = resource->guest_format;
  desc->type = resource->d3d_type;
  desc->usage = 0;
  desc->pool = 0;
  desc->multi_sample_type = 0;
  desc->multi_sample_quality = 0;
  desc->width = MipDimension(resource->width, level);
  desc->height = MipDimension(resource->height, level);
  return true;
}

bool DescribeSurfaceResource(u32 guest_address, u32 desc_address) {
  const auto resource = FindTexture(guest_address);
  if (!resource) {
    return false;
  }
  if (!resource->surface || !desc_address) {
    return false;
  }
  auto* desc = GuestAt<D3DSurfaceDesc>(desc_address);
  desc->format = resource->guest_format;
  desc->type = 4;
  desc->usage = 0;
  desc->pool = 0;
  desc->multi_sample_type = 0;
  desc->multi_sample_quality = 0;
  desc->width = resource->width;
  desc->height = resource->height;
  return true;
}

TextureResourceView ResolveTextureResource(u32 guest_address) {
  const auto resource = FindTexture(guest_address);
  if (!resource) {
    return {};
  }
  return {resource->texture.get(),
          resource->view.get(),
          resource->format,
          resource->descriptor_index,
          resource->width,
          resource->height,
          resource->surface,
          IsDepthFormat(resource->format)};
}

bool UploadTextureResource(u32 guest_address,
                           plume::RenderCommandList* commands) {
  const auto resource = FindTexture(guest_address);
  if (!resource || !commands || resource->surface ||
      !resource->mirror_address || !resource->mirror_size ||
      !resource->texture || IsDepthFormat(resource->format)) {
    return false;
  }
  // Cube LockRect has a separate ABI and is not hooked yet. Do not upload the
  // same 2D mirror into six faces and silently fabricate corrupt data.
  if (resource->d3d_type ==
      static_cast<u32>(D3DResourceType::kCubeTexture)) {
    return false;
  }

  std::lock_guard resource_lock(resource->mutex);
  u64 upload_size = 0;
  for (u32 level = 0; level < resource->levels; ++level) {
    upload_size = AlignUp(static_cast<u32>(upload_size), 0x200);
    upload_size += LevelSize(*resource, level);
  }
  auto* device = HostDevice::Device();
  if (!device || upload_size == 0 || upload_size > UINT32_MAX) {
    return false;
  }
  auto upload = std::shared_ptr<plume::RenderBuffer>(
      device->createBuffer(plume::RenderBufferDesc::UploadBuffer(upload_size))
          .release());
  if (!upload) {
    REXLOG_ERROR("Native GPU: texture upload allocation failed ({} bytes)",
                 upload_size);
    return false;
  }

  auto* mapped = static_cast<u8*>(upload->map());
  if (!mapped) {
    return false;
  }
  const auto* source =
      REX_KERNEL_MEMORY()->virtual_membase() + resource->mirror_address;
  u64 destination_offset = 0;
  for (u32 level = 0; level < resource->levels; ++level) {
    destination_offset = AlignUp(static_cast<u32>(destination_offset), 0x200);
    const u32 level_size = LevelSize(*resource, level);
    std::memcpy(mapped + destination_offset,
                source + LevelOffset(*resource, level), level_size);
    destination_offset += level_size;
  }
  upload->unmap();

  commands->barriers(
      plume::RenderBarrierStage::COPY,
      plume::RenderTextureBarrier(resource->texture.get(),
                                  plume::RenderTextureLayout::COPY_DEST));
  destination_offset = 0;
  const u32 format_size = plume::RenderFormatSize(resource->format);
  const u32 block_width = plume::RenderFormatBlockWidth(resource->format);
  if (!format_size || !block_width) {
    return false;
  }
  for (u32 level = 0; level < resource->levels; ++level) {
    destination_offset = AlignUp(static_cast<u32>(destination_offset), 0x200);
    const u32 width = MipDimension(resource->width, level);
    const u32 height = MipDimension(resource->height, level);
    const u32 depth = resource->d3d_type ==
                              static_cast<u32>(D3DResourceType::kVolumeTexture)
                          ? MipDimension(resource->depth, level)
                          : 1;
    const u32 pitch = RowPitch(resource->format, width);
    const u32 row_width = (pitch / format_size) * block_width;
    commands->copyTextureRegion(
        plume::RenderTextureCopyLocation::Subresource(
            resource->texture.get(), level),
        plume::RenderTextureCopyLocation::PlacedFootprint(
            upload.get(), resource->format, width, height, depth, row_width,
            destination_offset));
    destination_offset += LevelSize(*resource, level);
  }
  commands->barriers(
      plume::RenderBarrierStage::GRAPHICS,
      plume::RenderTextureBarrier(resource->texture.get(),
                                  plume::RenderTextureLayout::SHADER_READ));
  HostDevice::RetireResource(std::move(upload));
  return true;
}

plume::RenderFramebuffer* ResolveFramebuffer(u32 render_target,
                                             u32 depth_stencil) {
  const auto color = FindTexture(render_target);
  const auto depth = FindTexture(depth_stencil);
  if ((!color || !color->texture) && (!depth || !depth->texture)) {
    return nullptr;
  }
  if (color && IsDepthFormat(color->format)) {
    return nullptr;
  }
  if (depth && !IsDepthFormat(depth->format)) {
    return nullptr;
  }
  const auto owner = depth ? depth : color;
  const u32 key = depth ? render_target : depth_stencil;
  std::lock_guard lock(owner->mutex);
  const auto existing = owner->framebuffers.find(key);
  if (existing != owner->framebuffers.end()) {
    return existing->second.get();
  }

  plume::RenderFramebufferDesc desc;
  const plume::RenderTexture* color_attachments[1];
  if (color && color->texture) {
    color_attachments[0] = color->texture.get();
    desc.colorAttachments = color_attachments;
    desc.colorAttachmentsCount = 1;
  }
  if (depth && depth->texture) {
    desc.depthAttachment = depth->texture.get();
  }
  auto* device = HostDevice::Device();
  if (!device) {
    return nullptr;
  }
  auto framebuffer = device->createFramebuffer(desc);
  if (!framebuffer) {
    REXLOG_ERROR("Native GPU: failed to create framebuffer (rt=0x{:08X}, "
                 "ds=0x{:08X})", render_target, depth_stencil);
    return nullptr;
  }
  auto* result = framebuffer.get();
  owner->framebuffers.emplace(key, std::move(framebuffer));
  return result;
}

bool IsNativeTexture(u32 guest_address) {
  std::lock_guard lock(g_textures_mutex);
  return g_textures.contains(guest_address);
}

u32 NativeTextureType(u32 guest_address) {
  const auto resource = FindTexture(guest_address);
  return resource ? resource->d3d_type : 0;
}

u32 AddRefNativeTexture(u32 guest_address) {
  const auto resource = FindTexture(guest_address);
  if (!resource) {
    return 0;
  }
  auto* header = GuestAt<D3DResource>(guest_address);
  const u32 count = static_cast<u32>(header->reference_count) + 1;
  header->reference_count = count;
  return count;
}

u32 ReleaseNativeTexture(u32 guest_address) {
  std::shared_ptr<TextureResource> released;
  {
    std::lock_guard lock(g_textures_mutex);
    const auto it = g_textures.find(guest_address);
    if (it == g_textures.end()) {
      return ~u32{0};
    }
    auto* header = GuestAt<D3DResource>(guest_address);
    const u32 old_count = static_cast<u32>(header->reference_count);
    const u32 count = old_count ? old_count - 1 : 0;
    header->reference_count = count;
    if (count) {
      return count;
    }
    released = std::move(it->second);
    g_textures.erase(it);
  }
  auto* memory = REX_KERNEL_MEMORY();
  InvalidateFramebufferReferences(guest_address);
  HostDevice::UnregisterTexture(released->descriptor_index);
  HostDevice::RetireResource(released);
  if (released->mirror_address) {
    memory->SystemHeapFree(released->mirror_address);
  }
  memory->SystemHeapFree(guest_address);
  return 0;
}

void ResetTextureResources() {
  std::vector<std::pair<u32, u32>> allocations;
  {
    std::lock_guard lock(g_textures_mutex);
    allocations.reserve(g_textures.size());
    for (const auto& [guest_address, resource] : g_textures) {
      allocations.emplace_back(guest_address, resource->mirror_address);
    }
    g_textures.clear();
  }
  auto* memory = REX_KERNEL_MEMORY();
  for (const auto [guest_address, mirror_address] : allocations) {
    if (mirror_address) {
      memory->SystemHeapFree(mirror_address);
    }
    memory->SystemHeapFree(guest_address);
  }
}

}  // namespace legodimensions::gpu_native
