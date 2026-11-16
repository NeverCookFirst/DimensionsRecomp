// Guest-visible texture/surface resources and their CPU mirrors.
#pragma once

#include <array>
#include <memory>
#include <vector>

#include <plume_render_interface_types.h>
#include <rex/types.h>
#include "gpu_native/render_extent.h"
#include "gpu_native/depth_alias_decision.h"
#include "gpu_native/color_alias_decision.h"
#include "gpu_native/pool_copy.h"
#include "gpu_native/preparation_footprint.h"

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
  // Resource fields from the allocation behind this SRV. Native-owned guest
  // headers have no Xbox fetch constant and retain the existing sampler path.
  std::array<u32, 6> sampling_fetch{};
  bool has_sampling_fetch = false;
  // Exact decoded upload bytes, already hashed by UploadTextureResource.
  // Valid only for CPU-backed ordinary textures, never GPU resolves/depth.
  u64 content_hash = 0;
  bool content_hash_valid = false;
};

TextureResourceView ResolveTextureResource(u32 guest_address);
struct TexturePreparationSnapshot {
  std::shared_ptr<void> owner;
  std::array<u32, 7> header{};
  TexturePreparationFootprint footprint;
  // The capture settled any depth alias in order; a non-aliased format-6 texture binds from the cache.
  bool alias_settled = false;
};
TexturePreparationSnapshot CaptureTexturePreparation(u32 address);
// Same, starting from the record RefreshTextureHeaderOwner just resolved (null looks it up again).
TexturePreparationSnapshot CaptureTexturePreparation(u32 address, std::shared_ptr<void> resolved);
// Current metadata/ranges only, without adoption, upload or recording waits.
// A missing, unreadable, busy or changed borrowed placement is conservative.
// Caller holds no texture resource mutex.
TexturePreparationFootprint CaptureTexturePreparationFootprint(u32 address);
TextureResourceView PrepareCachedTexture(const TexturePreparationSnapshot& snapshot);
enum class TexturePreparationResult { kUnavailable, kUploadFailed, kReady };
// Sampling transaction under LockRecording. Revalidates placement after upload;
// a replacement/relocation is uploaded before publishing its immutable SRV.
// Unavailable initial bindings retain null defaults; failed uploads reject draw.
TexturePreparationResult PrepareTextureResource(u32 guest_address,
    plume::RenderCommandList* commands, bool require_content_hash,
    TextureResourceView& prepared);
TexturePreparationResult PrepareTextureBinding(u32 slot, u32 guest_address,
    const std::array<u32, 6>& fetch, u32 overrides,
    plume::RenderCommandList* commands, bool require_content_hash,
    TextureResourceView& prepared);
// Metadata only, under LockRecording. Guest coordinates remain logical pixels.
NativeRenderExtent NativeTextureExtent(u32 guest_address);
// Call at SetTexture and Resolve, where the header supplies the new binding.
// A draw already in flight must retain its old texture and immutable SRV.
void RefreshTextureHeader(u32 guest_address);
// Same, returning the record it found when the header was unchanged; null after a refresh or miss.
std::shared_ptr<void> RefreshTextureHeaderOwner(u32 guest_address);
// Prepares texture contents for sampling. CPU-backed textures require a
// successful upload; host render targets and native resolves need no upload.
// A false result must not bind stale or uninitialized host storage.
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
// BeginTiling promotion precedes the pass clear and geometry in queue order.
// Guest surface dimensions and Xbox tiling/command-queue fields are unchanged.
bool PromoteTiledSurface(u32 guest_address, u32 width, u32 height);
bool ReconfigureNativeSceneSurfaces();
// Prepare the entire scene group before publishing any storage. A creation/
// descriptor failure preserves all old surfaces. Explicit extent lets a frame
// revision prepare storage before committing its active settings snapshot.
bool ReconfigureNativeSceneSurfaces(NativeRenderExtent extent);
// Preserve existing native D32F/S8 data while changing physical dimensions.
// Guest logical coordinates, ownership and headers are unchanged on success;
// failed preparation leaves the old resource and immutable views published.
bool ResizeNativeDepthSurface(u32 guest_address, u32 width, u32 height);
// Frame-local native composition backed by two reusable immutable allocations.
// End copies logical writes back before restoring scene-sized storage. False
// restoration retains the authoritative native storage for an explicit retry.
bool BeginNativeCompositionTargets(u32 color_address, u32 depth_address);
bool EndNativeCompositionTargets(bool preserve_writes = true);
bool RetryNativeCompositionTargets();
// Caller holds HostDevice::LockRecording. Metadata only: never adopts a guest
// texture or allocates host storage. Returns logical tiling bounds, never
// the upscaled physical dimensions supplied to D3D12.
bool NativePromotedSurfaceExtent(u32 guest_address, u32& width, u32& height);
float SurfaceColorOutputScale(u32 guest_address);
bool PrepareSurfaceDepthAlias(u32 guest_address);
// The two halves of PrepareSurfaceDepthAlias: the decision advances the write
// generations in capture order, the transfer records later in that order.
DepthAliasDecision DecideSurfaceDepthAlias(u32 guest_address);
bool ApplySurfaceDepthAlias(const DepthAliasDecision& decision);
// Capture selects immutable source/destination owners in guest order; the
// transfer records before the draw in the same replay stream.
ColorAliasDecision DecideSurfaceColorAlias(u32 guest_address);
ColorAliasDecisions DecideSurfaceColorAliases(const std::array<u32, 4>& targets);
bool ApplySurfaceColorAliases(const ColorAliasDecisions& decisions);
bool PrepareSurfaceColorAlias(u32 guest_address);
void MarkSurfaceWritten(u32 guest_address, bool captured_write = false);
// Registry record for capture-time pinning; null when unknown.
std::shared_ptr<void> FindTextureOwner(u32 guest_address);

