// Guest-visible texture/surface resources and their CPU mirrors.
#pragma once

#include <array>

#include <plume_render_interface_types.h>
#include <rex/types.h>
#include "gpu_native/resolve_region.h"
#include "gpu_native/pool_copy.h"

namespace plume {
struct RenderFramebuffer;
struct RenderCommandList;
struct RenderTexture;
struct RenderTextureView;
}

namespace legodimensions::gpu_native {

u32 CreateTextureResource(u32 width, u32 height, u32 depth, u32 levels,
                          u32 usage, u32 guest_format, u32 pool, u32 d3d_type);
u32 CreateSurfaceResource(u32 width, u32 height, u32 guest_format,
                          u32 multi_sample, u32 parameters);

bool LockTextureResource(u32 guest_address, u32 level, u32 locked_rect,
                         u32 rect, u32 flags);
bool DescribeTextureResource(u32 guest_address, u32 level, u32 desc_address);
bool DescribeSurfaceResource(u32 guest_address, u32 desc_address);

struct TextureResourceView {
  plume::RenderTexture* texture = nullptr;
  plume::RenderTextureView* view = nullptr;
  plume::RenderFormat format = plume::RenderFormat::UNKNOWN;
  u32 descriptor_index = ~u32{0};
  u32 width = 0;
  u32 height = 0;
  u32 d3d_type = 0;
  bool surface = false;
  bool depth = false;
};

TextureResourceView ResolveTextureResource(u32 guest_address);
// Call at SetTexture and Resolve, where the header supplies the new binding.
// A draw already in flight must retain its old texture and immutable SRV.
void RefreshTextureHeader(u32 guest_address);
// Copies the complete CPU mirror into the host texture and leaves it in
// SHADER_READ. The CPU mirror remains authoritative for readback/correctness.
bool UploadTextureResource(u32 guest_address,
                           plume::RenderCommandList* commands,
                           bool require_content_hash = false);
struct TextureUploadTiming {
  u64 calls = 0;
  u64 source_hits = 0;
  u64 converted_bytes = 0;
  u64 hashed_bytes = 0;
  double source_ms = 0;
  double hash_ms = 0;
  double cpu_ms = 0;
};
bool NativeTextureTimingEnabled();
TextureUploadTiming ConsumeTextureUploadTiming();
bool ResolveTextureFromSurface(u32 destination_texture, u32 source_surface,
                               u32 destination_level,
                               u32 destination_slice, u32 resolve_flags = 0,
                               const ResolveRect* rectangle = nullptr,
                               const ResolvePoint* point = nullptr);
plume::RenderFramebuffer* ResolveFramebuffer(u32 render_target,
                                             u32 depth_stencil);
plume::RenderFramebuffer* ResolveFramebuffer(
    const std::array<u32, 4>& render_targets, u32 depth_stencil);
// Synchronous BeginTiling allocation, before the pass's clear and geometry.
// Guest surface dimensions and Xbox tiling/command-queue fields are unchanged.
bool PromoteTiledSurface(u32 guest_address, u32 width, u32 height);
float SurfaceColorOutputScale(u32 guest_address);
bool PrepareSurfaceDepthAlias(u32 guest_address);
void MarkSurfaceWritten(u32 guest_address);

bool IsNativeTexture(u32 guest_address);
// CPU texels are copied into retained UPLOAD storage; the GPU never reads
// their guest allocation. Render targets and resolved destinations excluded.
bool IsCpuUploadedTexture(u32 guest_address);
void LogUnknownTexture(u32 guest_address, const char* operation);
u32 NativeTextureType(u32 guest_address);
u32 AddRefNativeTexture(u32 guest_address);
u32 ReleaseNativeTexture(u32 guest_address);
void ResetTextureResources();
// A CPU copy must not read stale guest bytes for a GPU-resolved source.
bool PoolCopyHasHostTextureSource(PhysicalCopyRange source);
u32 InvalidatePoolCopyTextures(PhysicalCopyRange destination);

}  // namespace legodimensions::gpu_native