bool IsNativeTexture(u32 guest_address);
// Distinguish zero-fetch native-created headers from cached guest placements.
// Does not adopt a texture or allocate host storage.
bool OwnsNativeTextureHeader(u32 guest_address);
// CPU texels are copied into retained UPLOAD storage; the GPU never reads
// their guest allocation. Render targets and resolved destinations excluded.
bool IsCpuUploadedTexture(u32 guest_address);
void LogUnknownTexture(u32 guest_address, const char* operation);
u32 NativeTextureType(u32 guest_address);
u32 AddRefNativeTexture(u32 guest_address);
u32 ReleaseNativeTexture(u32 guest_address);
void StartTextureResources();
void ResetTextureResources();
// A CPU copy must not read stale guest bytes for a GPU-resolved source.
bool PoolCopyHasHostTextureSource(PhysicalCopyRange source);
// Textures whose pixels live on the GPU, taken once per pool copy batch instead of a scan per copy.
struct PoolCopyGpuSources { std::vector<std::shared_ptr<void>> textures; };
PoolCopyGpuSources CapturePoolCopyGpuSources();
bool PoolCopyGpuSourceOverlaps(const PoolCopyGpuSources& sources, PhysicalCopyRange source);
// Capture thread: false only when no registered texture header or data shares a page with the range.
bool PoolCopyMayTouchTextures(PhysicalCopyRange range);
// Capture thread: the GPU-owned source test and the invalidation, visiting only indexed candidates.
bool PoolCopyGpuSourceIndexed(PhysicalCopyRange source);
// A partial destination overwrite must preserve GPU-produced bytes it leaves
// untouched. Complete backing replacement does not need materialization.
bool PoolCopyGpuDestinationIndexed(PhysicalCopyRange destination);
u32 InvalidatePoolCopyTexturesIndexed(PhysicalCopyRange destination);
// Materialize supported GPU-owned texels in their exact guest tiled layout.
// Unsupported resources/failures keep the source guard active.
bool MaterializePoolCopyTextureSource(PhysicalCopyRange source);
bool MaterializePoolCopyTextureDestination(PhysicalCopyRange destination);
bool MaterializePoolCopyTextureDependencies(PhysicalCopyRange source,PhysicalCopyRange destination);
// LockRect: publish pending GPU writes to this allocation through any texture header.
bool MaterializeTextureForCpuRead(u32 guest_address);
u32 InvalidatePoolCopyTextures(PhysicalCopyRange destination);

// Moves on every texture registry insert or erase.
u64 TextureRegistryGeneration();
// Moves when a registered texture changes its guest footprint in place.
u64 TextureFootprintEpoch();

}  // namespace legodimensions::gpu_native
