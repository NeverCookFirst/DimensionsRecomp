#include "gpu_native/native_recording.h"
#include "gpu_native/pool_copy.h"
#include "gpu_native/footprint_index.h"
#include "gpu_native/textures.h"
#include "gpu_native/native_worker.h"
#include "gpu_native/draw_batch.h"
#include "gpu_native/resource_registry.h"
#include "gpu_native/color_resolve_swizzle.h"
#include "gpu_native/color_alias_publication.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <plume_render_interface.h>
#include <plume_d3d12.h>
// Same XXH3 result, compiled for the game's ISA rather than the SDK library.
#define XXH_INLINE_ALL
#include <xxhash.h>
#include <rex/logging.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include "gpu_native/d3d.h"
#include "gpu_native/device.h"
#include "gpu_native/fsr3_settings.h"
#include "gpu_native/shaders.h"
#include "gpu_native/format.h"
#include "gpu_native/texture_number.h"
#include "gpu_native/texture_swizzle.h"
#include "gpu_native/texture_upload_cache.h"
#include "gpu_native/texture_upload_source.h"
#include "gpu_native/texture_upload_publication.h"
#include "gpu_native/texture_volume_upload.h"
#include "gpu_native/texture_alpha4_upload.h"
#include "gpu_native/long_probe.h"
#include "gpu_native/memory_watch.h"
#include "gpu_native/depth_alias.h"
#include "gpu_native/texture_depth_upload.h"
#include "gpu_native/texture_readback.h"
#include "gpu_native/texture_writeback.h"
#include "gpu_native/physical_write_scope.h"
#include "gpu_native/guest_metadata.h"
#include "gpu_native/vertex_texture_read.h"
#include "gpu_native/slow_operation.h"

namespace legodimensions::gpu_native {
namespace {

constexpr u32 kPitchAlignment = 0x100;
struct TextureResource;
TexturePreparationFootprint TexturePreparationFootprintOf(
    TextureResource& resource, const std::array<u32, 7>* header);

// Owner identities keep a re-placed address from matching an older record's framebuffer.
struct FramebufferKey {
  std::array<u32, 4> colors{};
  u32 depth = 0;
  std::array<const void*, 4> color_owners{};
  const void* depth_owner = nullptr;
  bool operator==(const FramebufferKey&) const = default;
};
struct FramebufferKeyHash {
  size_t operator()(const FramebufferKey& key) const {
    size_t hash = key.depth;
    for (u32 color : key.colors) hash = hash * 16777619u ^ color;
    for (const void* owner : key.color_owners) hash = hash * 16777619u ^ reinterpret_cast<uintptr_t>(owner);
    return hash * 16777619u ^ reinterpret_cast<uintptr_t>(key.depth_owner);
  }
};
struct SurfaceStorage {
  std::unique_ptr<plume::RenderTexture> texture;
  std::unique_ptr<plume::RenderTextureView> view;
  std::unique_ptr<plume::RenderTextureView> resolved_view;
  std::unique_ptr<plume::RenderTexture> sampled_texture;
  std::unique_ptr<plume::RenderTextureView> sampled_view;
  u32 width=0,height=0;
  u32 descriptor=~0u,resolved_descriptor=~0u,sampled_descriptor=~0u;
};

struct TextureResource {
  u32 guest_address = 0;
  u32 mirror_address = 0;
  u32 mirror_size = 0;
  u32 width = 0;
  u32 height = 0;
  u32 logical_width = 0;
  u32 logical_height = 0;
  std::atomic<u64> promoted_viewport{0};
  u32 LogicalWidth() const { return logical_width ? logical_width : width; }
  u32 LogicalHeight() const { return logical_height ? logical_height : height; }
  u32 host_width = 0;
  u32 host_height = 0;
  u32 HostWidth() const { return host_width ? host_width : width; }
  u32 HostHeight() const { return host_height ? host_height : height; }
  u32 depth = 1;
  u32 levels = 1;
  u32 guest_format = 0;
  u32 d3d_type = 3;
  plume::RenderFormat format = plume::RenderFormat::UNKNOWN;
  bool surface = false;
  bool pool_reused = false;
  // TextureDataOverlapsPoolCopy cache: the guest layout only changes with the
  // fetch words or the extent, and PoolCopyHook scans every texture per copy.
  mutable bool layout_cached = false;
  mutable rex::graphics::xenos::xe_gpu_texture_fetch_t layout_fetch{};
  mutable u32 layout_key[4] = {};
  mutable u32 layout_base_bytes = 0;
  mutable u32 layout_mips_bytes = 0;
  std::mutex mutex;
  std::unique_ptr<plume::RenderTexture> texture;
  std::unique_ptr<plume::RenderTextureView> view;
  std::unique_ptr<plume::RenderTextureView> resolved_view;
  std::unique_ptr<plume::RenderTexture> sampled_texture;
  std::unique_ptr<plume::RenderTextureView> sampled_view;
  u32 sampled_descriptor_index = ~0u;
  bool sampled_valid = false;
  std::unordered_map<FramebufferKey, std::shared_ptr<plume::RenderFramebuffer>, FramebufferKeyHash>
      framebuffers;
  // Old immutable SRV slots stay reserved until release, even after their
  // texture storage retires. Never overwrite a descriptor used by an old list.
  std::vector<u32> previous_surface_descriptors;
  std::shared_ptr<SurfaceStorage> composition_storage;
  bool composition_active=false;
  u32 descriptor_index = ~u32{0};
  u32 resolved_descriptor_index = ~u32{0};
  bool owns_guest_memory = true;
  u32 adopted_header_type = 0;
  // Odd while guest_fetch is rewritten in place; readers that see it change take the mutex.
  std::atomic<u32> header_sequence{0};
  rex::graphics::xenos::xe_gpu_texture_fetch_t guest_fetch{};
  bool guest_uploaded = false;
  // CPU preparation and GPU replay have distinct publication points.
  std::weak_ptr<TextureUploadPublication> prepared_upload;
  std::shared_ptr<TextureUploadReplayState> upload_replay;
  u64 guest_content_hash = 0;
  TextureUploadSourceKey upload_source_key{};
  bool upload_source_key_valid = false;
  CpuMemoryStamp cpu_stamp;
  bool resolved_on_host = false;
  // A successful readback makes guest bytes current without changing the
  // logical-RGBA GPU view. Every subsequent resolve invalidates this claim.
  bool guest_backing_current = false;
  TextureWritebackCoverage writeback_coverage;
  // Diagnostic-only cache, serialized by the caller's LockRecording. Keep the
  // global probe deduplication for authority transitions and resource reuse.
  u64 last_authority_probe_signature = 0;
  bool authority_probe_signature_valid = false;
  u32 edram_base = ~0u;
  u32 guest_msaa = 0;
  u64 write_generation = 0;
  std::weak_ptr<ColorAliasInitialization> color_initialization;
  u64 color_initialization_source = 0;
  // Set once another record owns this address; pinned lookups then fall through.
  std::atomic<bool> replaced{false};
  u64 depth_resolve_generation = 0;
  u64 depth_alias_generation = 0;
  ~TextureResource();
};

TextureUploadPublicationCache g_texture_upload_publications(128ull * 1024 * 1024);

// Caller holds the resource mutex. Every cache consumer records its version;
// the final recording stream decides whether that version needs a GPU copy.
bool QueuePreparedTextureUpload(TextureResource& resource, plume::RenderCommandList* commands) {
  const auto upload = resource.prepared_upload.lock();
  if (!commands || !upload) return false;
  g_texture_upload_publications.Touch(upload);
  upload->Enqueue(commands);
  HostDevice::RetireResource(upload);
  return true;
}

void PublishPreparedTextureUpload(TextureResource& resource, plume::RenderCommandList* commands,
    std::unique_ptr<NativeDrawCommands> copy, std::shared_ptr<plume::RenderBuffer> staging, u64 bytes) {
  if (!resource.upload_replay) resource.upload_replay = std::make_shared<TextureUploadReplayState>();
  auto upload = std::make_shared<TextureUploadPublication>(resource.upload_replay,
      std::move(copy), std::move(staging), bytes);
  resource.prepared_upload = upload;
  g_texture_upload_publications.Keep(upload);
  upload->Enqueue(commands);
  HostDevice::RetireResource(std::move(upload));
}

// Caller holds the resource mutex. A readback only preserves GPU authority
// while its published guest bytes remain unchanged. Unknown/disabled watches
// cannot prove that, so the next GPU use must upload the current CPU bytes.
void RefreshPublishedTextureAuthority(TextureResource& resource) {
  if (resource.surface || !resource.resolved_on_host || !resource.guest_backing_current ||
      CpuMemoryUnchanged(resource.cpu_stamp)) return;
  resource.resolved_on_host=false;
  resource.guest_backing_current=false;
  resource.guest_uploaded=false;
  resource.upload_source_key_valid=false;
  resource.sampled_valid=false;
  resource.depth_resolve_generation=0;
  resource.depth_alias_generation=0;
  resource.cpu_stamp={};
  resource.writeback_coverage.Reset();
}

// Only matching raw storage formats have an exact inverse to guest bytes.
TextureReadbackFormat ResolvedTextureReadbackFormat(const TextureResource& resource) {
  using RF=plume::RenderFormat;
  using TF=rex::graphics::xenos::TextureFormat;
  using BF=TextureReadbackFormat;
  const auto guest=resource.guest_fetch.format;
  switch(resource.format) {
    case RF::R8G8B8A8_UNORM: if(guest==TF::k_8_8_8_8)return BF::kRgba8;break;
    case RF::R16G16_UNORM: if(guest==TF::k_16_16)return BF::kRg16;break;
    case RF::R16G16B16A16_UNORM: if(guest==TF::k_16_16_16_16)return BF::kRgba16;break;
    case RF::R16_FLOAT: if(guest==TF::k_16_FLOAT)return BF::kR16Float;break;
    case RF::R16G16_FLOAT: if(guest==TF::k_16_16_FLOAT)return BF::kRg16Float;break;
    case RF::R16G16B16A16_FLOAT: if(guest==TF::k_16_16_16_16_FLOAT)return BF::kRgba16Float;break;
    case RF::R32_FLOAT: if(guest==TF::k_32_FLOAT)return BF::kR32Float;break;
    case RF::R32G32_FLOAT: if(guest==TF::k_32_32_FLOAT)return BF::kRg32Float;break;
    case RF::R32G32B32A32_FLOAT: if(guest==TF::k_32_32_32_32_FLOAT)return BF::kRgba32Float;break;
    default: break;
  }
  return BF::kUnsupported;
}

// Resolve rectangles are in physical host coordinates after NativePhysicalResolve.
void RecordResolvedTexture(TextureResource& resource, const ResolveRegion& region,
                           u32 level, u32 slice) {
  std::lock_guard lock(resource.mutex);
  InvalidateTextureUploadReplay(HostDevice::BeginFrameCommands(), resource.upload_replay);
  resource.prepared_upload.reset();
  resource.resolved_on_host = true;
  resource.guest_backing_current = false;
  if (!level && !slice && !resource.surface && !resource.owns_guest_memory &&
      resource.levels == 1 && resource.depth == 1 &&
      ResolvedTextureReadbackFormat(resource)!=TextureReadbackFormat::kUnsupported)
    resource.writeback_coverage.Record(resource.HostWidth(), resource.HostHeight(),
        region.x, region.y, region.width, region.height);
}

// The game creates and releases EDRAM surfaces every frame (free on Xenos).
// A fresh committed D3D12 texture per CreateSurface cost ~40% of the render
// thread in Oz (profiled 2026-10-10). Surface storage is recycled instead: a
// TextureResource is destroyed only after RetireResource's fence passed, so
// its texture is idle and can be handed to the next surface of the same shape.
// LEGO_NATIVE_NO_SURFACE_POOL=1 disables reuse.
u64 SurfaceStorageBytes(const plume::RenderTexture* texture) {
  if (!texture) return 0;
  const auto* native = static_cast<const plume::D3D12Texture*>(texture);
  if (native->allocation) return native->allocation->GetSize();
  if (!native->device || !native->d3d) return 0;
  const auto desc = native->d3d->GetDesc();
  const auto size = native->device->d3d->GetResourceAllocationInfo(0, 1, &desc).SizeInBytes;
  return size == UINT64_MAX ? 0 : size;
}
namespace surface_pool {
struct Key {
  u32 width, height, format;
  bool operator==(const Key&) const = default;
};
struct KeyHash {
  size_t operator()(const Key& k) const {
    return (size_t(k.width) * 73856093u) ^ (size_t(k.height) * 19349663u) ^ k.format;
  }
};
constexpr size_t kMaxPerKey = 16;
constexpr u64 kMaxCachedBytes = 256ull * 1024 * 1024;
constexpr size_t kMaxCachedKeys = 128;
std::mutex g_mutex;
// Explicit Stop releases storage while the device is alive. Keep the empty
// wrapper alive to avoid cross-translation-unit destructor ordering at exit.
auto& g_free = *new std::unordered_map<Key, std::vector<std::unique_ptr<plume::RenderTexture>>, KeyHash>;
struct Usage {u64 bytes=0,last_use=0;};
auto& g_usage = *new std::unordered_map<Key, Usage, KeyHash>;
u64 g_cached_bytes=0,g_serial=0,g_evictions=0;
bool g_active = false;  // Protected by g_mutex; storage belongs to one device.
std::atomic<u64> g_hits{0}, g_misses{0};
void Start() {
  std::lock_guard lock(g_mutex);
  g_active = true;
}
void Stop() {
  std::lock_guard lock(g_mutex);
  g_active = false;
  g_free.clear();  // The creating device is still alive and all fences passed.
  g_usage.clear(); g_cached_bytes=0; g_serial=0; g_evictions=0;
  g_hits = 0;
  g_misses = 0;
}
bool Enabled() {
  static const bool enabled = !std::getenv("LEGO_NATIVE_NO_SURFACE_POOL");
  return enabled;
}
Key KeyFor(const TextureResource& r) {
  return {r.HostWidth(), r.HostHeight(), u32(r.format)};
}
std::unique_ptr<plume::RenderTexture> Take(const TextureResource& r) {
  std::lock_guard lock(g_mutex);
  if (!g_active) return nullptr;
  auto it = g_free.find(KeyFor(r));
  if (it == g_free.end() || it->second.empty()) {
    ++g_misses;
    return nullptr;
  }
  auto texture = std::move(it->second.back());
  it->second.pop_back();
  auto& usage=g_usage.at(it->first);
  g_cached_bytes-=usage.bytes;usage.last_use=++g_serial;
  if(it->second.empty()){g_usage.erase(it->first);g_free.erase(it);}
  const u64 hits = ++g_hits;
  if ((hits & (hits - 1)) == 0)
    REXLOG_INFO("Native GPU: surface pool hits={} misses={} cached_bytes={} keys={} evictions={}",
        hits,g_misses.load(),g_cached_bytes,g_free.size(),g_evictions);
  return texture;
}
void Give(Key key, std::unique_ptr<plume::RenderTexture> texture) {
  std::lock_guard lock(g_mutex);
  if (!g_active) return;
  const auto size=SurfaceStorageBytes(texture.get());
  if(!size || size>kMaxCachedBytes)return;
  const auto found=g_free.find(key);
  if(found!=g_free.end() && found->second.size()>=kMaxPerKey)return;
  // Storage reaches Give only after its GPU fence. Evict idle textures across
  // all shapes, so visiting worlds cannot retain an unbounded VRAM cache.
  while(!g_free.empty() && (g_cached_bytes>kMaxCachedBytes-size ||
        (g_free.size()>=kMaxCachedKeys && !g_free.contains(key)))) {
    auto oldest=std::min_element(g_free.begin(),g_free.end(),[](const auto& a,const auto& b){
      return g_usage.at(a.first).last_use<g_usage.at(b.first).last_use;
    });
    g_cached_bytes-=g_usage.at(oldest->first).bytes;
    oldest->second.pop_back();++g_evictions;
    if(oldest->second.empty()){g_usage.erase(oldest->first);g_free.erase(oldest);}
  }
  auto& usage=g_usage[key];usage.bytes=size;usage.last_use=++g_serial;
  auto& list = g_free[key];
  g_cached_bytes+=size;
  if (list.size() < kMaxPerKey) list.push_back(std::move(texture));
}
void Give(const TextureResource& r, std::unique_ptr<plume::RenderTexture> texture) {
  Give(KeyFor(r),std::move(texture));
}
}  // namespace surface_pool

TextureResource::~TextureResource() {
  if (!surface || !texture || !surface_pool::Enabled() || levels != 1 ||
      d3d_type != static_cast<u32>(D3DResourceType::kSurface)) return;
  // Views and framebuffers refer to the texture: drop them before recycling.
  framebuffers.clear();
  if(composition_storage) {
    auto alternate=std::move(composition_storage);
    alternate->sampled_view.reset();alternate->sampled_texture.reset();
    alternate->resolved_view.reset();alternate->view.reset();
    surface_pool::Give(surface_pool::Key{alternate->width,alternate->height,u32(format)},std::move(alternate->texture));
  }
  sampled_view.reset();
  sampled_texture.reset();
  resolved_view.reset();
  view.reset();
  surface_pool::Give(*this, std::move(texture));
}

std::unique_ptr<plume::RenderTexture> CreateHostTexture(
    const TextureResource& resource, u32 usage, u32 multi_sample);

std::mutex g_textures_mutex;
// Moves when a registered texture changes its guest memory footprint in place.
std::atomic<u64> g_texture_footprint_epoch{0};
void NoteTextureFootprintChange(u32 guest_address);  // defined with the pool copy index
ResourceRegistry<TextureResource> g_textures;
std::vector<std::shared_ptr<TextureResource>> g_composition_surfaces;
bool g_composition_restore_pending=false;
// Only these resources can contain cached framebuffer dependencies. Serialized
// with framebuffer creation/invalidation/reset by HostDevice::LockRecording;
// weak pointers never extend texture or framebuffer lifetimes.
std::unordered_map<u32, std::weak_ptr<TextureResource>> g_framebuffer_owners;
std::atomic<u32> g_texture_lifecycle_logs{0};
TextureUploadTiming g_upload_timing;  // Serialized by LockRecording.
u64 g_depth_alias_generation = 0;
std::mutex g_alias_mutex;  // Write generations and g_edram_colors.
std::unordered_map<u32, std::weak_ptr<TextureResource>> g_depth_resolves;
std::unordered_map<u32, std::weak_ptr<TextureResource>> g_edram_colors;
ColorAliasTracker g_color_aliases;

bool DepthAliasesEnabled() {
  // Validated native correctness path. Explicit 0 remains available for A/B.
  static const bool enabled = [] {
    const char* value = std::getenv("LEGO_NATIVE_DEPTH_ALIAS");
    return !value || std::strcmp(value, "0") != 0;
  }();
  return enabled;
}

DepthAliasLayout AliasLayout(const TextureResource& resource) {
  const auto& fetch = resource.guest_fetch;
  const u32 address = fetch.base_address << 12;
  return {address < 0x20000000 ? address : REX_KERNEL_MEMORY()->GetPhysicalAddress(address),
      resource.width, resource.height, u32(fetch.pitch), u32(fetch.tiled),
      u32(fetch.endianness), resource.levels, u32(fetch.dimension)};
}

struct UploadTimer {
  SlowNativeOperation slow{"texture-upload"};
  bool enabled = NativeTextureTimingEnabled();
  std::chrono::steady_clock::time_point start;
  UploadTimer() {
    if (enabled) {
      ++g_upload_timing.calls;
      start = std::chrono::steady_clock::now();
    }
  }
  ~UploadTimer() {
    if (enabled) g_upload_timing.cpu_ms +=
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - start).count();
  }
};

// Validate against the SDK allocation/protection table, not VirtualQuery's
// potentially very large host mapped-region scan on every texture binding.
// Includes commitment and allocation holes, not just the starting page.
bool ReadableUploadSpan(u32 address, u32 size) {
  if (!size) return true;
  if (!address || size > 128u * 1024 * 1024) return false;
  const u64 end = u64(address) + size;
  if (end > 0x100000000ull) return false;
  auto* memory = REX_KERNEL_MEMORY();
  const bool physical = address < 0x20000000u;
  if (physical && end > 0x20000000u) return false;
  for (u64 at = address; at < end;) {
    auto* heap = physical ? memory->GetPhysicalHeap() : memory->LookupHeap(u32(at));
    if (!heap || at < heap->heap_base() ||
        at >= u64(heap->heap_base()) + heap->heap_size()) return false;
    const u64 next = std::min(end, u64(heap->heap_base()) + heap->heap_size());
    if (!physical && heap->heap_type()==rex::memory::HeapType::kGuestPhysical) {
      const auto canonical=memory->GetPhysicalAddress(u32(at));
      const auto bytes=u32(next-at);
      if (canonical>=0x20000000u || bytes>0x20000000u-canonical ||
          memory->GetPhysicalAddress(u32(next-1))!=canonical+bytes-1 ||
          !memory->GetPhysicalHeap()->IsRangeCommittedReadable(canonical,bytes)) return false;
      // Empty physical aliases inherit canonical permissions. An explicitly
      // reserved/no-access alias must still reject reads, even if its backing
      // is readable through a different view. This validates residency only;
      // the payload read retains its normal provider/dependency semantics.
      const u32 page=heap->page_size();
      for(u64 here=at;here<next;) {
        const auto limit=std::min(next,(here&~u64(page-1))+page);
        if(!heap->IsRangeCommittedReadable(u32(here),u32(limit-here))) {
          u32 region=u32(here),size=0;
          if(!heap->QueryBaseAndSize(&region,&size)||size) return false;
        }
        here=limit;
      }
    } else if (!heap->IsRangeCommittedReadable(u32(at), u32(next - at))) return false;
    at = next;
  }
  return true;
}

// Uses the existing payload access path, after residency validation. Native
// producer ordering must be settled before entering this upload transaction;
// unlike CPU-authored metadata these bytes may never bypass that dependency.
bool CopyTextureUploadSpan(u32 address, std::span<u8> destination) {
  if (destination.size() > UINT32_MAX || !ReadableUploadSpan(address,u32(destination.size()))) return false;
  const auto* source = address < 0x20000000u
      ? REX_KERNEL_MEMORY()->TranslatePhysical<const u8*>(address)
      : REX_KERNEL_MEMORY()->TranslateVirtual<const u8*>(address);
  std::memcpy(destination.data(),source,destination.size());
  return true;
}

template <typename T>
T* GuestAt(u32 guest_address) {
  return REX_KERNEL_MEMORY()->TranslateVirtual<T*>(guest_address);
}

std::shared_ptr<TextureResource> FindTexture(u32 guest_address) {
  if (auto pinned = FindPinnedOwner(kPinTexture, guest_address)) {
    auto record = std::static_pointer_cast<TextureResource>(std::move(pinned));
    if (!record->replaced.load(std::memory_order_acquire)) return record;
  }
  return g_textures.Lookup(guest_address, g_textures_mutex);
}

// Callers hold LockRecording. True means this exact registry owner remains
// usable after checking its current placement header; false requires lookup
// again because a layout/type change may have retired the owner.
bool RefreshTextureResourceHeader(u32 guest_address,
    const std::shared_ptr<TextureResource>& resource);

bool TexturePlacementHeaderMatches(const TextureResource& resource) {
  if (resource.surface || resource.owns_guest_memory) return false;
  D3DTexture header{};
  if (!CopyNativeGuestMetadata(*REX_KERNEL_MEMORY(),resource.guest_address,&header,sizeof(header))) return false;
  if ((u32(header.resource.common)&0x1Fu)!=resource.adopted_header_type) return false;
  std::array<u32,6> current{},cached{};
  for (u32 i=0;i<current.size();++i) current[i]=header.format.dword[i];
  std::memcpy(cached.data(),&resource.guest_fetch,sizeof(cached));
  return current==cached;
}

// A texture may remain bound while streaming rewrites its placement header.
// Revalidate at use, independently of SetTexture. Refresh retains old storage
// through its submission fences and preserves native resolve authority on an
// address-only relocation.
std::shared_ptr<TextureResource> FindTextureAtUse(u32 guest_address) {
  const auto existing = FindTexture(guest_address);
  if (!existing || existing->owns_guest_memory || existing->surface) return existing;
  static const bool reuse_lookup=[] {
    const char* value=std::getenv("LEGO_NATIVE_REUSE_TEXTURE_LOOKUP");
    return !value || std::strcmp(value,"0")!=0;
  }();
  if (!reuse_lookup) {
    RefreshTextureHeader(guest_address);
    auto current = FindTexture(guest_address);
    return current && (current->owns_guest_memory || current->surface ||
        TexturePlacementHeaderMatches(*current)) ? current : nullptr;
  }
  if (RefreshTextureResourceHeader(guest_address,existing)) return existing;
  auto current = FindTexture(guest_address);
  // A failed metadata read must not return the same stale record. A different
  // registry owner may have been published while the old one was refreshed.
  return current != existing ? current : nullptr;
}

std::shared_ptr<TextureResource> AdoptTexture(u32 guest_address) {
  auto recording = HostDevice::LockRecording();
  if (!guest_address || !HostDevice::IsReady()) {
    return nullptr;
  }
  if (const auto existing = FindTextureAtUse(guest_address)) return existing;

  D3DTexture metadata{};
  if (!CopyNativeGuestMetadata(*REX_KERNEL_MEMORY(),guest_address,&metadata,sizeof(metadata))) return nullptr;
  const auto* guest = &metadata;
  const u32 common = guest->resource.common;
  const u32 d3d_type = common & 0x1Fu;
  if (d3d_type != static_cast<u32>(D3DResourceType::kTexture) &&
      d3d_type != static_cast<u32>(D3DResourceType::kVolumeTexture) &&
      d3d_type != static_cast<u32>(D3DResourceType::kCubeTexture)) {
    return nullptr;
  }

  const u32 word0 = guest->format.dword[0];
  const u32 word1 = guest->format.dword[1];
  const u32 word2 = guest->format.dword[2];
  const u32 word4 = guest->format.dword[4];
  const u32 word5 = guest->format.dword[5];
  const u32 dimension = (word5 >> 9) & 3u;
  auto resource = std::make_shared<TextureResource>();
  resource->guest_address = guest_address;
  resource->owns_guest_memory = false;
  resource->adopted_header_type = d3d_type;
  u32 fetch_words[6];
  for (u32 i = 0; i < 6; ++i) fetch_words[i] = guest->format.dword[i];
  std::memcpy(&resource->guest_fetch, fetch_words, sizeof(fetch_words));
  resource->guest_format = word1 & 0x3Fu;
  resource->format = ConvertXenosTextureFormat(resource->guest_format);
  resource->d3d_type = d3d_type;
  resource->surface = false;
  if (dimension == 2) {
    resource->width = (word2 & 0x7FFu) + 1;
    resource->height = ((word2 >> 11) & 0x7FFu) + 1;
    resource->depth = ((word2 >> 22) & 0x3FFu) + 1;
    resource->d3d_type = static_cast<u32>(D3DResourceType::kVolumeTexture);
  } else {
    resource->width = (word2 & 0x1FFFu) + 1;
    resource->height = ((word2 >> 13) & 0x1FFFu) + 1;
    resource->depth = dimension == 3 ? 6 : 1;
    if (dimension == 3) {
      resource->d3d_type = static_cast<u32>(D3DResourceType::kCubeTexture);
    }
  }
  const u32 mip_page = (word5 >> 12) & 0xFFFFFu;
  resource->levels = mip_page ? (((word4 >> 6) & 0xFu) + 1) : 1;
  if (!resource->width || !resource->height ||
      resource->format == plume::RenderFormat::UNKNOWN) {
    return nullptr;
  }
  resource->texture = CreateHostTexture(*resource, 0, 0);
  if (!resource->texture) {
    return nullptr;
  }
  resource->texture->setName((std::string("LEGO texture ") + std::to_string(guest_address) +
      " base " + std::to_string(resource->guest_fetch.base_address << 12)).c_str());
  {
    plume::RenderTextureViewDesc view_desc;
    constexpr plume::RenderSwizzle swizzles[] = {
        plume::RenderSwizzle::R, plume::RenderSwizzle::G,
        plume::RenderSwizzle::B, plume::RenderSwizzle::A,
        plume::RenderSwizzle::ZERO, plume::RenderSwizzle::ONE,
        plume::RenderSwizzle::ZERO, plume::RenderSwizzle::ZERO};
    const u32 swizzle = resource->guest_fetch.swizzle;
    const u32 host_swizzle = NativeTextureSwizzle(resource->guest_format, swizzle);
    view_desc.componentMapping = plume::RenderComponentMapping(
        swizzles[host_swizzle & 7], swizzles[(host_swizzle >> 3) & 7],
        swizzles[(host_swizzle >> 6) & 7], swizzles[(host_swizzle >> 9) & 7]);
    view_desc.format = resource->format;
    // A/B diagnostic: sample only the base level to isolate mip upload faults.
    // LEGO_NATIVE_FORCE_MIP=k views only level k (clamped) to tell mip data
    // faults from level-selection faults.
    static const int forced_mip = [] {
      if (std::getenv("LEGO_NATIVE_MIP0_ONLY")) return 0;
      const char* v = std::getenv("LEGO_NATIVE_FORCE_MIP");
      return v ? std::atoi(v) : -1;
    }();
    view_desc.mipLevels = forced_mip >= 0 ? 1 : resource->levels;
    if (forced_mip > 0) view_desc.mipSlice = std::min(u32(forced_mip), resource->levels - 1);
    view_desc.dimension =
        resource->d3d_type == static_cast<u32>(D3DResourceType::kVolumeTexture)
            ? plume::RenderTextureViewDimension::TEXTURE_3D
        : resource->d3d_type == static_cast<u32>(D3DResourceType::kCubeTexture)
            ? plume::RenderTextureViewDimension::TEXTURE_CUBE
            : plume::RenderTextureViewDimension::TEXTURE_2D;
    resource->view = resource->texture->createTextureView(view_desc);
    if (resource->view) {
      resource->descriptor_index = HostDevice::RegisterTexture(
          resource->texture.get(), resource->view.get());
    }
    // The CPU upload contains guest BGRA storage, so its SRV needs ZYXW.
    // A native color resolve instead copies logical RGBA render-target data.
    // Give that representation its own immutable SRV; applying the guest
    // storage swizzle a second time swaps red and blue in every resolved image.
    const u32 resolved_swizzle = NativeColorResolveSwizzle(swizzle);
    if (resource->format == plume::RenderFormat::R8G8B8A8_UNORM &&
        resolved_swizzle != swizzle) {
      view_desc.componentMapping = plume::RenderComponentMapping(
          plume::RenderSwizzle::R, plume::RenderSwizzle::G,
          plume::RenderSwizzle::B, swizzles[(resolved_swizzle >> 9) & 7]);
      resource->resolved_view = resource->texture->createTextureView(view_desc);
      if (resource->resolved_view)
        resource->resolved_descriptor_index = HostDevice::RegisterTexture(
            resource->texture.get(), resource->resolved_view.get());
    }
  }
  std::shared_ptr<TextureResource> winner;
  {
    std::lock_guard lock(g_textures_mutex);
    const auto [it, inserted] = g_textures.emplace(guest_address, resource);
    if (!inserted) {
      winner = it->second;
    }
  }
  if (winner) {
    HostDevice::UnregisterTexture(resource->descriptor_index);
    HostDevice::UnregisterTexture(resource->resolved_descriptor_index);
    return winner;
  }
  REXLOG_INFO(
      "Native GPU: adopted guest texture 0x{:08X} {}x{} depth={} dimension={} levels={} "
      "xenos_format={} pitch={} tiled={} num={} exp={} sign={},{},{},{} swizzle={:03X}",
      guest_address, resource->width, resource->height, resource->depth, dimension, resource->levels,
      resource->guest_format, ((word0 >> 22) & 0x1FFu) << 5, word0 >> 31,
      u32(resource->guest_fetch.num_format), i32(resource->guest_fetch.exp_adjust),
      u32(resource->guest_fetch.sign_x), u32(resource->guest_fetch.sign_y),
      u32(resource->guest_fetch.sign_z), u32(resource->guest_fetch.sign_w),
      u32(resource->guest_fetch.swizzle));
  return resource;
}

// Keep the raw UNORM16 storage (including resolve quantization) and expose a
// separate float SRV with Xenos integer/exp-adjust sampling semantics. This
// narrow path only covers single-level, unsigned, identity-swizzled RGBA16.
// It does not redefine CPU uploads or modify the linked game shaders.
// Integer num_format destinations take integer packing (value * 2^exp written
// as an integer, read back x65535 * 2^exp_adjust). Packing them as clamped
// UNORM like the Xenia resolve saturates the bloom source and whites out the
// screen (tested 2026-10-10); LEGO_NATIVE_UNORM_RESOLVE=1 brings that back.
static bool IntegerResolvePacking(bool integer) {
  static const bool unorm = std::getenv("LEGO_NATIVE_UNORM_RESOLVE") != nullptr;
  return integer && !unorm;
}

bool UpdateResolvedSampling(TextureResource& resource) {
  const auto& fetch = resource.guest_fetch;
  if ((resource.format != plume::RenderFormat::R16G16B16A16_UNORM &&
       resource.format != plume::RenderFormat::R16G16_UNORM) ||
      (!fetch.num_format && !fetch.exp_adjust)) return true;
  if (resource.levels != 1 || resource.d3d_type != u32(D3DResourceType::kTexture) ||
      fetch.swizzle != 0x688 || u32(fetch.sign_x) || u32(fetch.sign_y) ||
      u32(fetch.sign_z) || u32(fetch.sign_w)) return false;
  resource.sampled_valid = false;
  if (!resource.sampled_texture) {
    TextureResource description;
    description.width = resource.HostWidth();
    description.height = resource.HostHeight();
    description.levels = 1;
    description.format = plume::RenderFormat::R32G32B32A32_FLOAT;
    resource.sampled_texture = CreateHostTexture(description, 1, 0);
    if (!resource.sampled_texture) return false;
    plume::RenderTextureViewDesc view;
    view.dimension = plume::RenderTextureViewDimension::TEXTURE_2D;
    view.format = description.format;
    view.mipLevels = 1;
    resource.sampled_view = resource.sampled_texture->createTextureView(view);
    if (!resource.sampled_view) return false;
    resource.sampled_descriptor_index = HostDevice::RegisterTexture(
        resource.sampled_texture.get(), resource.sampled_view.get());
  }
  if (resource.sampled_descriptor_index == ~0u) return false;
  const float scale = Unsigned16SampleScale(fetch.exp_adjust, fetch.num_format != 0);
  if (!HostDevice::ResolveHdrColor(resource.texture.get(), resource.descriptor_index,
          resource.sampled_texture.get(), resource.HostWidth(), resource.HostHeight(), scale,
          ColorResolveDestination::kFloatRGBA32, false))
    return false;
  resource.sampled_valid = true;
  HostDevice::SnapshotTexture(resource.sampled_texture.get(),
      "sampled-" + std::to_string(resource.guest_address), false);
  return true;
}

void InvalidateFramebufferReferences(u32 guest_address, const void* identity = nullptr) {
  auto recording = HostDevice::LockRecording();
  std::vector<std::shared_ptr<TextureResource>> resources;
  {
    std::lock_guard lock(g_textures_mutex);
    resources.reserve(g_framebuffer_owners.size());
    for (auto it = g_framebuffer_owners.begin(); it != g_framebuffer_owners.end();) {
      const auto resource = it->second.lock();
      const auto current = g_textures.find(it->first);
      // A released/replaced owner retains its own framebuffers through fenced
      // retirement. Only current owners need their attachment keys invalidated.
      if (!resource || current == g_textures.end() || current->second != resource) {
        it = g_framebuffer_owners.erase(it);
      } else {
        resources.push_back(resource);
        ++it;
      }
    }
  }
  for (const auto& resource : resources) {
    std::vector<std::shared_ptr<plume::RenderFramebuffer>> retired;
    {
      std::lock_guard lock(resource->mutex);
      for (auto it = resource->framebuffers.begin(); it != resource->framebuffers.end();) {
        const auto& key = it->first;
        const bool named = key.depth == guest_address ||
            std::find(key.colors.begin(), key.colors.end(), guest_address) != key.colors.end();
        const bool owned = !identity || key.depth_owner == identity ||
            std::find(key.color_owners.begin(), key.color_owners.end(), identity) != key.color_owners.end();
        if (named && owned) {
          retired.push_back(std::move(it->second));
          it = resource->framebuffers.erase(it);
        } else ++it;
      }
      if (resource->framebuffers.empty())
        g_framebuffer_owners.erase(resource->guest_address);
    }
    for (auto& framebuffer : retired) HostDevice::RetireResource(std::move(framebuffer));
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

// Opt-in diagnostic: inspect the actual base-level bytes uploaded to D3D12,
// independently of draw geometry, shaders, swizzle and sampler state.
struct TextureCaptureState {
  std::filesystem::path root, trigger, active_root;
  std::string stage;
  std::unordered_set<std::string> completed_stage_names;
  bool all_levels = false, armed = false;
  bool pixel_shader_filter_enabled = false, pixel_shader_filter_valid = true;
  u64 pixel_shader_filter = 0;
  u32 checked_frame = ~u32{0};
  u64 written_bytes = 0;
  std::unordered_set<u32> dumped;
};

TextureCaptureState& TextureCapture() {
  static TextureCaptureState state = [] {
    TextureCaptureState result;
    char* value = nullptr;
    size_t length = 0;
    _dupenv_s(&value, &length, "LEGO_DUMP_TEXTURE_UPLOADS");
    result.root = value ? value : "";
    std::free(value);
    value = nullptr;
    _dupenv_s(&value, &length, "LEGO_DUMP_TEXTURE_UPLOADS_TRIGGER");
    result.trigger = value ? value : "";
    std::free(value);
    result.all_levels = std::getenv("LEGO_DUMP_TEXTURE_UPLOADS_ALL_LEVELS") != nullptr;
    value = nullptr;
    _dupenv_s(&value, &length, "LEGO_DUMP_TEXTURE_UPLOADS_PIXEL_SHADER");
    if (value) {
      result.pixel_shader_filter_enabled = true;
      result.pixel_shader_filter_valid = std::strlen(value) == 16;
      if (result.pixel_shader_filter_valid) {
        for (const char* digit = value; *digit; ++digit) {
          u32 nibble;
          if (*digit >= '0' && *digit <= '9') nibble = *digit - '0';
          else if (*digit >= 'a' && *digit <= 'f') nibble = *digit - 'a' + 10;
          else if (*digit >= 'A' && *digit <= 'F') nibble = *digit - 'A' + 10;
          else { result.pixel_shader_filter_valid = false; break; }
          result.pixel_shader_filter = (result.pixel_shader_filter << 4) | nibble;
        }
      }
      if (!result.pixel_shader_filter_valid) {
        REXLOG_WARN("Native GPU: texture upload capture disabled: LEGO_DUMP_TEXTURE_UPLOADS_PIXEL_SHADER must be exactly 16 hexadecimal digits");
      }
    }
    std::free(value);
    return result;
  }();
  return state;
}

// The renderer recording lock serializes capture state, just like CPU upload.
// Disabled captures do no filesystem work. The trigger is checked once/frame.
bool TextureUploadCapturePending(const TextureResource& resource) {
  auto& state = TextureCapture();
  if (state.root.empty() || state.written_bytes >= 128ull * 1024 * 1024) return false;
  if (!state.pixel_shader_filter_valid ||
      (state.pixel_shader_filter_enabled &&
       BoundShaderHash(ShaderStage::kPixel) != state.pixel_shader_filter)) return false;
  if (!state.trigger.empty() && state.checked_frame != g_probe_frame.load()) {
    state.checked_frame = g_probe_frame.load();
    std::error_code error;
    state.armed = std::filesystem::is_regular_file(state.trigger, error) && !error;
    if (state.armed && state.all_levels) {
      // A fresh stage name rearms the per-scene quota. Never reset the global
      // byte budget or overwrite an earlier scene. Read bounded trigger text.
      char name[34]{};
      std::ifstream input(state.trigger, std::ios::binary);
      input.read(name, sizeof(name));
      std::string stage(name, size_t(input.gcount()));
      while (!stage.empty() && (stage.back() == '\n' || stage.back() == '\r')) stage.pop_back();
      if (stage.empty()) stage = "capture";
      const bool valid = stage.size() <= 32 && std::all_of(stage.begin(), stage.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
      });
      if (!valid || (stage != state.stage &&
          (state.completed_stage_names.size() >= 4 || state.completed_stage_names.contains(stage)))) {
        state.armed = false;
      } else if (stage != state.stage) {
        state.completed_stage_names.insert(stage);
        state.stage = std::move(stage);
        state.active_root = state.root / state.stage;
        state.dumped.clear();
      }
    }
  }
  if (!state.trigger.empty() && !state.armed) return false;
  if (state.dumped.size() >= (state.all_levels ? 32u : 8u) ||
      state.dumped.contains(resource.guest_address)) return false;
  if (state.all_levels && (u32(resource.guest_fetch.dimension) != 1 || resource.levels > 16)) return false;
  if (resource.format != plume::RenderFormat::BC1_UNORM &&
      resource.format != plume::RenderFormat::BC3_UNORM &&
      resource.format != plume::RenderFormat::R8G8B8A8_UNORM) return false;
  static const bool logos_only = std::getenv("LEGO_DUMP_TEXTURE_UPLOADS_LOGOS_ONLY") != nullptr;
  if (logos_only) {
    const u64 vs = BoundShaderHash(ShaderStage::kVertex);
    const u64 ps = BoundShaderHash(ShaderStage::kPixel);
    if (!((vs == 0x91007ACB3E640E3Dull && ps == 0xEEE573BE160E037Dull) ||
          (vs == 0xC3958E2D1B795ED9ull && ps == 0x0C1840BF35E84F2Full))) return false;
  }
  return true;
}

void DumpTextureUpload(const TextureResource& resource, const u8* data,
                       std::span<const u8> base = {}, std::span<const u8> mips = {}) {
  if (!TextureUploadCapturePending(resource)) return;
  auto& state = TextureCapture();
  const auto& root = state.active_root.empty() ? state.root : state.active_root;
  const bool bc1 = resource.format == plume::RenderFormat::BC1_UNORM;
  const bool bc3 = resource.format == plume::RenderFormat::BC3_UNORM;
  const bool compressed = bc1 || bc3;
  if (!compressed && resource.format != plume::RenderFormat::R8G8B8A8_UNORM) return;
  state.dumped.insert(resource.guest_address);
  const u32 levels = state.all_levels ? resource.levels : 1;
  u64 payload_size = 128;
  for (u32 level = 0; level < levels; ++level) {
    const u32 w = std::max(1u, resource.width >> level), h = std::max(1u, resource.height >> level);
    payload_size += u64(compressed ? ((w + 3) / 4) * (bc1 ? 8 : 16) : w * 4) * RowCount(resource.format, h);
  }
  if (state.all_levels) payload_size += base.size() + mips.size();
  if (payload_size > 128ull * 1024 * 1024 - state.written_bytes) return;
  state.written_bytes += payload_size;
  std::error_code error;
  std::filesystem::create_directories(root, error);
  if (error) return;
  // Legacy DDS, little-endian host. Strip row padding and 512-byte mip gaps.
  u32 header[32]{};
  header[0] = 0x20534444; header[1] = 124;
  header[2] = 0x1007 | (compressed ? 0x80000 : 0x8);
  header[3] = resource.height; header[4] = resource.width;
  const u32 row_bytes = compressed ? ((resource.width + 3) / 4) * (bc1 ? 8 : 16) : resource.width * 4;
  const u32 rows = RowCount(resource.format, resource.height);
  header[5] = compressed ? row_bytes * rows : row_bytes;
  header[19] = 32; header[20] = compressed ? 4 : 0x41;
  header[21] = bc1 ? 0x31545844 : bc3 ? 0x35545844 : 0;
  if (!compressed) {
    header[22] = 32; header[23] = 0xFF; header[24] = 0xFF00;
    header[25] = 0xFF0000; header[26] = 0xFF000000;
  }
  header[27] = 0x1000;
  if (levels > 1) { header[2] |= 0x20000; header[7] = levels; header[27] |= 0x400008; }
  const std::string name = std::to_string(resource.guest_address);
  std::ofstream file(root / (name + ".dds"), std::ios::binary);
  file.write(reinterpret_cast<const char*>(header), sizeof(header));
  u64 offset = 0;
  for (u32 level = 0; level < levels; ++level) {
    offset = (offset + 511) & ~u64{511};
    const u32 w = std::max(1u, resource.width >> level), h = std::max(1u, resource.height >> level);
    const u32 bytes = compressed ? ((w + 3) / 4) * (bc1 ? 8 : 16) : w * 4;
    const u32 level_rows = RowCount(resource.format, h), pitch = RowPitch(resource.format, w);
    for (u32 row = 0; row < level_rows; ++row)
      file.write(reinterpret_cast<const char*>(data + offset + u64(row) * pitch), bytes);
    offset += u64(pitch) * level_rows;
  }
  if (state.all_levels) {
    for (const auto& item : {std::pair{".guest-base.bin", base}, std::pair{".guest-mips.bin", mips}}) {
      if (item.second.empty()) continue;
      std::ofstream raw(root / (name + item.first), std::ios::binary);
      raw.write(reinterpret_cast<const char*>(item.second.data()), item.second.size());
    }
  }
  std::ofstream metadata(root / (name + ".txt"));
  const auto& fetch = resource.guest_fetch;
  metadata << "guest=" << std::hex << resource.guest_address
           << " base=" << (fetch.base_address << 12) << " mip=" << (fetch.mip_address << 12)
           << " swizzle=" << fetch.swizzle << std::dec << " endian=" << u32(fetch.endianness)
           << " pitch=" << (fetch.pitch << 5) << " tiled=" << fetch.tiled
           << " packed=" << fetch.packed_mips << " levels=" << levels
           << " width=" << resource.width << " height=" << resource.height
           << " frame=" << g_probe_frame.load() << std::hex
           << " VS=" << BoundShaderHash(ShaderStage::kVertex)
           << " PS=" << BoundShaderHash(ShaderStage::kPixel) << '\n';
  std::array<u32, 6> words{};
  std::memcpy(words.data(), &fetch, sizeof(words));
  metadata << "fetch=";
  for (const auto word : words) metadata << word << ',';
  metadata << '\n';
  REXLOG_INFO("Native GPU: captured texture upload {} ({}x{}, format={})",
              name, resource.width, resource.height, resource.guest_format);
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
  NoteTextureFootprintChange(resource.guest_address);
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
  desc.width = resource.HostWidth();
  desc.height = resource.HostHeight();
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
  } else if (IsRenderTargetFormat(resource.format)) {
    desc.flags |= plume::RenderTextureFlag::RENDER_TARGET;
  }
  // Placed render targets start with undefined compression metadata; committed ones are zeroed and initialized.
  desc.committed = resource.surface || IsDepthFormat(resource.format) ||
                   usage != 0 || multi_sample != 0 ||
                   (desc.flags & plume::RenderTextureFlag::RENDER_TARGET);
  auto texture = device->createTexture(desc);
  // Plume may return a wrapper even when D3D12 resource creation failed.
  // Do not dereference that wrapper through setName or createTextureView.
  if (texture && !static_cast<plume::D3D12Texture*>(texture.get())->d3d) {
    REXLOG_ERROR("Native GPU: texture allocation failed {}x{} depth={} mips={} format={}",
                 desc.width, desc.height, desc.depth, desc.mipLevels,
                 static_cast<u32>(desc.format));
    return nullptr;
  }
  return texture;
}

void PublishPromotedViewport(TextureResource& resource) {
  const u32 width = resource.logical_width ? resource.logical_width : resource.width;
  const u32 height = resource.logical_height ? resource.logical_height : resource.height;
  const bool promoted = resource.surface && resource.texture && width && height &&
      width <= 16384 && height <= 32768 &&
      (width != resource.width || height != resource.height);
  resource.promoted_viewport.store(promoted ? (u64(width) << 32) | height : 0,
                                   std::memory_order_release);
}

// Prepared allocations have not been published to a guest resource. Aborted
// groups release their new descriptor slots. A depth copy may already have
// recorded transitions, so its tentative storage still needs fence retention.
struct PreparedNativeStorage {
  TextureResource* resource = nullptr;
  NativeRenderExtent extent{};
  std::shared_ptr<SurfaceStorage> replacement, retired;
  bool resized = false, recorded_copy = false;
  PreparedNativeStorage() = default;
  PreparedNativeStorage(PreparedNativeStorage&&) = default;
  PreparedNativeStorage(const PreparedNativeStorage&) = delete;
  PreparedNativeStorage& operator=(const PreparedNativeStorage&) = delete;
  PreparedNativeStorage& operator=(PreparedNativeStorage&&) = delete;
  ~PreparedNativeStorage() {
    if (!replacement) return;
    for (u32 descriptor : {replacement->descriptor, replacement->resolved_descriptor})
      if (descriptor != ~0u) HostDevice::UnregisterTexture(descriptor);
    if (recorded_copy) HostDevice::RetireResource(std::move(replacement));
  }
};

bool PrepareNativeStorage(TextureResource& resource, NativeRenderExtent extent,
                          bool preserve_depth, PreparedNativeStorage& prepared) {
  if (preserve_depth && (!resource.surface ||
      resource.format != plume::RenderFormat::D32_FLOAT_S8_UINT)) return false;
  prepared.resource = &resource;
  prepared.extent = extent;
  if (resource.HostWidth() == extent.width && resource.HostHeight() == extent.height) return true;
  prepared.replacement = std::make_shared<SurfaceStorage>();
  prepared.retired = std::make_shared<SurfaceStorage>();
  auto& replacement = *prepared.replacement;
  TextureResource description;
  description.width=extent.width; description.height=extent.height;
  description.levels=resource.levels; description.surface=resource.surface;
  description.format=resource.format; description.d3d_type=resource.d3d_type;
  replacement.texture=CreateHostTexture(description, 1, 0);
  if (!replacement.texture || !resource.view) return false;
  replacement.view=replacement.texture->createTextureView(static_cast<plume::D3D12TextureView*>(resource.view.get())->desc);
  if (!replacement.view) return false;
  if (resource.resolved_view) {
    replacement.resolved_view=replacement.texture->createTextureView(static_cast<plume::D3D12TextureView*>(resource.resolved_view.get())->desc);
    if (!replacement.resolved_view) return false;
  }
  replacement.descriptor=HostDevice::RegisterTexture(replacement.texture.get(),replacement.view.get());
  if (replacement.descriptor==~0u) return false;
  replacement.resolved_descriptor=replacement.resolved_view ?
      HostDevice::RegisterTexture(replacement.texture.get(),replacement.resolved_view.get()) : ~0u;
  if (replacement.resolved_view && replacement.resolved_descriptor==~0u) return false;
  // Do allocating bookkeeping before publishing the first member of a group.
  resource.previous_surface_descriptors.reserve(resource.previous_surface_descriptors.size()+3);
  if (preserve_depth) {
    prepared.recorded_copy = true;
    if (!HostDevice::ResampleDepthSurface(resource.texture.get(),replacement.texture.get())) return false;
  }
  prepared.resized = true;
  return true;
}

void CommitNativeStorage(PreparedNativeStorage& prepared,
                         std::shared_ptr<SurfaceStorage>* saved_storage = nullptr) {
  auto& resource = *prepared.resource;
  const auto extent = prepared.extent;
  if (!prepared.resized) {
    resource.logical_width=extent.logical_width; resource.logical_height=extent.logical_height;
    PublishPromotedViewport(resource);
    return;
  }
  InvalidateFramebufferReferences(resource.guest_address);
  auto retired=std::move(prepared.retired);
  retired->width=resource.HostWidth();retired->height=resource.HostHeight();
  retired->descriptor=resource.descriptor_index;
  retired->resolved_descriptor=resource.resolved_descriptor_index;
  retired->sampled_descriptor=resource.sampled_descriptor_index;
  retired->texture=std::move(resource.texture); retired->view=std::move(resource.view);
  retired->resolved_view=std::move(resource.resolved_view);
  retired->sampled_texture=std::move(resource.sampled_texture);
  retired->sampled_view=std::move(resource.sampled_view);
  for (u32 old : {resource.descriptor_index,resource.resolved_descriptor_index,resource.sampled_descriptor_index})
    if (old!=~0u) resource.previous_surface_descriptors.push_back(old);
  auto& replacement = *prepared.replacement;
  resource.texture=std::move(replacement.texture); resource.view=std::move(replacement.view);
  resource.prepared_upload.reset(); resource.upload_replay.reset();
  resource.resolved_view=std::move(replacement.resolved_view);
  resource.descriptor_index=std::exchange(replacement.descriptor,~0u);
  resource.resolved_descriptor_index=std::exchange(replacement.resolved_descriptor,~0u);
  prepared.recorded_copy = false;
  resource.sampled_descriptor_index=~0u; resource.sampled_valid=false;
  resource.guest_uploaded=false; resource.resolved_on_host=false;
  resource.guest_backing_current=false;resource.writeback_coverage.Reset();
  resource.depth_alias_generation=0; resource.depth_resolve_generation=0;
  resource.logical_width=extent.logical_width; resource.logical_height=extent.logical_height;
  resource.host_width=extent.width; resource.host_height=extent.height;
  PublishPromotedViewport(resource);
  if(saved_storage) *saved_storage=std::move(retired);
  else HostDevice::RetireResource(std::move(retired));
  REXLOG_INFO("Native GPU: physical storage {:08X} logical={}x{} host={}x{} surface={}",
      resource.guest_address,extent.logical_width,extent.logical_height,extent.width,extent.height,resource.surface);
}

// Replace storage and every immutable SRV as one transaction. Submitted lists
// retain their old storage until the host fence retires it.
bool ResizeNativeStorage(TextureResource& resource, NativeRenderExtent extent,
                         bool preserve_depth = false,
                         std::shared_ptr<SurfaceStorage>* saved_storage = nullptr) {
  PreparedNativeStorage prepared;
  if (!PrepareNativeStorage(resource,extent,preserve_depth,prepared)) return false;
  CommitNativeStorage(prepared,saved_storage);
  return true;
}

// Both backing allocations and SRV slots remain immutable and owned by the
// surface. Swap storage metadata only after a full content copy succeeds.
void SwapCompositionStorage(TextureResource& resource) {
  auto& saved=*resource.composition_storage;
  InvalidateFramebufferReferences(resource.guest_address);
  for(u32 descriptor:{resource.descriptor_index,resource.resolved_descriptor_index,
                      resource.sampled_descriptor_index})
    if(descriptor!=~0u && std::find(resource.previous_surface_descriptors.begin(),
        resource.previous_surface_descriptors.end(),descriptor)==resource.previous_surface_descriptors.end())
      resource.previous_surface_descriptors.push_back(descriptor);
  const u32 width=resource.HostWidth(),height=resource.HostHeight();
  resource.host_width=saved.width;resource.host_height=saved.height;
  saved.width=width;saved.height=height;
  std::swap(resource.texture,saved.texture);std::swap(resource.view,saved.view);
  std::swap(resource.resolved_view,saved.resolved_view);
  std::swap(resource.sampled_texture,saved.sampled_texture);
  std::swap(resource.sampled_view,saved.sampled_view);
  std::swap(resource.descriptor_index,saved.descriptor);
  std::swap(resource.resolved_descriptor_index,saved.resolved_descriptor);
  std::swap(resource.sampled_descriptor_index,saved.sampled_descriptor);
  resource.sampled_valid=false;resource.guest_uploaded=false;
  resource.resolved_on_host=false;resource.guest_backing_current=false;resource.writeback_coverage.Reset();
  resource.depth_alias_generation=0;resource.depth_resolve_generation=0;
  resource.authority_probe_signature_valid=false;
}

bool CopyCompositionStorage(TextureResource& resource, bool to_saved) {
  auto& saved=*resource.composition_storage;
  auto* source=to_saved?resource.texture.get():saved.texture.get();
  auto* destination=to_saved?saved.texture.get():resource.texture.get();
  const u32 descriptor=to_saved?resource.descriptor_index:saved.descriptor;
  const u32 width=to_saved?saved.width:resource.HostWidth();
  const u32 height=to_saved?saved.height:resource.HostHeight();
  if(resource.format==plume::RenderFormat::D32_FLOAT_S8_UINT)
    return HostDevice::ResampleDepthSurface(source,destination);
  const ResolveRegion output{0,0,width,height,0,0};
  const ResolveRegion input{0,0,to_saved?resource.HostWidth():saved.width,
      to_saved?resource.HostHeight():saved.height,0,0};
  return HostDevice::ResolveHdrColor(source,descriptor,destination,width,height,1.0f,
      ColorResolveDestination::kScaledRGBA8,true,&output,&input);
}

bool BeginCompositionSurface(TextureResource& resource, u32 width, u32 height) {
  if(resource.composition_active) return resource.HostWidth()==width && resource.HostHeight()==height;
  if(resource.HostWidth()==width && resource.HostHeight()==height) return true;
  if(resource.composition_storage &&
      (resource.composition_storage->width!=width || resource.composition_storage->height!=height)) {
    // Mode changes can leave an obsolete alternate. Its descriptor slots stay
    // reserved with the owner while submitted lists retain its old allocation.
    HostDevice::RetireResource(std::move(resource.composition_storage));
  }
  if(!resource.composition_storage) {
    if(!ResizeNativeStorage(resource,{resource.LogicalWidth(),resource.LogicalHeight(),width,height},
        resource.format==plume::RenderFormat::D32_FLOAT_S8_UINT,&resource.composition_storage)) return false;
    if(resource.format==plume::RenderFormat::R8G8B8A8_UNORM && !CopyCompositionStorage(resource,false)) {
      SwapCompositionStorage(resource);return false;
    }
  } else {
    if(!CopyCompositionStorage(resource,true)) return false;
    SwapCompositionStorage(resource);
  }
  resource.composition_active=true;
  return true;
}

bool EnsureDepthSamplingMirror(TextureResource& resource) {
  if (resource.sampled_texture && resource.sampled_view &&
      resource.sampled_descriptor_index != ~0u) return true;
  plume::RenderTextureDesc desc;
  desc.dimension = plume::RenderTextureDimension::TEXTURE_2D;
  desc.width = resource.HostWidth(); desc.height = resource.HostHeight();
  desc.depth = 1; desc.mipLevels = 1; desc.arraySize = 1;
  desc.multisampling.sampleCount = plume::RenderSampleCount::COUNT_1;
  desc.format = plume::RenderFormat::R32_FLOAT;
  desc.flags |= plume::RenderTextureFlag::RENDER_TARGET;
  desc.committed = true;
  auto texture = HostDevice::Device()->createTexture(desc);
  if (!texture || !static_cast<plume::D3D12Texture*>(texture.get())->d3d) return false;
  plume::RenderTextureViewDesc view_desc;
  view_desc.dimension = plume::RenderTextureViewDimension::TEXTURE_2D;
  view_desc.format = plume::RenderFormat::R32_FLOAT;
  view_desc.mipLevels = 1;
  constexpr plume::RenderSwizzle swizzles[] = {
      plume::RenderSwizzle::R, plume::RenderSwizzle::G,
      plume::RenderSwizzle::B, plume::RenderSwizzle::A,
      plume::RenderSwizzle::ZERO, plume::RenderSwizzle::ONE};
  const u32 swizzle = NativeTextureSwizzle(resource.guest_format & 0x3Fu,
                                         resource.guest_fetch.swizzle);
  view_desc.componentMapping = plume::RenderComponentMapping(
      swizzles[swizzle & 7], swizzles[(swizzle >> 3) & 7],
      swizzles[(swizzle >> 6) & 7], swizzles[(swizzle >> 9) & 7]);
  auto view = texture->createTextureView(view_desc);
  if (!view) return false;
  const u32 descriptor = HostDevice::RegisterTexture(texture.get(), view.get());
  if (descriptor == ~0u) return false;
  // Publish a complete immutable descriptor only after all allocation steps
  // succeed. Host resolves reuse this same mirror and remain authoritative.
  resource.sampled_texture = std::move(texture);
  resource.sampled_view = std::move(view);
  resource.sampled_descriptor_index = descriptor;
  return true;
}

bool UploadGuestDepthTexture(TextureResource& resource,
                             plume::RenderCommandList* commands,
                             bool require_content_hash) {
  namespace tu = rex::graphics::texture_util;
  using rex::graphics::xenos::DataDimension;
  std::lock_guard lock(resource.mutex);
  const auto& fetch = resource.guest_fetch;
  // Only the actual UNORM24 2D atlas path is supported here. FLOAT24 has a
  // different encoding; cubes, volumes and mip chains need separate coverage.
  if (!commands || resource.surface || resource.owns_guest_memory ||
      resource.resolved_on_host || !resource.texture || resource.guest_format != 22 ||
      !IsDepthFormat(resource.format) || resource.levels != 1 ||
      resource.d3d_type != u32(D3DResourceType::kTexture) ||
      fetch.dimension != DataDimension::k2DOrStacked || fetch.stacked ||
      !fetch.base_address || fetch.num_format || fetch.exp_adjust ||
      (u32(fetch.sign_x) | u32(fetch.sign_y) | u32(fetch.sign_z) | u32(fetch.sign_w)))
    return false;
  const bool audit = std::getenv("LEGO_NATIVE_AUDIT_TEXTURE_WATCH") != nullptr;
  if (!require_content_hash && !audit && resource.guest_uploaded &&
      resource.sampled_valid && CpuMemoryUnchanged(resource.cpu_stamp) &&
      QueuePreparedTextureUpload(resource, commands)) {
    if (NativeTextureTimingEnabled()) ++g_upload_timing.source_hits;
    return true;
  }
  const auto layout = tu::GetGuestTextureLayout(fetch.dimension, fetch.pitch,
      resource.width, resource.height, 1, fetch.tiled, fetch.format,
      fetch.packed_mips, true, 0);
  const u32 address = fetch.base_address << 12;
  const u32 size = layout.base.level_data_extent_bytes;
  if (!size || layout.base.array_slice_data_extent_bytes > size ||
      !ReadableUploadSpan(address, size)) return false;
  const CpuMemorySpan spans[] = {{address, size}};
  auto stamp = CpuMemoryWatchEnabled() ? WatchCpuMemory(spans) : CpuMemoryStamp{};
  const auto retained = TextureUploadSource::Capture(address,size,0,0,CopyTextureUploadSpan);
  if (!retained) return false;
  const auto* source = retained->Base().data();
  const auto start = NativeTextureTimingEnabled() ? std::chrono::steady_clock::now()
                                                : std::chrono::steady_clock::time_point{};
  const auto key = MakeTextureUploadSourceKey({source, size}, {},
      {reinterpret_cast<const u8*>(&fetch), sizeof(fetch)});
  if (NativeTextureTimingEnabled()) {
    const double elapsed = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - start).count();
    g_upload_timing.hash_ms += elapsed;
    g_upload_timing.source_ms += elapsed;
    g_upload_timing.hashed_bytes += size;
  }
  if (resource.guest_uploaded && resource.sampled_valid &&
      resource.upload_source_key_valid && resource.upload_source_key == key &&
      QueuePreparedTextureUpload(resource, commands)) {
    resource.cpu_stamp = std::move(stamp);
    if (NativeTextureTimingEnabled()) ++g_upload_timing.source_hits;
    return true;
  }
  const u32 pitch = RowPitch(plume::RenderFormat::R32_FLOAT, resource.width);
  const u64 bytes = u64(pitch) * resource.height;
  if (!resource.width || !resource.height || !bytes || bytes > 128u * 1024 * 1024)
    return false;
  std::vector<u8> decoded(static_cast<size_t>(bytes));
  u32 offset_x = 0, offset_y = 0, offset_z = 0;
  if (fetch.packed_mips)
    tu::GetPackedMipOffset(resource.width, resource.height, 1, fetch.format,
                           0, offset_x, offset_y, offset_z);
  const u32 endian_xor = u32(fetch.endianness) == 1 ? 1
      : u32(fetch.endianness) == 2 ? 3 : u32(fetch.endianness) == 3 ? 2 : 0;
  if (!CopyTextureDepth24({source, layout.base.array_slice_data_extent_bytes},
          decoded, resource.width, resource.height, pitch, endian_xor,
          [&](u32 x, u32 y, u32) -> int64_t {
            if (fetch.tiled)
              return tu::GetTiledOffset2D(x + offset_x, y + offset_y,
                  layout.base.row_pitch_bytes / 4, 2);
            return u64(y + offset_y) * layout.base.row_pitch_bytes +
                   u64(x + offset_x) * 4;
          })) return false;
  // A failed host resolve can leave resized storage without valid contents.
  // Do not mark a partial guest-sized CPU upload into that mirror as valid.
  if (resource.HostWidth()!=resource.width || resource.HostHeight()!=resource.height) return false;
  if (!EnsureDepthSamplingMirror(resource)) return false;
  auto upload = std::shared_ptr<plume::RenderBuffer>(HostDevice::Device()->createBuffer(
      plume::RenderBufferDesc::UploadBuffer(static_cast<u32>(bytes))).release());
  if (!upload) return false;
  auto* mapped = upload->map();
  if (!mapped) return false;
  std::memcpy(mapped, decoded.data(), decoded.size());
  upload->unmap();
  auto copy = CreateNativeDrawCommands();
  copy->barriers(plume::RenderBarrierStage::COPY,
      plume::RenderTextureBarrier(resource.sampled_texture.get(), plume::RenderTextureLayout::COPY_DEST));
  copy->copyTextureRegion(
      plume::RenderTextureCopyLocation::Subresource(resource.sampled_texture.get(), 0, 0),
      plume::RenderTextureCopyLocation::PlacedFootprint(upload.get(), plume::RenderFormat::R32_FLOAT,
          resource.width, resource.height, 1, pitch / 4, 0));
  copy->barriers(plume::RenderBarrierStage::GRAPHICS,
      plume::RenderTextureBarrier(resource.sampled_texture.get(), plume::RenderTextureLayout::SHADER_READ));
  PublishPreparedTextureUpload(resource, commands, std::move(copy), std::move(upload), bytes);
  resource.guest_uploaded = true;
  resource.sampled_valid = true;
  resource.guest_content_hash = XXH3_64bits(decoded.data(), decoded.size());
  resource.upload_source_key = key;
  resource.upload_source_key_valid = true;
  resource.cpu_stamp = std::move(stamp);
  if (NativeTextureTimingEnabled()) g_upload_timing.converted_bytes += bytes;
  if (LongProbeEnabled()) LongProbeEvent("depth_texture_upload", false,
      "guest=", resource.guest_address, "base=", address, "width=", resource.width,
      "height=", resource.height, "bytes=", bytes);
  return true;
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
  if (resource->surface && usage == 1 && resource->levels == 1 &&
      resource->d3d_type == static_cast<u32>(D3DResourceType::kSurface) &&
      surface_pool::Enabled())
    resource->texture = surface_pool::Take(*resource);
  resource->pool_reused = resource->texture != nullptr;
  if (!resource->texture)
    resource->texture = CreateHostTexture(*resource, usage, multi_sample);
  if (!resource->texture) {
    memory->SystemHeapFree(resource->guest_address);
    resource->guest_address = 0;
    return 0;
  }
  resource->texture->setName(resource->surface ? "LEGO guest surface"
                                               : "LEGO guest texture");
  {
    plume::RenderTextureViewDesc view_desc;
    view_desc.format = resource->format;
    // A/B diagnostic: sample only the base level to isolate mip upload faults.
    // LEGO_NATIVE_FORCE_MIP=k views only level k (clamped) to tell mip data
    // faults from level-selection faults.
    static const int forced_mip = [] {
      if (std::getenv("LEGO_NATIVE_MIP0_ONLY")) return 0;
      const char* v = std::getenv("LEGO_NATIVE_FORCE_MIP");
      return v ? std::atoi(v) : -1;
    }();
    view_desc.mipLevels = forced_mip >= 0 ? 1 : resource->levels;
    if (forced_mip > 0) view_desc.mipSlice = std::min(u32(forced_mip), resource->levels - 1);
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
  if (g_texture_lifecycle_logs.fetch_add(1, std::memory_order_relaxed) < 64) {
    REXLOG_INFO(
        "Native GPU: created {} 0x{:08X} {}x{} levels={} format=0x{:08X}",
        resource->surface ? "surface" : "texture", resource->guest_address,
        resource->width, resource->height, resource->levels,
        resource->guest_format);
  }
  return resource->guest_address;
}

}  // namespace

bool NativeTextureTimingEnabled() {
  static const bool enabled = [] {
    char* value = nullptr;
    size_t size = 0;
    _dupenv_s(&value, &size, "LEGO_NATIVE_TIMING");
    const bool result = value && *value;
    std::free(value);
    return result;
  }();
  return enabled;
}

TextureUploadTiming ConsumeTextureUploadTiming() {
  auto recording = HostDevice::LockRecording();
  return std::exchange(g_upload_timing, {});
}

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
                          u32 multi_sample, u32 parameters) {
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
  const auto extent = NativeConfiguredSceneExtent(width, height);
  resource->host_width = extent.width; resource->host_height = extent.height;
  if (parameters && !ReadableUploadSpan(parameters, 12)) return 0;
  const u32 address = AllocateResource(resource, 1, multi_sample);
  if (!address) return 0;
  u32 texture_format = guest_format & 63;
  if (texture_format == 54) texture_format = 7;
  // The TU23 XG surface-header helper uses this read-only XEX format table.
  const u32 table = *GuestAt<const be_u16>(0x824C9660u + texture_format * 2);
  u32 render_format = (table >> 8) & 15;
  const u32 base = parameters ? u32(*GuestAt<const be_u32>(parameters)) & 0xFFF : 0;
  resource->edram_base = parameters ? base : ~0u;
  resource->guest_msaa = multi_sample;
  const bool depth = texture_format == 22 || texture_format == 23;
  resource->texture->setName((std::string("LEGO surface ") + std::to_string(address) +
      " base " + std::to_string(base)).c_str());
  const u32 bias = parameters && !depth ? u32(*GuestAt<const be_u32>(parameters + 8)) & 63 : 0;
  if (!render_format && (guest_format & 0x3FE00) == 0x7E00) render_format = 1;
  auto* guest = GuestAt<D3DSurface>(address);
  // +28 is RB_COLOR_INFO for color surfaces, RB_DEPTH_INFO for depth.
  guest->depth_info = base | ((depth ? (texture_format == 23 ? 1u : 0u) : render_format) << 16) |
      (depth ? 0u : bias << 20);
  // A new committed texture starts zeroed and the game relies on that (some
  // pass accumulates into a fresh surface without clearing it: whiteout).
  // Give a recycled one the same contents.
  // The clear queues behind earlier draws like any other host work; the
  // previous owner's draws already passed their fence before the pool gave
  // the storage back.
  if (resource->pool_reused) {
    const auto clear = [address, resource] {
      auto recording = HostDevice::LockRecording();
      auto* commands = HostDevice::BeginFrameCommands();
      auto* framebuffer = IsDepthFormat(resource->format) ? ResolveFramebuffer(0, address)
                                                          : ResolveFramebuffer(address, 0);
      if (!commands || !framebuffer) return;
      if (IsDepthFormat(resource->format)) {
        commands->barriers(plume::RenderBarrierStage::GRAPHICS, plume::RenderTextureBarrier(
            resource->texture.get(), plume::RenderTextureLayout::DEPTH_WRITE));
        commands->setFramebuffer(framebuffer);
        commands->clearDepthStencil(true, true, 0.0f, 0, nullptr, 0);
      } else {
        commands->barriers(plume::RenderBarrierStage::GRAPHICS, plume::RenderTextureBarrier(
            resource->texture.get(), plume::RenderTextureLayout::COLOR_WRITE));
        commands->setFramebuffer(framebuffer);
        commands->clearColor(0, plume::RenderColor(0.0f, 0.0f, 0.0f, 0.0f), nullptr, 0);
      }
    };
    if (NativeWorkerShouldQueue() && NativeQueuedSurfaceClearEnabled()) NativeWorkerSubmit(clear);
    else clear();
  }
  static std::atomic<u32> logs{0};
  if (!depth && logs.fetch_add(1) < 24)
    REXLOG_INFO("Native GPU: surface color info {:08X} guest_fmt={:08X} rt_fmt={} bias={} scale={}",
        address, guest_format, render_format, ColorExponentBias(guest->depth_info),
        ColorOutputScale(guest->depth_info));
  return address;
}

// Four calls per draw on the worker; the registry generation proves the cached
// owner is still the one registered at this address.
float SurfaceColorOutputScale(u32 guest_address) {
  if (!guest_address) return 1.0f;
  decltype(HostDevice::LockRecording()) recording;
  if (!CurrentDrawPreparation() || NativeFsr3Enabled()) recording = HostDevice::LockRecording();
  struct Entry { u32 address = 0; u64 generation = ~u64{0}; std::weak_ptr<TextureResource> owner; };
  static thread_local std::array<Entry, 256> cache;
  auto& entry = cache[((guest_address >> 5) ^ (guest_address >> 13)) & 255];
  const u64 generation = g_textures.BucketVersion(guest_address);
  if (entry.address != guest_address || entry.generation != generation)
    entry = {guest_address, generation, FindTexture(guest_address)};
  const auto resource = entry.owner.lock();
  return resource && resource->surface && !IsDepthFormat(resource->format)
      ? ColorOutputScale(GuestAt<const D3DSurface>(guest_address)->depth_info) : 1.0f;
}

ColorAliasLayout SurfaceColorAliasLayout(const TextureResource& resource) {
  return {resource.edram_base, resource.LogicalWidth(), resource.LogicalHeight(),
      resource.HostWidth(), resource.HostHeight(), resource.surface &&
      resource.guest_msaa == 0 && resource.format == plume::RenderFormat::R8G8B8A8_UNORM,
      resource.guest_msaa == 0 && ((resource.guest_format & 63) == 6 ||
          (resource.guest_format & 63) == 22 || (resource.guest_format & 63) == 23 ||
          (resource.guest_format & 63) == 36)};
}

ColorAliasDecision DecideSurfaceColorAlias(u32 guest_address) {
  if (!guest_address) return {};
  const auto destination = FindTexture(guest_address);
  if (!destination || !destination->surface) return {};
  std::lock_guard alias(g_alias_mutex);
  const auto previous = g_color_aliases.Find(SurfaceColorAliasLayout(*destination));
  if (previous.owner == destination) {
    if (!previous.initialization) return {};
    return {{}, destination, {}, destination->texture.get(), 0, previous.initialization,
        previous.initialization->Dependencies()};
  }
  auto source = previous.owner;
  if (!source) return {};
  const auto producer = std::static_pointer_cast<TextureResource>(source);
  auto copy = CreateNativeDrawCommands();
  // A skipped earlier producer still has to initialize its source allocation
  // before this consumer inherits it. Nested tapes publish only at final replay.
  const plume::RenderTextureBarrier barriers[] = {
      {producer->texture.get(), plume::RenderTextureLayout::COPY_SOURCE},
      {destination->texture.get(), plume::RenderTextureLayout::COPY_DEST}};
  copy->barriers(plume::RenderBarrierStage::COPY, barriers, 2);
  copy->copyTextureRegion(
      plume::RenderTextureCopyLocation::Subresource(destination->texture.get(), 0),
      plume::RenderTextureCopyLocation::Subresource(producer->texture.get(), 0));
  // Captured raw allocations are immutable until ordered replay. A dependency
  // mistake or later storage replacement fails closed instead of copying it.
  const auto* source_texture = producer->texture.get();
  const auto* destination_texture = destination->texture.get();
  const auto* source_record = producer.get();
  const auto* destination_record = destination.get();
  auto dependencies = previous.initialization ? previous.initialization->Dependencies() : std::vector<u32>{};
  if (dependencies.size() >= 64) dependencies = {0}; // conservative dependency overflow
  if ((dependencies.empty() || dependencies[0]) &&
      std::find(dependencies.begin(), dependencies.end(), producer->guest_address) == dependencies.end())
    dependencies.push_back(producer->guest_address);
  auto initialization = ColorAliasInitialization::Create(
      std::move(copy), std::vector<std::shared_ptr<void>>{source},
      [source_record, destination_record, source_texture, destination_texture] {
        return source_record->texture.get() == source_texture &&
            destination_record->texture.get() == destination_texture &&
            SurfaceColorAliasLayout(*source_record) == SurfaceColorAliasLayout(*destination_record);
      }, [](std::shared_ptr<void> owner) { HostDevice::RetireResource(std::move(owner)); }, dependencies, previous.initialization);
  destination->color_initialization = initialization;
  destination->color_initialization_source = previous.sequence;
  return {std::move(source), destination, producer->texture.get(),
      destination->texture.get(), producer->guest_address, std::move(initialization), std::move(dependencies)};
}

ColorAliasDecisions DecideSurfaceColorAliases(const std::array<u32, 4>& targets) {
  ColorAliasDecisions result;
  for (u32 i = 0; i < targets.size(); ++i) result[i] = DecideSurfaceColorAlias(targets[i]);
  return result;
}

bool ApplySurfaceColorAliases(const ColorAliasDecisions& decisions) {
  auto recording = HostDevice::LockRecording();
  for (const auto& decision : decisions) {
    if (!decision.initialization || !decision.destination) continue;
    const auto destination = std::static_pointer_cast<TextureResource>(decision.destination);
    if (!destination->texture ||
        destination->texture.get() != decision.destination_texture ||
        !SurfaceColorAliasLayout(*destination).supported) return false;
    auto* commands = HostDevice::BeginFrameCommands();
    if (!commands) return false;
    decision.initialization->Enqueue(commands);
    HostDevice::RetireResource(destination);
  }
  return true;
}

bool PrepareSurfaceColorAlias(u32 guest_address) {
  ColorAliasDecisions decisions;
  decisions[0] = DecideSurfaceColorAlias(guest_address);
  return ApplySurfaceColorAliases(decisions);
}

void MarkSurfaceWritten(u32 guest_address, bool captured_write) {
  if (!guest_address) return;
  // Every draw marks its targets; the registry generation proves the cached owner.
  struct Entry { u32 address = 0; u64 generation = ~u64{0}; std::weak_ptr<TextureResource> owner; };
  static thread_local std::array<Entry, 256> cache;
  auto& entry = cache[((guest_address >> 5) ^ (guest_address >> 13)) & 255];
  const u64 generation = g_textures.BucketVersion(guest_address);
  if (entry.address != guest_address || entry.generation != generation)
    entry = {guest_address, generation, FindTexture(guest_address)};
  const auto resource = entry.owner.lock();
  if (!resource || !resource->surface) return;
  std::shared_ptr<ColorAliasInitialization> initialization;
  std::vector<std::shared_ptr<void>> retired_color_owners;
  {
  std::lock_guard alias(g_alias_mutex);
  const auto previous = g_color_aliases.Find(SurfaceColorAliasLayout(*resource));
  if (previous.owner == resource) initialization = previous.initialization;
  else if (resource->color_initialization_source == previous.sequence)
    initialization = resource->color_initialization.lock();
  retired_color_owners = g_color_aliases.Written(SurfaceColorAliasLayout(*resource), resource, initialization);
  if (DepthAliasesEnabled()) {
  resource->write_generation = ++g_depth_alias_generation;
  if (resource->edram_base != ~0u && resource->guest_msaa == 0 &&
      resource->format == plume::RenderFormat::R8G8B8A8_UNORM) {
    auto& slot = g_edram_colors[resource->edram_base];
    if (slot.owner_before(resource) || resource.owner_before(slot) || slot.expired()) slot = resource;
  }
  }
  }
  if (!captured_write && initialization)
    initialization->Overwritten(HostDevice::BeginFrameCommands());
}

DepthAliasDecision DecideSurfaceDepthAlias(u32 guest_address) {
  if (!DepthAliasesEnabled() || !guest_address) return {};
  const auto destination = FindTexture(guest_address);
  if (!destination || !destination->surface || destination->edram_base == ~0u ||
      destination->guest_msaa != 0 || (destination->guest_format & 63) != 22 ||
      !DepthColorAliasHalfTile(destination->LogicalWidth(), destination->HostWidth())) return {};
  std::lock_guard alias(g_alias_mutex);
  const auto found = g_edram_colors.find(destination->edram_base);
  const auto source = found == g_edram_colors.end() ? nullptr : found->second.lock();
  if (!source || source->write_generation <= destination->write_generation ||
      source->HostWidth() != destination->HostWidth() ||
      source->HostHeight() != destination->HostHeight() ||
      source->LogicalWidth() != destination->LogicalWidth()) return {};
  destination->write_generation = source->write_generation;
  return {source, destination};
}

bool ApplySurfaceDepthAlias(const DepthAliasDecision& decision) {
  if (!decision.source || !decision.destination) return true;
  auto recording = HostDevice::LockRecording();
  const auto source = std::static_pointer_cast<TextureResource>(decision.source);
  const auto destination = std::static_pointer_cast<TextureResource>(decision.destination);
  if (!source->texture || !destination->texture) return false;
  if (!HostDevice::TransferDepthAlias(source->texture.get(), source->descriptor_index,
      destination->texture.get(), destination->HostWidth(), destination->HostHeight(), true,
      DepthColorAliasHalfTile(destination->LogicalWidth(), destination->HostWidth()))) return false;
  static std::atomic<u32> logs{0};
  if (logs.fetch_add(1) < 24) REXLOG_INFO("Native GPU: EDRAM color->depth alias {:08X}->{:08X} base={} {}x{}",
      source->guest_address, destination->guest_address, destination->edram_base,
      destination->HostWidth(), destination->HostHeight());
  return true;
}

bool PrepareSurfaceDepthAlias(u32 guest_address) {
  auto recording = HostDevice::LockRecording();
  return ApplySurfaceDepthAlias(DecideSurfaceDepthAlias(guest_address));
}

bool LockTextureResource(u32 guest_address, u32 level, u32 locked_rect,
                         u32 /*rect*/, u32 /*flags*/) {
  const auto resource = FindTexture(guest_address);
  // Borrowed textures upload from their Xenos allocations, not this mirror.
  // Let the XDK lock expose that same allocation (and its rect/mip layout).
  if (!resource || !resource->owns_guest_memory) {
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
  if (!resource || !resource->owns_guest_memory) {
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
  if (!resource || !resource->owns_guest_memory) {
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

namespace {
bool RefreshTextureResourceHeader(u32 guest_address,
    const std::shared_ptr<TextureResource>& resource) {
  if (!resource) return false;
  if (resource->owns_guest_memory || resource->surface) return true;
  D3DTexture metadata{};
  if (!CopyNativeGuestMetadata(*REX_KERNEL_MEMORY(),guest_address,&metadata,sizeof(metadata))) return false;
  const auto* header = &metadata;
  std::array<u32, 6> current{}, cached{};
  for (u32 i = 0; i < current.size(); ++i) current[i] = header->format.dword[i];
  {
    // Unchanged header without the record's mutex, which an upload may hold for a whole hash;
    // a torn or concurrent rewrite falls through to the locked path.
    const u32 sequence = resource->header_sequence.load(std::memory_order_acquire);
    if (!(sequence & 1u)) {
      auto* words = reinterpret_cast<u32*>(&resource->guest_fetch);
      for (u32 i = 0; i < cached.size(); ++i)
        cached[i] = std::atomic_ref<u32>(words[i]).load(std::memory_order_relaxed);
      std::atomic_thread_fence(std::memory_order_acquire);
      if (resource->header_sequence.load(std::memory_order_relaxed) == sequence && cached == current &&
          resource->adopted_header_type == (u32(header->resource.common) & 0x1Fu))
        return true;
    }
  }
  bool address_only = false;
  {
    std::lock_guard lock(resource->mutex);
    std::memcpy(cached.data(), &resource->guest_fetch, sizeof(cached));
    const bool same_type = resource->adopted_header_type == (u32(header->resource.common) & 0x1Fu);
    if (cached == current && same_type) return true;
    // Header addresses are CPU aliases, including the E/F +4 KB mapping.
    // Keep them raw for UploadTextureResource; SetTexture separately converts
    // the active device fetch to physical addresses and preserves sampler bits.
    auto old_layout = cached, new_layout = current;
    old_layout[1] &= 0xFFFu; new_layout[1] &= 0xFFFu;
    old_layout[5] &= 0xFFFu; new_layout[5] &= 0xFFFu;
    // Adding/removing a mip allocation changes the host mip count even when
    // word4 still advertises the same maximum level.
    address_only = same_type && old_layout == new_layout &&
        bool(cached[5] >> 12) == bool(current[5] >> 12);
    if (address_only) {
      const u32 sequence = resource->header_sequence.load(std::memory_order_relaxed);
      resource->header_sequence.store(sequence + 1, std::memory_order_relaxed);
      std::atomic_thread_fence(std::memory_order_release);
      auto* words = reinterpret_cast<u32*>(&resource->guest_fetch);
      for (u32 i = 0; i < current.size(); ++i)
        std::atomic_ref<u32>(words[i]).store(current[i], std::memory_order_relaxed);
      resource->header_sequence.store(sequence + 2, std::memory_order_release);
      NoteTextureFootprintChange(guest_address);
      resource->cpu_stamp = {};
      resource->guest_backing_current = false;
      resource->depth_alias_generation = 0;
      // Keep resolved host storage authoritative. Its guest allocation may
      // move, but uploading CPU memory would erase the native resolve result.
      // Preserve an existing CPU upload, but force a new content check by
      // discarding its old memory stamp. The relocation-independent key only
      // reuses it if all base/mip bytes and decoding semantics still match.
    }
  }
  if (!address_only) {
    // The same 52-byte header may now describe another size/format/mip chain.
    // Re-adopt it lazily with fresh storage, retaining old lists at their fences.
    resource->replaced.store(true, std::memory_order_release);
    {
      std::lock_guard lock(g_textures_mutex);
      const auto it = g_textures.find(guest_address);
      if (it == g_textures.end() || it->second != resource) return false;
      g_textures.erase(it);
    }
    InvalidateFramebufferReferences(guest_address);
    HostDevice::UnregisterTexture(resource->descriptor_index);
    HostDevice::UnregisterTexture(resource->resolved_descriptor_index);
    HostDevice::UnregisterTexture(resource->sampled_descriptor_index);
    for (u32 descriptor : resource->previous_surface_descriptors)
      HostDevice::UnregisterTexture(descriptor);
    if (resource->mirror_address)
      REX_KERNEL_MEMORY()->SystemHeapFree(resource->mirror_address);
    HostDevice::RetireResource(resource);
  }
  static std::atomic<u32> refresh_logs{0};
  if (LongProbeEnabled()) LongProbeEvent("texture_header_refresh", !address_only,
      "guest=", guest_address, "action=", address_only ? "relocate" : "replace",
      "host_resolved=", resource->resolved_on_host,
      "cached=", LongProbeHex(cached), "current=", LongProbeHex(current));
  if (refresh_logs.fetch_add(1, std::memory_order_relaxed) < 64)
    REXLOG_INFO("Native GPU: refreshed borrowed texture {:08X} action={} host_resolved={} "
        "base={:08X}->{:08X} mip={:08X}->{:08X} size={:08X}->{:08X}",
        guest_address, address_only ? "relocate" : "replace", resource->resolved_on_host,
        cached[1], current[1], cached[5], current[5], cached[2], current[2]);
  return address_only;
}
}  // namespace

void RefreshTextureHeader(u32 guest_address) {
  RefreshTextureHeaderOwner(guest_address);
}

std::shared_ptr<void> RefreshTextureHeaderOwner(u32 guest_address) {
  const auto resource = FindTexture(guest_address);
  // Surfaces and owned headers have nothing to refresh; an unknown address has no record.
  if (!resource) return {};
  if (resource->owns_guest_memory || resource->surface) return resource;
  {
    // Compare before locking: an unchanged header is the common case per draw.
    D3DTexture metadata{};
    if (!CopyNativeGuestMetadata(*REX_KERNEL_MEMORY(),guest_address,&metadata,sizeof(metadata))) return {};
    const auto* header = &metadata;
    std::array<u32, 6> current{}, cached{};
    for (u32 i = 0; i < current.size(); ++i) current[i] = header->format.dword[i];
    // adopted_header_type is fixed when the record is made; only guest_fetch changes in place.
    const bool same_type = resource->adopted_header_type == (u32(header->resource.common) & 0x1Fu);
    const u32 sequence = resource->header_sequence.load(std::memory_order_acquire);
    bool consistent = false;
    if (!(sequence & 1u)) {
      auto* words = reinterpret_cast<u32*>(&resource->guest_fetch);
      for (u32 i = 0; i < cached.size(); ++i)
        cached[i] = std::atomic_ref<u32>(words[i]).load(std::memory_order_relaxed);
      std::atomic_thread_fence(std::memory_order_acquire);
      consistent = resource->header_sequence.load(std::memory_order_relaxed) == sequence;
    }
    if (!consistent) {
      std::lock_guard lock(resource->mutex);
      std::memcpy(cached.data(), &resource->guest_fetch, sizeof(cached));
    }
    if (cached == current && same_type) return resource;
  }
  auto recording = HostDevice::LockRecording();
  RefreshTextureResourceHeader(guest_address, resource ? resource : FindTexture(guest_address));
  // The record may have been relocated or replaced: the caller looks it up again.
  return {};
}

NativeRenderExtent NativeTextureExtent(u32 guest_address) {
  if (!guest_address) return {};
  decltype(HostDevice::LockRecording()) recording;
  if (!CurrentDrawPreparation() || NativeFsr3Enabled()) recording = HostDevice::LockRecording();
  const auto resource=FindTexture(guest_address);
  return resource ? NativeRenderExtent{resource->LogicalWidth(),resource->LogicalHeight(),resource->HostWidth(),resource->HostHeight()} : NativeRenderExtent{};
}

namespace {
TextureResourceView TextureResourceViewOf(u32 guest_address,
    const std::shared_ptr<TextureResource>& resource) {
  if (!resource) {
    return {};
  }
  std::array<u32, 6> sampling_fetch{};
  const bool has_sampling_fetch = !resource->owns_guest_memory && !resource->surface;
  if (has_sampling_fetch)
    std::memcpy(sampling_fetch.data(), &resource->guest_fetch, sizeof(sampling_fetch));
  if ((resource->resolved_on_host || resource->guest_uploaded) && resource->sampled_valid)
    return {resource->sampled_texture.get(), resource->sampled_view.get(),
            IsDepthFormat(resource->format) ? plume::RenderFormat::R32_FLOAT
                                          : plume::RenderFormat::R32G32B32A32_FLOAT,
            resource->sampled_descriptor_index,
            resource->HostWidth(), resource->HostHeight(), resource->d3d_type, false, false,
            sampling_fetch, has_sampling_fetch};
  const bool native_color = resource->resolved_on_host &&
      resource->resolved_descriptor_index != ~u32{0};
  if (LongProbeEnabled()) {
    const u64 signature = (u64(guest_address) << 32) ^
        (u64(resource->descriptor_index) << 3) ^
        (u64(resource->resolved_descriptor_index) << 11) ^
        (u64(resource->resolved_on_host) << 1) ^ u64(native_color);
    if (!resource->authority_probe_signature_valid ||
        resource->last_authority_probe_signature != signature) {
      resource->last_authority_probe_signature = signature;
      resource->authority_probe_signature_valid = true;
      if (LongProbeOnce(signature ^ 0xC010A07000000000ull))
        LongProbeEvent("texture_color_authority", false, "guest=", guest_address,
            "host_resolved=", resource->resolved_on_host, "logical_rgba=", native_color,
            "raw_descriptor=", resource->descriptor_index,
            "resolved_descriptor=", resource->resolved_descriptor_index,
            "guest_swizzle=", u32(resource->guest_fetch.swizzle),
            "width=", resource->width, "height=", resource->height);
    }
  }
  return {resource->texture.get(),
          native_color ? resource->resolved_view.get() : resource->view.get(),
          resource->format,
          native_color ? resource->resolved_descriptor_index : resource->descriptor_index,
          resource->HostWidth(),
          resource->HostHeight(),
          resource->d3d_type,
          resource->surface,
          IsDepthFormat(resource->format), sampling_fetch, has_sampling_fetch,
          resource->guest_content_hash,
          resource->guest_uploaded && !resource->owns_guest_memory &&
              !resource->surface && !resource->resolved_on_host &&
              !resource->depth_alias_generation && !IsDepthFormat(resource->format)};
}

// A format-6 texture over a resolved depth buffer takes that depth as storage; false on a failed transfer.
bool SettleDepthAlias(u32 guest_address, TextureResource& resource,
    plume::RenderCommandList* commands, bool& aliased) {
  aliased = false;
  if (!DepthAliasesEnabled() || !commands || resource.surface || resource.owns_guest_memory ||
      resource.resolved_on_host || resource.guest_format != 6 ||
      resource.guest_fetch.num_format != 0 || resource.guest_fetch.exp_adjust != 0 ||
      (u32(resource.guest_fetch.sign_x) | u32(resource.guest_fetch.sign_y) |
       u32(resource.guest_fetch.sign_z) | u32(resource.guest_fetch.sign_w))) return true;
  const auto layout = AliasLayout(resource);
  const auto found = g_depth_resolves.find(layout.physical_base);
  const auto depth = found == g_depth_resolves.end() ? nullptr : found->second.lock();
  if (depth && depth->sampled_valid && depth->depth_resolve_generation &&
      CompatibleDepthAlias(layout, AliasLayout(*depth))) {
    if (resource.depth_alias_generation != depth->depth_resolve_generation) {
      if (!ResizeNativeStorage(resource, {resource.width,resource.height,depth->HostWidth(),depth->HostHeight()})) return false;
      if (!HostDevice::TransferDepthAlias(depth->sampled_texture.get(), depth->sampled_descriptor_index,
          resource.texture.get(), resource.HostWidth(), resource.HostHeight(), false)) return false;
      InvalidateTextureUploadReplay(commands, resource.upload_replay);
      resource.prepared_upload.reset();
      resource.depth_alias_generation = depth->depth_resolve_generation;
      // Keep the raw-storage SRV swizzle: this conversion writes packed bytes,
      // unlike a color resolve, which writes logical RGBA values.
      static u32 logs = 0;
      if (logs++ < 24) REXLOG_INFO("Native GPU: depth raw texture alias {:08X}->{:08X} physical={:08X}",
          depth->guest_address, guest_address, layout.physical_base);
    }
    aliased = true;
    return true;
  }
  if (resource.depth_alias_generation) {
    resource.depth_alias_generation = 0;
    resource.guest_uploaded = false;
  }
  return true;
}

bool UploadTextureResourceStorage(u32 guest_address,
    const std::shared_ptr<TextureResource>& resource,
    plume::RenderCommandList* commands, bool require_content_hash) {
  commands = NativeResourceCommands(commands);
  UploadTimer timer;
  // Render targets and native resolves already have authoritative host
  // storage, including resolved depth sampling mirrors. No CPU upload is
  // required; reporting success lets callers reject genuine upload failures.
  if (resource && commands && resource->texture) {
    std::lock_guard lock(resource->mutex);
    RefreshPublishedTextureAuthority(*resource);
    if (resource->surface || resource->resolved_on_host) return true;
  }
  if (resource && resource->guest_format == 22 && IsDepthFormat(resource->format))
    return UploadGuestDepthTexture(*resource, commands, require_content_hash);
  if (resource) {
    bool aliased = false;
    if (!SettleDepthAlias(guest_address, *resource, commands, aliased)) return false;
    if (aliased) return true;
  }
  if (resource && commands && !resource->surface && !resource->resolved_on_host &&
      (resource->HostWidth()!=resource->width || resource->HostHeight()!=resource->height)) {
    if (!ResizeNativeStorage(*resource, {resource->width,resource->height,resource->width,resource->height})) return false;
  }
  if (resource && commands && !resource->owns_guest_memory &&
      !resource->surface && !IsDepthFormat(resource->format)) {
    namespace tu = rex::graphics::texture_util;
    using rex::graphics::xenos::DataDimension;
    std::lock_guard lock(resource->mutex);
    RefreshPublishedTextureAuthority(*resource);
    if (resource->resolved_on_host) return true;
    static const bool audit_texture_watch = std::getenv("LEGO_NATIVE_AUDIT_TEXTURE_WATCH") != nullptr;
    const bool capture_pending = TextureUploadCapturePending(*resource);
    // Vertex textures carry animation and are content-checked on every use, but a hash only needs to
    // repeat after a write: an armed, unchanged watch over the same bytes proves there was none.
    static const bool vertex_watch = [] {
      const char* value = std::getenv("LEGO_NATIVE_VERTEX_TEXTURE_WATCH");
      return !value || value[0] != '0';
    }();
    const bool watch_hit = !capture_pending && (!require_content_hash || vertex_watch) &&
        resource->guest_uploaded && !resource->prepared_upload.expired() &&
        !resource->cpu_stamp.pages.empty() && CpuMemoryUnchanged(resource->cpu_stamp);
    if (watch_hit && !audit_texture_watch) {
      if (QueuePreparedTextureUpload(*resource, commands)) {
        if (timer.enabled) ++g_upload_timing.source_hits;
        return true;
      }
    }
    const auto& fetch = resource->guest_fetch;
    // Cube faces are array slices within each guest mip. A volume mip instead
    // includes all Z slices in one subresource. Stacked 2D arrays remain separate.
    const bool cube = fetch.dimension == DataDimension::kCube;
    const bool volume = fetch.dimension == DataDimension::k3D;
    if ((!cube && !volume && fetch.dimension != DataDimension::k2DOrStacked) ||
        fetch.stacked || !fetch.base_address)
      return false;
    const u32 faces = cube ? 6 : 1;
    const auto* format_info = rex::graphics::FormatInfo::Get(fetch.format);
    const bool alpha4 = fetch.format == rex::graphics::xenos::TextureFormat::k_DXT3A;
    const u32 block_bytes = alpha4 ? 8 : plume::RenderFormatSize(resource->format);
    const u32 block_width = alpha4 ? 4 : plume::RenderFormatBlockWidth(resource->format);
    if (!block_bytes || !block_width ||
        format_info->bits_per_pixel * format_info->block_width * format_info->block_height != block_bytes * 8)
      return false;
    const auto layout = tu::GetGuestTextureLayout(fetch.dimension, fetch.pitch,
        resource->width, resource->height, volume ? resource->depth : 1, fetch.tiled, fetch.format,
        fetch.packed_mips, true, resource->levels - 1);
    const u32 endian_xor=TextureEndianXor(u32(fetch.endianness));
    const u32 base_size=TextureEndianExtent(layout.base.level_data_extent_bytes,endian_xor);
    const u32 mip_size = resource->levels > 1 ? TextureEndianExtent(layout.mips_total_extent_bytes,endian_xor) : 0;
    if(!base_size || (layout.mips_total_extent_bytes && resource->levels>1 && !mip_size)) return false;
    TextureUploadSourceKey source_key{};
    const CpuMemorySpan source_spans[] = {
      {u32(fetch.base_address << 12), base_size},
      {u32(fetch.mip_address << 12), mip_size}};
    auto cpu_stamp = CpuMemoryWatchEnabled() ? WatchCpuMemory(source_spans) : CpuMemoryStamp{};
    const auto source_start = timer.enabled ? std::chrono::steady_clock::now()
                                            : std::chrono::steady_clock::time_point{};
    const bool source_key_valid =
        ReadableUploadSpan(fetch.base_address << 12, base_size) &&
        (!mip_size || (fetch.mip_address && ReadableUploadSpan(fetch.mip_address << 12, mip_size)));
    if (LongProbeEnabled() && !source_key_valid && LongProbeOnce(0xBAD0000000000000ull | guest_address))
      LongProbeEvent("unreadable_texture_source", true, "guest=", guest_address,
          "base=", u32(fetch.base_address<<12), "mip=", u32(fetch.mip_address<<12));
    // Every format must have resident base/mip storage before decoding. The
    // layout extents include tiled padding, cube faces and packed mip tails;
    // a failed hash-span check must never fall through to unchecked CPU reads.
    if (!source_key_valid) return false;
    const auto retained = TextureUploadSource::Capture(fetch.base_address << 12,base_size,
        fetch.mip_address << 12,mip_size,CopyTextureUploadSpan);
    if (!retained) return false;
    const auto* base_bytes = retained->Base().data();
    const auto* mip_bytes = retained->Mips().data();
    if (source_key_valid) {
      const auto hash_start = timer.enabled ? std::chrono::steady_clock::now()
                                            : std::chrono::steady_clock::time_point{};
      source_key = MakeRelocatableTextureUploadSourceKey(
          {base_bytes, base_size}, {mip_bytes, mip_size},
          {reinterpret_cast<const u8*>(&fetch), sizeof(fetch)});
      if (watch_hit && audit_texture_watch) {
        static u64 verified = 0, mismatches = 0;
        ++verified;
        if (!resource->upload_source_key_valid || resource->upload_source_key != source_key) {
          ++mismatches;
          if (mismatches <= 32)
            REXLOG_ERROR("Native texture watch stale: guest={:08X} base={:08X} mip={:08X}",
                guest_address, u32(fetch.base_address << 12), u32(fetch.mip_address << 12));
        }
        if (verified % 100000 == 0)
          REXLOG_INFO("Native texture watch audit: verified={} mismatches={}", verified, mismatches);
      }
      if (timer.enabled) {
        g_upload_timing.hash_ms += std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - hash_start).count();
        g_upload_timing.hashed_bytes += u64(base_size) + mip_size;
        g_upload_timing.source_ms += std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - source_start).count();
      }
      if (!capture_pending && resource->guest_uploaded && resource->upload_source_key_valid &&
          resource->upload_source_key == source_key && QueuePreparedTextureUpload(*resource, commands)) {
        if (timer.enabled) ++g_upload_timing.source_hits;
        resource->cpu_stamp = std::move(cpu_stamp);
        return true;
      }
    }
    resource->upload_source_key_valid = false;
    timer.slow.Mark("source-validation-hash");
    u64 upload_size_wide = 0;
    for (u32 face = 0; face < faces; ++face) {
      for (u32 level = 0; level < resource->levels; ++level)
        upload_size_wide = ((upload_size_wide + 511) & ~u64{511}) +
                           LevelSize(*resource, level);
    }
    if (!upload_size_wide || upload_size_wide > UINT32_MAX) return false;
    const u32 upload_size = static_cast<u32>(upload_size_wide);
    // Untiling, diagnostics and the decoded-content hash need CPU reads.
    // Keep those in normal RAM; UPLOAD memory must be a write-only destination.
    std::vector<u8> decoded(upload_size);
    auto* mapped = decoded.data();
    u32 destination_offset = 0;
    for (u32 subresource = 0; subresource < faces * resource->levels; ++subresource) {
      const u32 face = subresource / resource->levels;
      const u32 level = subresource % resource->levels;
      destination_offset = AlignUp(destination_offset, 512);
      const u32 packed_level = std::min(level, layout.packed_level);
      const auto& guest_level = level == 0 ? layout.base : layout.mips[packed_level];
      const u64 base_wide = (level == 0 ? u64(u32(fetch.base_address) << 12)
          : u64(u32(fetch.mip_address) << 12) + layout.mip_offsets_bytes[packed_level]) +
          u64(face) * guest_level.array_slice_stride_bytes;
      if (base_wide > UINT32_MAX) return false;
      const u32 base = u32(base_wide);
      // D3D resource headers retain the CPU allocation address, unlike PM4
      // fetch constants. In particular, the E/F physical aliases include a
      // 4 KB bias in Memory::TranslateVirtual/GetPhysicalAddress. Masking them
      // with TranslatePhysical reads the preceding page and scrambles tiles.
      // Keep support for headers that already contain a physical address.
      const auto source_bytes = retained->Slice(level != 0,base,
          TextureEndianExtent(guest_level.array_slice_data_extent_bytes,endian_xor));
      if (source_bytes.empty()) return false;
      const auto* source = source_bytes.data();
      u32 offset_x = 0, offset_y = 0, offset_z = 0;
      if (fetch.packed_mips)
        tu::GetPackedMipOffset(resource->width, resource->height,
                               volume ? resource->depth : 1, fetch.format,
                               level, offset_x, offset_y, offset_z);
      const u32 width = MipDimension(resource->width, level);
      const u32 height = MipDimension(resource->height, level);
      const u32 pitch = RowPitch(resource->format, width);
      if (alpha4) {
        const bool copied = CopyTextureAlpha4Blocks(
            {source, TextureEndianExtent(guest_level.array_slice_data_extent_bytes,endian_xor)},
            {mapped + destination_offset, LevelSize(*resource, level)},
            width, height, volume ? MipDimension(resource->depth, level) : 1,
            pitch, endian_xor, [&](u32 x, u32 y, u32 z) -> int64_t {
              if (fetch.tiled) {
                if (volume) return tu::GetTiledOffset3D(x + offset_x, y + offset_y, z + offset_z,
                    guest_level.row_pitch_bytes / 8, guest_level.z_slice_stride_block_rows, 3);
                return tu::GetTiledOffset2D(x + offset_x, y + offset_y,
                    guest_level.row_pitch_bytes / 8, 3);
              }
              return (u64(z + offset_z) * guest_level.z_slice_stride_block_rows + y + offset_y) *
                  guest_level.row_pitch_bytes + u64(x + offset_x) * 8;
            });
        if (!copied) return false;
        destination_offset += LevelSize(*resource, level);
        continue;
      }
      if (volume) {
        const u32 block_height = format_info->block_height;
        const bool copied = CopyTextureVolumeBlocks(
            {source, TextureEndianExtent(guest_level.array_slice_data_extent_bytes,endian_xor)},
            {mapped + destination_offset, LevelSize(*resource, level)},
            (width + block_width - 1) / block_width,
            (height + block_height - 1) / block_height,
            MipDimension(resource->depth, level), block_bytes, pitch, endian_xor,
            [&](u32 x, u32 y, u32 z) -> int64_t {
              if (fetch.tiled)
                return tu::GetTiledOffset3D(x + offset_x, y + offset_y, z + offset_z,
                    guest_level.row_pitch_bytes / block_bytes,
                    guest_level.z_slice_stride_block_rows, rex::log2_floor(block_bytes));
              return (u64(z + offset_z) * guest_level.z_slice_stride_block_rows +
                      y + offset_y) * guest_level.row_pitch_bytes +
                     u64(x + offset_x) * block_bytes;
            });
        if (!copied) return false;
        destination_offset += LevelSize(*resource, level);
        continue;
      }
      // The checked block copier also handles one 2D/cube slice. Validate each
      // computed address, including endian XOR and packed-mip offsets, against
      // the same guest extent whose residency was checked above.
      const bool copied = CopyTextureVolumeBlocks(
          {source, TextureEndianExtent(guest_level.array_slice_data_extent_bytes,endian_xor)},
          {mapped + destination_offset, LevelSize(*resource, level)},
          (width + block_width - 1) / block_width,
          (height + format_info->block_height - 1) / format_info->block_height,
          1, block_bytes, pitch, endian_xor,
          [&](u32 x, u32 y, u32) -> int64_t {
            if (fetch.tiled)
              return tu::GetTiledOffset2D(x + offset_x, y + offset_y,
                  guest_level.row_pitch_bytes / block_bytes, rex::log2_floor(block_bytes));
            return u64(y + offset_y) * guest_level.row_pitch_bytes +
                   u64(x + offset_x) * block_bytes;
          });
      if (!copied) return false;
      destination_offset += LevelSize(*resource, level);
    }
    DumpTextureUpload(*resource, mapped,
        source_key_valid ? std::span<const u8>(base_bytes, base_size) : std::span<const u8>{},
        source_key_valid ? std::span<const u8>(mip_bytes, mip_size) : std::span<const u8>{});
    if (timer.enabled) g_upload_timing.converted_bytes += upload_size;
    const u64 content_hash = XXH3_64bits(mapped, upload_size);
    timer.slow.Mark("decode-content-hash");
    if (LongProbeEnabled() && (!resource->guest_uploaded || resource->guest_content_hash != content_hash))
      LongProbeEvent("texture_upload_changed", false, "guest=", guest_address,
          "width=", resource->width, "height=", resource->height,
          "hash=", content_hash, "previous_hash=", resource->guest_content_hash,
          "bytes=", upload_size, "base=", u32(fetch.base_address<<12),
          "mip=", u32(fetch.mip_address<<12));
    if (resource->guest_uploaded && resource->guest_content_hash == content_hash &&
        QueuePreparedTextureUpload(*resource, commands)) {
      resource->upload_source_key = source_key;
      resource->upload_source_key_valid = source_key_valid;
      resource->cpu_stamp = std::move(cpu_stamp);
      return true;
    }
    auto upload = std::shared_ptr<plume::RenderBuffer>(HostDevice::Device()->createBuffer(
        plume::RenderBufferDesc::UploadBuffer(upload_size)).release());
    if (!upload) return false;
    auto* upload_bytes = upload->map();
    if (!upload_bytes) return false;
    std::memcpy(upload_bytes, decoded.data(), decoded.size());
    upload->unmap();
    timer.slow.Mark("allocate-map-stage");
    auto copy = CreateNativeDrawCommands();
    copy->barriers(plume::RenderBarrierStage::COPY,
        plume::RenderTextureBarrier(resource->texture.get(), plume::RenderTextureLayout::COPY_DEST));
    destination_offset = 0;
    // Copy footprints describe decoded host storage, not the guest blocks
    // (DXT3A expands eight-byte blocks into individual R8 pixels).
    const u32 host_block_bytes = plume::RenderFormatSize(resource->format);
    const u32 host_block_width = plume::RenderFormatBlockWidth(resource->format);
    for (u32 subresource = 0; subresource < faces * resource->levels; ++subresource) {
      const u32 face = subresource / resource->levels;
      const u32 level = subresource % resource->levels;
      destination_offset = AlignUp(destination_offset, 512);
      const u32 width = MipDimension(resource->width, level);
      const u32 height = MipDimension(resource->height, level);
      copy->copyTextureRegion(
          plume::RenderTextureCopyLocation::Subresource(resource->texture.get(), level, face),
          plume::RenderTextureCopyLocation::PlacedFootprint(upload.get(), resource->format,
              width, height, volume ? MipDimension(resource->depth, level) : 1,
              RowPitch(resource->format, width) / host_block_bytes * host_block_width,
              destination_offset));
      destination_offset += LevelSize(*resource, level);
    }
    copy->barriers(plume::RenderBarrierStage::GRAPHICS,
        plume::RenderTextureBarrier(resource->texture.get(), plume::RenderTextureLayout::SHADER_READ));
    PublishPreparedTextureUpload(*resource, commands, std::move(copy), std::move(upload), upload_size);
    timer.slow.Mark("record-copy-retire");
    resource->guest_uploaded = true;
    resource->guest_content_hash = content_hash;
    resource->upload_source_key = source_key;
    resource->upload_source_key_valid = source_key_valid;
    resource->cpu_stamp = std::move(cpu_stamp);
    if (volume) {
      static std::atomic<u32> volume_uploads{0};
      if (volume_uploads.fetch_add(1) < 8)
        REXLOG_INFO("Native GPU: uploaded volume 0x{:08X} {}x{}x{} levels={} bytes={} hash={:016X}",
                    guest_address, resource->width, resource->height, resource->depth,
                    resource->levels, upload_size, content_hash);
    }
    return true;
  }
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
  const auto retained = TextureUploadSource::Capture(resource->mirror_address,resource->mirror_size,
      0,0,CopyTextureUploadSpan);
  if (!retained) return false;
  const auto* source = retained->Base().data();
  u64 destination_offset = 0;
  for (u32 level = 0; level < resource->levels; ++level) {
    destination_offset = AlignUp(static_cast<u32>(destination_offset), 0x200);
    const u32 level_size = LevelSize(*resource, level);
    if (u64(LevelOffset(*resource,level)) + level_size > retained->Base().size()) return false;
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

}  // namespace

TextureResourceView ResolveTextureResource(u32 guest_address) {
  if (!guest_address) return {};
  if (CurrentDrawPreparation() && !NativeFsr3Enabled()) {
    const auto resource = FindTexture(guest_address);
    if (resource && resource->surface) return TextureResourceViewOf(guest_address, resource);
  }
  auto recording = HostDevice::LockRecording();
  const auto resource=AdoptTexture(guest_address);
  if (!resource || resource->surface) return TextureResourceViewOf(guest_address,resource);
  // Front-buffer and direct-view callers must observe the same CPU authority
  // checks as sampled bindings, including a record invalidated by a prior use.
  TextureResourceView prepared;
  return PrepareTextureResource(guest_address,HostDevice::BeginFrameCommands(),false,prepared)==TexturePreparationResult::kReady
      ? prepared : TextureResourceView{};
}

TexturePreparationSnapshot CaptureTexturePreparation(u32 address) {
  return CaptureTexturePreparation(address, nullptr);
}

TexturePreparationSnapshot CaptureTexturePreparation(u32 address, std::shared_ptr<void> resolved) {
  if (!address) return {};
  TexturePreparationSnapshot snapshot;
  // A record found by the header refresh of this same draw is what FindTexture would return now.
  if (resolved && !std::static_pointer_cast<TextureResource>(resolved)->replaced.load(std::memory_order_acquire))
    snapshot.owner = std::move(resolved);
  else
    snapshot.owner = FindTexture(address);
  if (!snapshot.owner) { snapshot.footprint.unknown = true; return snapshot; }
  if (DepthAliasesEnabled()) {
    auto& resource = *std::static_pointer_cast<TextureResource>(snapshot.owner);
    if (resource.guest_format == 6 && !resource.owns_guest_memory && !resource.surface) {
      bool aliased = false;
      // In order on the capture thread under the registry half: the transfer records into this draw's tape.
      snapshot.alias_settled = SettleDepthAlias(address, resource, NativeResourceCommands(nullptr), aliased);
    }
  }
  auto& resource = *std::static_pointer_cast<TextureResource>(snapshot.owner);
  if (resource.owns_guest_memory || resource.surface) {
    snapshot.footprint = TexturePreparationFootprintOf(resource, nullptr);
    return snapshot;
  }
  D3DTexture metadata{};
  if (!CopyNativeGuestMetadata(*REX_KERNEL_MEMORY(),address,&metadata,sizeof(metadata))) {
    snapshot.owner.reset(); snapshot.footprint.unknown = true; return snapshot;
  }
  const auto* header = &metadata;
  snapshot.header[0] = u32(header->resource.common) & 0x1Fu;
  for (u32 i = 0; i < 6; ++i) snapshot.header[i + 1] = header->format.dword[i];
  snapshot.footprint = TexturePreparationFootprintOf(resource, &snapshot.header);
  return snapshot;
}

TextureResourceView PrepareCachedTexture(const TexturePreparationSnapshot& snapshot) {
  const auto resource = std::static_pointer_cast<TextureResource>(snapshot.owner);
  static const bool audit = std::getenv("LEGO_NATIVE_AUDIT_TEXTURE_WATCH") != nullptr;
  if (!resource || LongProbeEnabled() || audit) return {};
  std::lock_guard lock(resource->mutex);
  RefreshPublishedTextureAuthority(*resource);
  // Surfaces and host resolves own authoritative storage; the resolve wait keeps the record still.
  if (resource->texture && (resource->surface || resource->resolved_on_host)) {
    const auto view = TextureResourceViewOf(resource->guest_address, resource);
    if ((!resource->owns_guest_memory && (resource->adopted_header_type != snapshot.header[0] ||
        std::memcmp(&resource->guest_fetch, snapshot.header.data() + 1, sizeof(resource->guest_fetch)))) ||
        !view.texture || view.descriptor_index == ~u32{0}) return {};
    return view;
  }
  if (resource->owns_guest_memory || resource->depth_alias_generation || IsDepthFormat(resource->format) ||
      (DepthAliasesEnabled() && resource->guest_format == 6 && !snapshot.alias_settled) ||
      !resource->guest_uploaded || resource->adopted_header_type != snapshot.header[0] ||
      std::memcmp(&resource->guest_fetch, snapshot.header.data() + 1, sizeof(resource->guest_fetch)) ||
      !CpuMemoryUnchanged(resource->cpu_stamp) ||
      !QueuePreparedTextureUpload(*resource, NativeResourceCommands(nullptr))) return {};
  return TextureResourceViewOf(resource->guest_address, resource);
}

bool UploadTextureResource(u32 guest_address,
    plume::RenderCommandList* commands, bool require_content_hash) {
  auto recording = HostDevice::LockRecording();
  return UploadTextureResourceStorage(guest_address,FindTexture(guest_address),
      commands,require_content_hash);
}

bool UnlockedTextureUploadEnabled() {
  static const bool enabled = [] {
    const char* value = std::getenv("LEGO_NATIVE_UNLOCKED_TEXTURE_UPLOAD");
    return !value || value[0] != '0';
  }();
  return enabled;
}

// An upload that touches only its own record: no host-owned or resolved storage, no depth alias,
// no resize, and no diagnostics that share counters under the lock.
bool RegistryFreeTextureUpload(const TextureResource& resource) {
  return resource.texture && !resource.surface && !resource.resolved_on_host && !resource.owns_guest_memory &&
      !IsDepthFormat(resource.format) && resource.guest_format != 22 &&
      !(DepthAliasesEnabled() && resource.guest_format == 6) && !resource.depth_alias_generation &&
      resource.HostWidth() == resource.width && resource.HostHeight() == resource.height &&
      !NativeTextureTimingEnabled() && !TextureUploadCapturePending(resource) &&
      !std::getenv("LEGO_NATIVE_AUDIT_TEXTURE_WATCH");
}

TexturePreparationResult PrepareTextureResource(u32 guest_address,
    plume::RenderCommandList* commands, bool require_content_hash,
    TextureResourceView& prepared) {
  prepared={};
  auto recording=HostDevice::LockRecording();
  auto resource=AdoptTexture(guest_address);
  const auto initial=TextureResourceViewOf(guest_address,resource);
  if (!initial.texture || initial.descriptor_index==~u32{0})
    return TexturePreparationResult::kUnavailable;
  // A CPU streaming writer can publish a placement header without SetTexture,
  // including while an upload is decoding its previous backing. Never publish
  // a newly adopted SRV unless that exact placement has completed preparation.
  for (u32 attempt=0; attempt<3; ++attempt) {
    const auto fetch_before=resource->guest_fetch;
    // Decoding and hashing a plain CPU texture needs only its own mutex: the record stays alive
    // through the shared pointer, and a replacement or relocation meanwhile is caught below.
    const bool release=UnlockedTextureUploadEnabled() && CurrentDrawPreparation() &&
        NativeRecordingGate::RegistryDepth()==1 && RegistryFreeTextureUpload(*resource);
    if (release) recording.unlock();
    const bool uploaded=UploadTextureResourceStorage(guest_address,resource,commands,require_content_hash);
    if (release) recording.lock();
    if (!uploaded)
      return TexturePreparationResult::kUploadFailed;
    if (!RefreshTextureResourceHeader(guest_address,resource)) {
      LongProbeEvent("texture_prepare_retry",false,"guest=",guest_address,
          "attempt=",attempt+1,"reason=replace");
      resource=AdoptTexture(guest_address);
      if (!resource) return TexturePreparationResult::kUploadFailed;
      continue;
    }
    if (std::memcmp(&fetch_before,&resource->guest_fetch,sizeof(fetch_before))) {
      LongProbeEvent("texture_prepare_retry",false,"guest=",guest_address,
          "attempt=",attempt+1,"reason=relocate");
      continue; // Same owner, but its base/mip allocation relocated.
    }
    const auto view=TextureResourceViewOf(guest_address,resource);
    if (!view.texture || view.descriptor_index==~u32{0})
      return TexturePreparationResult::kUploadFailed;
    prepared=view;
    return TexturePreparationResult::kReady;
  }
  LongProbeEvent("texture_prepare_unstable",true,"guest=",guest_address);
  return TexturePreparationResult::kUploadFailed;
}

TexturePreparationResult PrepareTextureBinding(u32 slot, u32 guest_address,
    const std::array<u32, 6>& fetch, u32 overrides,
    plume::RenderCommandList* commands, bool require_content_hash,
    TextureResourceView& prepared) {
  std::unique_lock<NativeRecordingGate> recording;
  static const bool enabled = [] {
    const char* value = std::getenv("LEGO_NATIVE_TEXTURE_BINDING_CACHE");
    return value && value[0] == '1' && !std::getenv("LEGO_NATIVE_AUDIT_TEXTURE_WATCH");
  }();
  struct Entry {
    u32 address = 0, overrides = 0;
    u64 generation = 0, epoch = 0;
    std::array<u32, 6> fetch{};
    std::array<u32, 7> header{};
    std::weak_ptr<TextureResource> owner;
    TextureResourceView view;
  };
  static thread_local std::array<Entry, kNativeTextureSlots> entries;
  // Vertex-shader textures request the content hash and also need a state legal for non-pixel reads.
  if (require_content_hash) {
    // A draw preparation locks inside PrepareTextureResource, which releases the registry
    // around a plain upload; holding it here would keep the hash under the lock.
    if (!UnlockedTextureUploadEnabled() || !CurrentDrawPreparation()) recording = HostDevice::LockRecording();
    const auto result = PrepareTextureResource(guest_address, commands, true, prepared);
    static const bool vertex_read_state = [] {
      const char* value = std::getenv("LEGO_NATIVE_VERTEX_TEXTURE_READ_STATE");
      const bool enabled = value && value[0] != '0';
      REXLOG_INFO("Native GPU: vertex texture non-pixel read state={}", enabled);
      return enabled;
    }();
    if (vertex_read_state && result == TexturePreparationResult::kReady && commands && prepared.texture)
      PrepareVertexTextureShaderRead(commands, prepared.texture);
    return result;
  }
  if (!enabled || slot >= entries.size() || !guest_address || LongProbeEnabled())
    return PrepareTextureResource(guest_address, commands, require_content_hash, prepared);
  auto& entry = entries[slot];
  D3DTexture metadata{};
  if (!CopyNativeGuestMetadata(*REX_KERNEL_MEMORY(),guest_address,&metadata,sizeof(metadata)))
    return TexturePreparationResult::kUnavailable;
  const auto* header = &metadata;
  std::array<u32, 7> words;
  words[0] = u32(header->resource.common) & 0x1Fu;
  for (u32 i = 0; i < 6; ++i) words[i + 1] = header->format.dword[i];
  const auto epoch = CpuMemoryWriteEpoch();
  const auto generation = g_textures.BucketVersion(guest_address);
  if (entry.address == guest_address && entry.fetch == fetch && entry.overrides == overrides &&
      entry.header == words && entry.generation == generation) {
    if (const auto owner = entry.owner.lock()) {
      std::lock_guard owner_lock(owner->mutex);
      if (owner->guest_uploaded &&
        !owner->resolved_on_host && !owner->depth_alias_generation &&
        owner->guest_content_hash == entry.view.content_hash &&
        std::memcmp(&owner->guest_fetch, words.data() + 1, sizeof(owner->guest_fetch)) == 0 &&
        owner->texture.get() == entry.view.texture && owner->descriptor_index == entry.view.descriptor_index &&
        ((epoch && entry.epoch == epoch) || CpuMemoryUnchanged(owner->cpu_stamp)) &&
        QueuePreparedTextureUpload(*owner, NativeResourceCommands(commands))) {
        entry.epoch = epoch;
        prepared = entry.view;
        return TexturePreparationResult::kReady;
      }
    }
  }
  entry = {};
  if (!UnlockedTextureUploadEnabled() || !CurrentDrawPreparation()) recording = HostDevice::LockRecording();
  const auto result = PrepareTextureResource(guest_address, commands, false, prepared);
  if (result != TexturePreparationResult::kReady) return result;
  const auto owner = FindTexture(guest_address);
  // Depth-capable aliases and host-owned storage retain their ordered preparation path.
  if (owner && !owner->owns_guest_memory && !owner->surface && !owner->resolved_on_host &&
      !owner->depth_alias_generation && !IsDepthFormat(owner->format) &&
      !(DepthAliasesEnabled() && owner->guest_format == 6) && owner->guest_uploaded &&
      std::memcmp(&owner->guest_fetch, words.data() + 1, sizeof(owner->guest_fetch)) == 0 &&
      CpuMemoryUnchanged(owner->cpu_stamp))
    entry = {guest_address, overrides, generation, epoch, fetch, words, owner, prepared};
  return result;
}

bool ResolveTextureFromSurface(u32 destination_texture, u32 source_surface,
                               u32 destination_level,
                               u32 destination_slice, u32 resolve_flags,
                               const ResolveRect* rectangle, const ResolvePoint* point) {
  auto recording = HostDevice::LockRecording();
  if (!PrepareSurfaceColorAlias(source_surface) ||
      !PrepareSurfaceDepthAlias(source_surface)) return false;
  RefreshTextureHeader(destination_texture);
  const auto destination = FindTexture(destination_texture);
  const auto adopted_destination =
      destination ? destination : AdoptTexture(destination_texture);
  const auto source = FindTexture(source_surface);
    if (source && adopted_destination && source->LogicalWidth()==1280 &&
        source->LogicalHeight()==720 &&
      adopted_destination->width==1280 && adopted_destination->height==720 &&
      adopted_destination->d3d_type==u32(D3DResourceType::kTexture)) {
    if (!ResizeNativeStorage(*adopted_destination,
        {1280,720,source->HostWidth(),source->HostHeight()})) return false;
  }
  auto region = source && adopted_destination &&
      destination_level < adopted_destination->levels ? CheckedResolveRegion(
      source->LogicalWidth(), source->LogicalHeight(),
      MipDimension(adopted_destination->width, destination_level),
      MipDimension(adopted_destination->height, destination_level), rectangle, point)
      : std::nullopt;
  ResolveRegion source_area{};
  if (region) region=NativePhysicalResolve(*region,
      {source->LogicalWidth(),source->LogicalHeight(),source->HostWidth(),source->HostHeight()},
      {MipDimension(adopted_destination->width,destination_level),MipDimension(adopted_destination->height,destination_level),
       MipDimension(adopted_destination->HostWidth(),destination_level),MipDimension(adopted_destination->HostHeight(),destination_level)},&source_area);
  if (LongProbeEnabled()) {
    const std::array<u32,12> signature{source_surface,destination_texture,resolve_flags,
        destination_level,destination_slice,region ? region->left : 0,
        region ? region->top : 0,region ? region->width : 0,region ? region->height : 0,
        region ? region->x : 0,region ? region->y : 0,source ? source->guest_format : 0};
    if (HostDevice::LongProbeSnapshotActive() || LongProbeOnce(XXH3_64bits(signature.data(),sizeof(signature))))
      LongProbeEvent("resolve", !region || !source || !adopted_destination,
          "signature_src_dst_flags_level_slice_rect_point_format=", LongProbeHex(signature),
          "dst_width=", adopted_destination ? adopted_destination->width : 0,
          "dst_height=", adopted_destination ? adopted_destination->height : 0);
  }
  if (source && adopted_destination && !region) {
    static std::atomic<u32> bad_regions{0};
    if (bad_regions.fetch_add(1) < 16)
      REXLOG_WARN("Native GPU: rejected invalid resolve region src={:08X} dst={:08X}",
                  source_surface, destination_texture);
    return false;
  }
  if (region && (source_area.width!=region->width || source_area.height!=region->height)) {
    // Downsample scene-scaled base writes without changing smaller mip storage.
    if (!source->surface || adopted_destination->surface || destination_level ||
        destination_slice || !source->texture || !adopted_destination->texture ||
        source->format!=plume::RenderFormat::R8G8B8A8_UNORM ||
        adopted_destination->format!=source->format ||
        adopted_destination->d3d_type!=u32(D3DResourceType::kTexture) ||
        (resolve_flags & 0xFC000000u)) return false;
    if (!HostDevice::ResolveHdrColor(source->texture.get(),source->descriptor_index,
        adopted_destination->texture.get(),adopted_destination->HostWidth(),
        adopted_destination->HostHeight(),1.0f,ColorResolveDestination::kScaledRGBA8,
        true,&*region,&source_area)) return false;
    RecordResolvedTexture(*adopted_destination, *region, destination_level, destination_slice);
    return UpdateResolvedSampling(*adopted_destination);
  }
  if (!adopted_destination) {
    static std::atomic<u32> unknown_resolves{0};
    if (unknown_resolves.fetch_add(1) < 4)
      LogUnknownTexture(destination_texture, "resolve destination");
  }
  if (source && source->texture && !IsDepthFormat(source->format)) {
    HostDevice::SnapshotTexture(source->texture.get(),
        "resolve-" + std::to_string(source_surface) + "-to-" +
        std::to_string(destination_texture), true);
  }
  // Xenos k_24_8 sampling returns normalized depth in X. Keep each atlas tile
  // in a float sampling mirror. Stencil and CPU readback remain unsupported.
  if (source && adopted_destination && region && source->surface &&
      !adopted_destination->surface && source->texture &&
      IsDepthFormat(source->format) && IsDepthFormat(adopted_destination->format) &&
      (adopted_destination->guest_format & 0x3Fu) == 22 &&
      destination_level == 0 && destination_slice == 0 &&
      adopted_destination->levels == 1 &&
      adopted_destination->d3d_type == u32(D3DResourceType::kTexture) &&
      !(resolve_flags & 0xFC000000u) &&
      (adopted_destination->guest_fetch.swizzle == 0xB48 ||
       adopted_destination->guest_fetch.swizzle == 0x688)) {
    auto& dst = *adopted_destination;
    // CPU-first and GPU-first sampling must compose the same Xenos depth
    // channel expansion with the guest swizzle. Publish storage/view/descriptor
    // together so an allocation failure can be retried safely.
    if (!EnsureDepthSamplingMirror(dst)) return false;
    if (!HostDevice::ResolveHdrColor(source->texture.get(), source->descriptor_index,
        dst.sampled_texture.get(), dst.HostWidth(), dst.HostHeight(), 1.0f,
        ColorResolveDestination::kDepthFloat32, true, &*region)) return false;
    dst.resolved_on_host = true; dst.guest_backing_current=false; dst.sampled_valid = true;
    if (DepthAliasesEnabled()) {
      dst.depth_resolve_generation = ++g_depth_alias_generation;
      g_depth_resolves[AliasLayout(dst).physical_base] = adopted_destination;
    }
    HostDevice::SnapshotTexture(dst.sampled_texture.get(),
        "depth-atlas-" + std::to_string(destination_texture), false);
    static std::unordered_set<u64> logged_depth_regions;
    const std::array<u32, 8> depth_region{source->guest_address, destination_texture,
        region->left, region->top, region->width, region->height, region->x, region->y};
    if (logged_depth_regions.size() < 32 && logged_depth_regions.insert(
        XXH3_64bits(depth_region.data(), sizeof(depth_region))).second)
      REXLOG_INFO("Native GPU: depth atlas src=({},{}) {}x{} dst=({},{}) texture={:08X}",
          region->left, region->top, region->width, region->height,
          region->x, region->y, destination_texture);
    return true;
  }
  // Preserve both host depth and stencil planes. Unlike a color subrect copy,
  // CopyResource requires identical full resource descriptions. It does NOT
  // implement Xenos 24-bit quantization or guest-memory writeback.
  if (source && adopted_destination && source->surface &&
      !adopted_destination->surface && source->texture && adopted_destination->texture &&
      source->texture.get() != adopted_destination->texture.get() &&
      IsDepthFormat(source->format) && source->format == adopted_destination->format &&
      destination_level == 0 && destination_slice == 0 &&
      !(resolve_flags & 0xFC000000u) && !rectangle && !point) {
    auto* src = static_cast<plume::D3D12Texture*>(source->texture.get())->d3d;
    auto* dst = static_cast<plume::D3D12Texture*>(adopted_destination->texture.get())->d3d;
    if (src && dst) {
      const auto a = src->GetDesc();
      const auto b = dst->GetDesc();
      if (a.Dimension == b.Dimension && a.Width == b.Width && a.Height == b.Height &&
          a.DepthOrArraySize == 1 && b.DepthOrArraySize == 1 &&
          a.MipLevels == 1 && b.MipLevels == 1 && a.Format == b.Format &&
          a.SampleDesc.Count == 1 && b.SampleDesc.Count == 1 &&
          a.SampleDesc.Quality == b.SampleDesc.Quality) {
        auto* commands = HostDevice::BeginFrameCommands();
        if (!commands) return false;
        commands->setFramebuffer(nullptr);
        const plume::RenderTextureBarrier before[] = {
          {source->texture.get(), plume::RenderTextureLayout::COPY_SOURCE},
          {adopted_destination->texture.get(), plume::RenderTextureLayout::COPY_DEST}};
        commands->barriers(plume::RenderBarrierStage::COPY, before, 2);
        RecordNativeCommand(commands, [dst, src](auto* list) {
          static_cast<plume::D3D12CommandList*>(list)->d3d->CopyResource(dst, src);
        });
        const plume::RenderTextureBarrier after[] = {
          {source->texture.get(), plume::RenderTextureLayout::DEPTH_WRITE},
          {adopted_destination->texture.get(), plume::RenderTextureLayout::SHADER_READ}};
        commands->barriers(plume::RenderBarrierStage::GRAPHICS, after, 2);
        RecordResolvedTexture(*adopted_destination, *region, destination_level, destination_slice);
        static std::atomic<u32> depth_copies{0};
        if (depth_copies.fetch_add(1) < 8)
          REXLOG_INFO("Native GPU: copied host depth+stencil {}x{} dst=0x{:08X} srv={}",
                      source->HostWidth(), source->HostHeight(), destination_texture,
                      adopted_destination->descriptor_index);
        return true;
      }
    }
  }
  if (source && adopted_destination && source->surface &&
      !adopted_destination->surface && destination_level == 0 && destination_slice == 0 &&
      source->texture && adopted_destination->texture &&
      source->format == plume::RenderFormat::R16G16B16A16_SNORM &&
      adopted_destination->format == plume::RenderFormat::R16G16B16A16_UNORM) {
    // Host fixed16 storage is value/32 in SNORM. Resolve converts values,
    // including its signed 6-bit exponent, into the destination number format.
    // Copying SNORM bits into UNORM would also corrupt negative values.
    const auto& fetch = adopted_destination->guest_fetch;
    const float scale = 32.0f * Unsigned16ResolveScale(
        static_cast<i32>(resolve_flags) >> 26, IntegerResolvePacking(fetch.num_format != 0));
    if (!HostDevice::ResolveHdrColor(source->texture.get(), source->descriptor_index,
            adopted_destination->texture.get(), adopted_destination->HostWidth(),
            adopted_destination->HostHeight(), scale, ColorResolveDestination::kUnormRGBA16,
            true, &*region))
      return false;
    RecordResolvedTexture(*adopted_destination, *region, destination_level, destination_slice);
    if (!UpdateResolvedSampling(*adopted_destination)) return false;
    static std::atomic<u32> logs{0};
    if (logs.fetch_add(1) < 8)
      REXLOG_INFO("Native GPU: fixed16 resolve SNORM->UNORM flags={:08X} scale={} dst={:08X}",
          resolve_flags, scale, destination_texture);
    return true;
  }
  if (source && adopted_destination && source->surface &&
      !adopted_destination->surface && destination_level == 0 &&
      destination_slice == 0 && source->texture && adopted_destination->texture &&
      adopted_destination->d3d_type == static_cast<u32>(D3DResourceType::kTexture) &&
      ((source->format == plume::RenderFormat::R16G16B16A16_FLOAT &&
        adopted_destination->format == plume::RenderFormat::R16G16B16A16_UNORM) ||
       (source->format == plume::RenderFormat::R16G16_FLOAT &&
        adopted_destination->format == plume::RenderFormat::R16G16_UNORM))) {
    const auto& fetch = adopted_destination->guest_fetch;
    // UINT16 resolve writes a clamped integer, not a normalized fraction.
    // Our raw UNORM16 allocation represents that integer divided by 65535.
    const float scale = Unsigned16ResolveScale(static_cast<i32>(resolve_flags) >> 26,
                                              IntegerResolvePacking(fetch.num_format != 0));
    if (!HostDevice::ResolveHdrColor(source->texture.get(), source->descriptor_index,
                                    adopted_destination->texture.get(),
                                    adopted_destination->HostWidth(), adopted_destination->HostHeight(), scale,
                                    adopted_destination->format == plume::RenderFormat::R16G16_UNORM
                                        ? ColorResolveDestination::kUnormRG16
                                        : ColorResolveDestination::kUnormRGBA16,
                                    true, &*region)) return false;
    RecordResolvedTexture(*adopted_destination, *region, destination_level, destination_slice);
    if (!UpdateResolvedSampling(*adopted_destination)) return false;
    static std::atomic<u32> conversions{0};
    if (conversions.fetch_add(1) < 8)
      REXLOG_INFO("Native GPU: HDR resolve FLOAT->UNORM {}x{} scale={}",
                  source->HostWidth(), source->HostHeight(), scale);
    return true;
  }
  const char* failure = nullptr;
  if (!adopted_destination || !source) failure = "unregistered resource";
  else if (!adopted_destination->texture || !source->texture)
    failure = "missing host texture";
  else if (!source->surface || adopted_destination->surface)
    failure = "invalid resource kind";
  else if (destination_level >= adopted_destination->levels)
    failure = "mip out of range";
  else if (IsDepthFormat(source->format) || IsDepthFormat(adopted_destination->format))
    failure = "depth conversion not implemented";
  else if (adopted_destination->format != source->format)
    failure = "format conversion not implemented";
  if (failure) {
    static std::atomic<u32> failures{0};
    if (failures.fetch_add(1) < 32) {
      REXLOG_WARN("Native GPU: resolve rejected ({}) src=0x{:08X} {}x{} fmt={} "
                  "dst=0x{:08X} {}x{} fmt={} mip={} slice={}", failure,
                  source_surface, source ? source->HostWidth() : 0,
                  source ? source->HostHeight() : 0, source ? u32(source->format) : 0,
                  destination_texture, adopted_destination ? adopted_destination->width : 0,
                  adopted_destination ? adopted_destination->height : 0,
                  adopted_destination ? u32(adopted_destination->format) : 0,
                  destination_level, destination_slice);
    }
    return false;
  }
  const auto& resolved_destination = adopted_destination;
  const bool cube = resolved_destination->d3d_type ==
                    static_cast<u32>(D3DResourceType::kCubeTexture);
  if ((!cube && destination_slice != 0) || (cube && destination_slice >= 6)) {
    return false;
  }
  auto* commands = HostDevice::BeginFrameCommands();
  if (!commands) {
    return false;
  }
  const plume::RenderTextureBarrier barriers[] = {
      plume::RenderTextureBarrier(source->texture.get(),
                                  plume::RenderTextureLayout::COPY_SOURCE),
      plume::RenderTextureBarrier(resolved_destination->texture.get(),
                                  plume::RenderTextureLayout::COPY_DEST),
  };
  commands->barriers(plume::RenderBarrierStage::COPY, barriers, 2);
  // EDRAM surfaces are padded (for example 640x368 -> 640x360). A whole
  // subresource copy exceeds the destination and can remove the D3D12 device.
  const plume::RenderBox source_box(region->left, region->top,
      region->left + region->width, region->top + region->height, 0, 1);
  commands->copyTextureRegion(
      plume::RenderTextureCopyLocation::Subresource(
          resolved_destination->texture.get(), destination_level,
          destination_slice),
      plume::RenderTextureCopyLocation::Subresource(source->texture.get(), 0,
                                                    0), region->x, region->y, 0, &source_box);
  const plume::RenderTextureBarrier final_barriers[] = {
      plume::RenderTextureBarrier(source->texture.get(),
                                  plume::RenderTextureLayout::COLOR_WRITE),
      plume::RenderTextureBarrier(resolved_destination->texture.get(),
                                  plume::RenderTextureLayout::SHADER_READ),
  };
  commands->barriers(plume::RenderBarrierStage::GRAPHICS, final_barriers, 2);
  RecordResolvedTexture(*resolved_destination, *region, destination_level, destination_slice);
  return UpdateResolvedSampling(*resolved_destination);
}

bool NativePromotedSurfaceExtent(u32 guest_address, u32& width, u32& height) {
  const auto resource = FindTexture(guest_address);
  const u64 extent = resource ? resource->promoted_viewport.load(std::memory_order_acquire) : 0;
  if (!extent) return false;
  width = u32(extent >> 32); height = u32(extent);
  return true;
}

bool PromoteTiledSurface(u32 guest_address, u32 width, u32 height) {
  auto recording = HostDevice::LockRecording();
  if (!guest_address) return true;
  const auto resource = FindTexture(guest_address);
  if (!resource || !resource->surface || resource->width != width ||
      resource->height > height || !width || !height) return false;
  return ResizeNativeStorage(*resource, NativeConfiguredSceneExtent(width,height));
}

bool ResizeNativeDepthSurface(u32 guest_address, u32 width, u32 height) {
  auto recording=HostDevice::LockRecording();
  if(!width || !height || width>16384 || height>16384) return false;
  const auto resource=FindTexture(guest_address);
  if(!resource || !resource->surface || !resource->texture) return false;
  return ResizeNativeStorage(*resource,{resource->LogicalWidth(),resource->LogicalHeight(),
      width,height},true);
}

bool EndNativeCompositionTargets(bool preserve_writes) {
  auto recording=HostDevice::LockRecording();
  std::vector<std::shared_ptr<TextureResource>> pending;
  for(auto it=g_composition_surfaces.rbegin();it!=g_composition_surfaces.rend();++it) {
    const auto& resource=*it;
    // A released/replaced guest surface has no subsequent logical readers.
    // Its submitted native storage remains retained by existing frame fences.
    if(FindTexture(resource->guest_address).get()!=resource.get()) continue;
    if(preserve_writes && !CopyCompositionStorage(*resource,true)) {
      pending.push_back(resource);continue;
    }
    SwapCompositionStorage(*resource);resource->composition_active=false;
  }
  g_composition_surfaces=std::move(pending);
  g_composition_restore_pending=!g_composition_surfaces.empty();
  return !g_composition_restore_pending;
}

bool RetryNativeCompositionTargets() {
  return !g_composition_restore_pending || EndNativeCompositionTargets(true);
}

bool BeginNativeCompositionTargets(u32 color_address, u32 depth_address) {
  auto recording=HostDevice::LockRecording();
  if(!RetryNativeCompositionTargets() || (!color_address && !depth_address) ||
      (color_address && color_address==depth_address)) return false;
  const auto color=FindTexture(color_address),depth=FindTexture(depth_address);
  if((color_address && (!color || color->format!=plume::RenderFormat::R8G8B8A8_UNORM)) ||
      (depth_address && (!depth || depth->format!=plume::RenderFormat::D32_FLOAT_S8_UINT))) return false;
  for(const auto& resource:{color,depth})
    if(resource && (!resource->surface || !resource->texture || !resource->view || resource->levels!=1 ||
        resource->LogicalWidth()!=1280 || resource->LogicalHeight()!=720)) return false;
  const size_t begin=g_composition_surfaces.size();
  for(const auto& resource:{color,depth}) {
    if(!resource) continue;
    if(!BeginCompositionSurface(*resource,1920,1080)) {
      // Roll back only this extension. Earlier targets may already contain
      // native warp/UI writes from the current frame and must stay active.
      while(g_composition_surfaces.size()>begin) {
        auto failed=std::move(g_composition_surfaces.back());g_composition_surfaces.pop_back();
        SwapCompositionStorage(*failed);failed->composition_active=false;
      }
      return false;
    }
    if(resource->composition_active && std::find(g_composition_surfaces.begin(),
        g_composition_surfaces.end(),resource)==g_composition_surfaces.end())
      g_composition_surfaces.push_back(resource);
  }
  return true;
}

bool ReconfigureNativeSceneSurfaces(NativeRenderExtent extent) {
  auto recording = HostDevice::LockRecording();
  if (extent.logical_width!=1280 || extent.logical_height!=720 || !extent.width || !extent.height) return false;
  std::vector<std::shared_ptr<TextureResource>> surfaces;
  {
    std::lock_guard lock(g_textures_mutex);
    for (const auto& [address, resource] : g_textures)
      if (resource->surface && resource->LogicalWidth()==1280 &&
          resource->LogicalHeight()==720) surfaces.push_back(resource);
  }
  std::vector<PreparedNativeStorage> prepared;
  prepared.reserve(surfaces.size());
  for (const auto& resource : surfaces) {
    if (resource->composition_active) return false;
    prepared.emplace_back();
    if (!PrepareNativeStorage(*resource,extent,false,prepared.back())) return false;
  }
  // The recording lock spans preparation and the whole publication. Failed
  // texture/view/descriptor creation above leaves every old surface usable.
  for (auto& update : prepared) CommitNativeStorage(update);
  return true;
}

bool ReconfigureNativeSceneSurfaces() {
  return ReconfigureNativeSceneSurfaces(NativeConfiguredSceneExtent(1280,720));
}

plume::RenderFramebuffer* ResolveFramebuffer(u32 render_target, u32 depth_stencil) {
  return ResolveFramebuffer(std::array<u32, 4>{render_target, 0, 0, 0}, depth_stencil);
}

plume::RenderFramebuffer* ResolveFramebuffer(
    const std::array<u32, 4>& render_targets, u32 depth_stencil) {
  // A hit needs only the owner lock: erase and retire run under it, and pinned owners stay alive.
  if (NativeRecordingGate::InPreparation() && !NativeRecordingMutex().HeldByCurrentThread()) {
    FramebufferKey key{render_targets, depth_stencil};
    std::shared_ptr<TextureResource> owner;
    bool complete = true;
    for (u32 i = 0; i < render_targets.size() && complete; ++i) {
      if (!render_targets[i]) continue;
      const auto color = FindTexture(render_targets[i]);
      if (!color) complete = false;
      else { key.color_owners[i] = color.get(); if (!owner) owner = color; }
    }
    if (complete && depth_stencil) {
      const auto depth = FindTexture(depth_stencil);
      if (!depth) complete = false;
      else { key.depth_owner = depth.get(); owner = depth; }
    }
    if (complete && owner) {
      std::lock_guard lock(owner->mutex);
      const auto existing = owner->framebuffers.find(key);
      if (existing != owner->framebuffers.end()) return existing->second.get();
    }
  }
  // Registration and insertion must be atomic relative to invalidation; its
  // owner snapshot must never miss a framebuffer being created concurrently.
  auto recording = HostDevice::LockRecording();
  std::array<std::shared_ptr<TextureResource>, 4> colors;
  u32 color_count = 0;
  for (u32 i = 0; i < colors.size(); ++i) {
    if (!render_targets[i]) continue;
    colors[i] = FindTexture(render_targets[i]);
    if (!colors[i] || !colors[i]->texture || IsDepthFormat(colors[i]->format)) return nullptr;
    color_count = i + 1;
  }
  // Plume requires contiguous color attachments. Reject a gap rather than
  // renumbering shader outputs or dereferencing a null texture in Plume.
  for (u32 i = 0; i < color_count; ++i) if (!colors[i]) return nullptr;
  const auto depth = FindTexture(depth_stencil);
  if (!color_count && (!depth || !depth->texture)) {
    return nullptr;
  }
  if (depth && !IsDepthFormat(depth->format)) {
    return nullptr;
  }
  const auto owner = depth ? depth : colors[0];
  FramebufferKey key{render_targets, depth_stencil};
  for (u32 i = 0; i < colors.size(); ++i) key.color_owners[i] = colors[i].get();
  key.depth_owner = depth.get();
  std::lock_guard lock(owner->mutex);
  const auto existing = owner->framebuffers.find(key);
  if (existing != owner->framebuffers.end()) {
    return existing->second.get();
  }

  plume::RenderFramebufferDesc desc;
  std::array<const plume::RenderTexture*, 4> color_attachments{};
  if (color_count) {
    for (u32 i = 0; i < color_count; ++i) color_attachments[i] = colors[i]->texture.get();
    desc.colorAttachments = color_attachments.data();
    desc.colorAttachmentsCount = color_count;
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
                 "ds=0x{:08X})", render_targets[0], depth_stencil);
    return nullptr;
  }
  auto* result = framebuffer.get();
  {
    std::lock_guard lock(g_textures_mutex);
    g_framebuffer_owners[owner->guest_address] = owner;
  }
  owner->framebuffers.emplace(key, std::move(framebuffer));
  return result;
}

bool IsNativeTexture(u32 guest_address) {
  std::lock_guard lock(g_textures_mutex);
  return g_textures.contains(guest_address);
}

bool OwnsNativeTextureHeader(u32 guest_address) {
  // Registry membership includes borrowed placement textures. Only headers
  // created here have absent Xenos fetch fields and need decoder exclusion.
  std::lock_guard lock(g_textures_mutex);
  const auto it = g_textures.find(guest_address);
  return it != g_textures.end() && it->second->owns_guest_memory;
}

void LogUnknownTexture(u32 guest_address, const char* operation) {
  if (!guest_address) {
    REXLOG_WARN("Native GPU: {} received a null texture", operation);
    return;
  }
  const auto* words = GuestAt<const be_u32>(guest_address);
  REXLOG_WARN(
      "Native GPU: {} unknown texture 0x{:08X}, header "
      "{:08X} {:08X} {:08X} {:08X} {:08X} {:08X} {:08X} {:08X} "
      "{:08X} {:08X} {:08X} {:08X} {:08X}",
      operation, guest_address, static_cast<u32>(words[0]),
      static_cast<u32>(words[1]), static_cast<u32>(words[2]),
      static_cast<u32>(words[3]), static_cast<u32>(words[4]),
      static_cast<u32>(words[5]), static_cast<u32>(words[6]),
      static_cast<u32>(words[7]), static_cast<u32>(words[8]),
      static_cast<u32>(words[9]), static_cast<u32>(words[10]),
      static_cast<u32>(words[11]), static_cast<u32>(words[12]));
}

u32 NativeTextureType(u32 guest_address) {
  const auto resource = FindTexture(guest_address);
  // Borrowed headers must use the original GetResourceType, including its
  // dimension/stacked distinctions. Registry metadata is a host view.
  return resource && resource->owns_guest_memory ? resource->d3d_type : 0;
}

bool IsCpuUploadedTexture(u32 guest_address) {
  auto recording = HostDevice::LockRecording();
  const auto resource = FindTexture(guest_address);
  if (!resource) return false;
  std::lock_guard lock(resource->mutex);
  return !resource->surface && !resource->resolved_on_host &&
      !resource->depth_alias_generation && !resource->owns_guest_memory;
}

u32 AddRefNativeTexture(u32 guest_address) {
  // Match ReleaseNativeTexture's registry lock across the complete RMW.
  // A pinned draw owner is not an authorization to change a retired header.
  std::lock_guard lock(g_textures_mutex);
  if (!g_textures.contains(guest_address)) return 0;
  auto* header = GuestAt<D3DResource>(guest_address);
  const u32 count = static_cast<u32>(header->reference_count) + 1;
  header->reference_count = count;
  return count;
}

// Resources whose final release is queued but not yet run; guarded by g_textures_mutex.
std::unordered_set<const void*> g_destroying_textures;

void DestroyNativeTexture(u32 guest_address, const std::shared_ptr<TextureResource>& released) {
  auto recording = HostDevice::LockRecording();
  {
    std::lock_guard lock(g_textures_mutex);
    g_destroying_textures.erase(released.get());
    const auto it = g_textures.find(guest_address);
    if (it != g_textures.end() && it->second == released) g_textures.erase(it);
  }
  if (g_texture_lifecycle_logs.fetch_add(1, std::memory_order_relaxed) < 64) {
    REXLOG_INFO("Native GPU: released {} 0x{:08X}",
                released->surface ? "surface" : "texture", guest_address);
  }
  auto* memory = REX_KERNEL_MEMORY();
  InvalidateFramebufferReferences(guest_address, released.get());
  HostDevice::UnregisterTexture(released->descriptor_index);
  HostDevice::UnregisterTexture(released->resolved_descriptor_index);
  HostDevice::UnregisterTexture(released->sampled_descriptor_index);
  for (u32 descriptor : released->previous_surface_descriptors)
    HostDevice::UnregisterTexture(descriptor);
  HostDevice::RetireResource(released);
  if (released->mirror_address) {
    memory->SystemHeapFree(released->mirror_address);
  }
  if (released->owns_guest_memory) {
    memory->SystemHeapFree(guest_address);
  }
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
    released = it->second;
    if (!g_destroying_textures.insert(released.get()).second) return 0;
  }
  // Queued draws may still sample it, so destruction runs behind them.
  if (NativeWorkerShouldQueue() && NativeDeferredReleaseEnabled()) {
    if (NativeFullDrawPreparationEnabled()) {
      // Capture order forgets the record behind earlier captures; GPU cleanup replays behind their tapes.
      NativeWorkerSubmit([guest_address, released] {
        {
          std::lock_guard lock(g_textures_mutex);
          const auto it = g_textures.find(guest_address);
          if (it != g_textures.end() && it->second == released) g_textures.erase(it);
        }
        QueueNativeReplayTask([guest_address, released] { DestroyNativeTexture(guest_address, released); });
      }, false, true, true);
      return 0;
    }
    NativeWorkerSubmit([guest_address, released] { DestroyNativeTexture(guest_address, released); }, false, false, false, NativeTaskStage::kRecorder);
    return 0;
  }
  DestroyNativeTexture(guest_address, released);
  return 0;
}

std::shared_ptr<void> FindTextureOwner(u32 guest_address) {
  return guest_address ? g_textures.Lookup(guest_address, g_textures_mutex) : nullptr;
}

void StartTextureResources() {
  g_texture_upload_publications.Reset();
  surface_pool::Start();
}

void ResetTextureResources() {
  auto recording = HostDevice::LockRecording();
  g_texture_upload_publications.Reset();
  // Disable recycling before clearing registries. Retained submitted-frame
  // references may be destroyed later in HostState teardown; they must not
  // repopulate a cache tied to a device that is about to be destroyed.
  surface_pool::Stop();
  g_composition_surfaces.clear();g_composition_restore_pending=false;
  g_framebuffer_owners.clear();
  g_depth_resolves.clear();
  g_edram_colors.clear();
  g_color_aliases.Reset();
  g_depth_alias_generation = 0;
  std::vector<std::pair<u32, u32>> allocations;
  {
    std::lock_guard lock(g_textures_mutex);
    allocations.reserve(g_textures.size());
    for (const auto& [guest_address, resource] : g_textures) {
      if (resource->owns_guest_memory) {
        allocations.emplace_back(guest_address, resource->mirror_address);
      } else if (resource->mirror_address) {
        allocations.emplace_back(0, resource->mirror_address);
      }
    }
    g_textures.clear();
  }
  auto* memory = REX_KERNEL_MEMORY();
  for (const auto [guest_address, mirror_address] : allocations) {
    if (mirror_address) {
      memory->SystemHeapFree(mirror_address);
    }
    if (guest_address) {
      memory->SystemHeapFree(guest_address);
    }
  }
}

namespace {
// Guest (not yet physical) ranges of a texture's backing: the mirror, or the base and mip chains.
std::array<PhysicalCopyRange, 2> TextureDataRanges(const TextureResource& resource) {
  if (resource.surface) return {};  // EDRAM has no physical-memory backing.
  if (resource.owns_guest_memory) return {PhysicalCopyRange{resource.mirror_address, resource.mirror_size}, {}};
  namespace tu = rex::graphics::texture_util;
  const auto& fetch = resource.guest_fetch;
  if (!fetch.base_address || !resource.levels) return {};
  const u32 key[4] = {resource.width, resource.height, resource.depth, resource.levels};
  if (!resource.layout_cached ||
      std::memcmp(&resource.layout_fetch, &fetch, sizeof(fetch)) != 0 ||
      std::memcmp(resource.layout_key, key, sizeof(key)) != 0) {
    const auto layout = tu::GetGuestTextureLayout(fetch.dimension, fetch.pitch,
        resource.width, resource.height,
        fetch.dimension == rex::graphics::xenos::DataDimension::k3D ? resource.depth : 1,
        fetch.tiled, fetch.format, fetch.packed_mips, true, resource.levels - 1);
    const u32 endian_xor=TextureEndianXor(u32(fetch.endianness));
    resource.layout_base_bytes = TextureEndianExtent(layout.base.level_data_extent_bytes,endian_xor);
    resource.layout_mips_bytes = TextureEndianExtent(layout.mips_total_extent_bytes,endian_xor);
    std::memcpy(&resource.layout_fetch, &fetch, sizeof(fetch));
    std::memcpy(resource.layout_key, key, sizeof(key));
    resource.layout_cached = true;
  }
  std::array<PhysicalCopyRange, 2> ranges{PhysicalCopyRange{u32(fetch.base_address) << 12, resource.layout_base_bytes}, {}};
  if (resource.levels > 1 && fetch.mip_address)
    ranges[1] = {u32(fetch.mip_address) << 12, resource.layout_mips_bytes};
  return ranges;
}

bool TextureDataOverlapsPoolCopy(const TextureResource& resource, PhysicalCopyRange range) {
  for (const auto& data : TextureDataRanges(resource))
    if (data.length && GuestRangeOverlapsPoolCopy(data.address, data.length, range)) return true;
  return false;
}

TexturePreparationFootprint TexturePreparationFootprintOf(
    TextureResource& resource, const std::array<u32, 7>* header) {
  TexturePreparationFootprint footprint;
  // A worker can be uploading this record. Capturing its dependencies must
  // not stall the coordinator behind that upload. Callers hold no resource mutex.
  // Unknown is a conservative wait at the resolve boundary, outside locks.
  std::unique_lock lock(resource.mutex, std::try_to_lock);
  if (!lock.owns_lock()) { footprint.unknown = true; return footprint; }
  if (resource.replaced.load(std::memory_order_acquire) ||
      (!resource.owns_guest_memory && !resource.surface &&
       (!header || (*header)[0] != resource.adopted_header_type ||
        std::memcmp(header->data() + 1, &resource.guest_fetch, sizeof(resource.guest_fetch))))) {
    footprint.unknown = true;
    return footprint;
  }
  // Header addresses are virtual. Unlike pixel base pages, a low virtual
  // header must not be interpreted as an identically numbered physical page.
  const auto physical = REX_KERNEL_MEMORY()->GetPhysicalAddress(resource.guest_address);
  if (physical != UINT32_MAX) {
    if (physical > 0x20000000u - sizeof(D3DTexture)) footprint.unknown = true;
    else footprint.ranges[0] = {physical, sizeof(D3DTexture)};
  }
  const auto data = TextureDataRanges(resource);
  for (size_t i = 0; i < data.size(); ++i)
    if (data[i].length && !PoolCopyPhysicalRange(data[i].address, data[i].length, footprint.ranges[i + 1]))
      footprint.unknown = true;
  return footprint;
}

// Discarding host authority after a partial overwrite would lose GPU-produced
// bytes outside the copy. A complete replacement of every backing span needs
// no readback. Compare canonical physical ranges, including E/F alias bias.
bool TextureDataCoveredByPoolCopy(const TextureResource& resource, PhysicalCopyRange copy) {
  bool any = false;
  for (const auto& data : TextureDataRanges(resource)) {
    if (!data.length) continue;
    PhysicalCopyRange physical;
    if (!PoolCopyPhysicalRange(data.address, data.length, physical) ||
        copy.address > physical.address ||
        u64(copy.address) + copy.length < u64(physical.address) + physical.length) return false;
    any = true;
  }
  return any;
}
}  // namespace

TexturePreparationFootprint CaptureTexturePreparationFootprint(u32 address) {
  if (!address) return {};
  const auto resource = FindTexture(address);
  if (!resource) { TexturePreparationFootprint result; result.unknown = true; return result; }
  if (resource->owns_guest_memory || resource->surface)
    return TexturePreparationFootprintOf(*resource, nullptr);
  D3DTexture metadata{};
  if (!CopyNativeGuestMetadata(*REX_KERNEL_MEMORY(),address,&metadata,sizeof(metadata))) {
    TexturePreparationFootprint result; result.unknown = true; return result;
  }
  std::array<u32, 7> header{};
  header[0] = u32(metadata.resource.common) & 0x1Fu;
  for (u32 i = 0; i < 6; ++i) header[i + 1] = metadata.format.dword[i];
  return TexturePreparationFootprintOf(*resource, &header);
}

bool PoolCopyHasHostTextureSource(PhysicalCopyRange source) {
  auto recording = HostDevice::LockRecording();
  std::lock_guard lock(g_textures_mutex);
  for (const auto& [address, resource] : g_textures) {
    std::lock_guard texture_lock(resource->mutex);
    // The header is always CPU data, even for a GPU-resolved texture. Its
    // relocation must not be mistaken for a read of GPU-produced pixels.
    if (((resource->resolved_on_host && !resource->guest_backing_current) || resource->depth_alias_generation) &&
        TextureDataOverlapsPoolCopy(*resource, source)) {
      static std::atomic<u32> blocked_sources{0};
      if (blocked_sources.fetch_add(1, std::memory_order_relaxed) < 32)
        REXLOG_WARN("Native GPU: pool copy GPU source physical={:08X} bytes={} "
            "texture={:08X} resolved={} depth_alias={}", source.address,
            source.length, address, resource->resolved_on_host,
            resource->depth_alias_generation);
      return true;
    }
  }
  return false;
}

// Textures by 64 KB physical block plus a page bitmap for the quick reject; erased entries are skipped on use.
struct TexturePoolIndex {
  PhysicalPageBitmap pages;
  std::unordered_map<u32, std::vector<u32>> blocks;
  std::vector<std::weak_ptr<TextureResource>> entries;
  u64 key[2] = {~0ull, ~0ull};
};

namespace {
// Textures whose footprint moved in place (relocated header, new mirror); the index adds them again.
std::mutex g_texture_footprint_mutex;
std::vector<u32> g_texture_footprint_changes;
bool g_texture_footprint_overflow = false;

void NoteTextureFootprintChange(u32 guest_address) {
  {
    std::lock_guard lock(g_texture_footprint_mutex);
    if (g_texture_footprint_changes.size() < 8192) g_texture_footprint_changes.push_back(guest_address);
    else g_texture_footprint_overflow = true;
  }
  g_texture_footprint_epoch.fetch_add(1, std::memory_order_release);
}

void AddToTexturePoolIndex(TexturePoolIndex& index, const std::shared_ptr<TextureResource>& resource) {
  std::lock_guard texture_lock(resource->mutex);
  const u32 id = u32(index.entries.size());
  bool used = false;
  const auto add = [&](PhysicalCopyRange physical) {
    index.pages.Mark(physical);
    const u32 first = physical.address >> 16, last = u32((u64(physical.address) + physical.length - 1) >> 16);
    for (u32 block = first; block <= last; ++block) {
      auto& ids = index.blocks[block];
      if (ids.empty() || ids.back() != id) ids.push_back(id);
    }
    used = true;
  };
  PhysicalCopyRange physical;
  // The same ranges InvalidatePoolCopyTextures tests: the header, then the base and mip data.
  if (!resource->owns_guest_memory && PoolCopyPhysicalRange(resource->guest_address, sizeof(D3DTexture), physical))
    add(physical);
  for (const auto& data : TextureDataRanges(*resource))
    if (data.length && PoolCopyPhysicalRange(data.address, data.length, physical)) add(physical);
  if (used) index.entries.push_back(resource);
}
}  // namespace

// New and moved textures are added as they appear; stale entries (erased or old footprints) only widen
// the candidate sets, and a full rebuild drops them once they outnumber the live textures.
TexturePoolIndex& CurrentTexturePoolIndex() {
  static TexturePoolIndex index;
  static bool built = false;
  const u64 insertions = g_textures.Insertions();
  const u64 epoch = g_texture_footprint_epoch.load(std::memory_order_acquire);
  if (built && index.key[0] == insertions && index.key[1] == epoch) return index;
  std::vector<u32> moved;
  bool complete = true;
  {
    std::lock_guard lock(g_texture_footprint_mutex);
    moved.swap(g_texture_footprint_changes);
    complete = !std::exchange(g_texture_footprint_overflow, false);
  }
  // Only reads registries: the registry half suffices and never waits on the recorder.
  struct RegistryOnly {
    RegistryOnly() { NativeRecordingGate::EnterPreparation(); }
    ~RegistryOnly() { NativeRecordingGate::LeavePreparation(); }
  } registry_only;
  auto recording = HostDevice::LockRecording();
  std::lock_guard lock(g_textures_mutex);
  std::vector<u32> added;
  complete = g_textures.TakeInsertedKeys(added) && complete;
  // Keys read after the takes: a change racing with this update is taken next time.
  index.key[0] = g_textures.Insertions();
  index.key[1] = g_texture_footprint_epoch.load(std::memory_order_acquire);
  if (!built || !complete || index.entries.size() > 2 * g_textures.size() + 4096) {
    index.pages.Clear();
    index.blocks.clear();
    index.entries.clear();
    for (const auto& [address, resource] : g_textures) AddToTexturePoolIndex(index, resource);
    built = true;
  } else {
    added.insert(added.end(), moved.begin(), moved.end());
    std::sort(added.begin(), added.end());
    added.erase(std::unique(added.begin(), added.end()), added.end());
    for (const u32 address : added) {
      const auto found = g_textures.find(address);
      if (found != g_textures.end()) AddToTexturePoolIndex(index, found->second);
    }
  }
  return index;
}

// Every registered texture that may touch the range (a superset; exact tests follow on each).
std::vector<std::shared_ptr<TextureResource>> PoolCopyTextureCandidates(PhysicalCopyRange range) {
  std::vector<std::shared_ptr<TextureResource>> result;
  auto& index = CurrentTexturePoolIndex();
  if (!index.pages.Any(range)) return result;
  std::vector<u32> ids;
  const u32 first = range.address >> 16, last = u32((u64(range.address) + range.length - 1) >> 16);
  for (u32 block = first; block <= last; ++block) {
    const auto found = index.blocks.find(block);
    if (found != index.blocks.end()) ids.insert(ids.end(), found->second.begin(), found->second.end());
  }
  std::sort(ids.begin(), ids.end());
  ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
  for (const u32 id : ids)
    if (auto resource = index.entries[id].lock()) result.push_back(std::move(resource));
  // A texture re-added after a footprint move has two entries; visit it once.
  std::sort(result.begin(), result.end());
  result.erase(std::unique(result.begin(), result.end()), result.end());
  return result;
}

bool PoolCopyMayTouchTextures(PhysicalCopyRange range) {
  return CurrentTexturePoolIndex().pages.Any(range);
}

bool PoolCopyGpuSourceIndexed(PhysicalCopyRange source) {
  for (const auto& resource : PoolCopyTextureCandidates(source)) {
    std::lock_guard texture_lock(resource->mutex);
    if (((resource->resolved_on_host && !resource->guest_backing_current) || resource->depth_alias_generation) &&
        TextureDataOverlapsPoolCopy(*resource, source)) return true;
  }
  return false;
}

bool PoolCopyGpuDestinationIndexed(PhysicalCopyRange destination) {
  for (const auto& resource : PoolCopyTextureCandidates(destination)) {
    std::lock_guard texture_lock(resource->mutex);
    if (((resource->resolved_on_host && !resource->guest_backing_current) || resource->depth_alias_generation) &&
        TextureDataOverlapsPoolCopy(*resource, destination) &&
        !TextureDataCoveredByPoolCopy(*resource, destination)) return true;
  }
  return false;
}

PoolCopyGpuSources CapturePoolCopyGpuSources() {
  PoolCopyGpuSources sources;
  auto recording = HostDevice::LockRecording();
  std::lock_guard lock(g_textures_mutex);
  for (const auto& [address, resource] : g_textures) {
    std::lock_guard texture_lock(resource->mutex);
    if ((resource->resolved_on_host && !resource->guest_backing_current) || resource->depth_alias_generation)
      sources.textures.push_back(resource);
  }
  return sources;
}

bool PoolCopyGpuSourceOverlaps(const PoolCopyGpuSources& sources, PhysicalCopyRange source) {
  for (const auto& owner : sources.textures) {
    auto& resource = *std::static_pointer_cast<TextureResource>(owner);
    std::lock_guard texture_lock(resource.mutex);
    // The flags are read again: a batch flush may have handed the backing back to the CPU.
    if (((resource.resolved_on_host && !resource.guest_backing_current) || resource.depth_alias_generation) &&
        TextureDataOverlapsPoolCopy(resource, source)) return true;
  }
  return false;
}

struct ResolvedTextureWriteback {
  std::shared_ptr<TextureResource> resource;
  PhysicalCopyRange backing;
  TextureReadbackFormat format;
  u32 texel_bytes=0,row_pitch=0,ox=0,oy=0;
  bool rgba8=false;
  std::vector<u8> snapshot,staged;
  CpuMemoryStamp cpu_stamp;
};

enum class TextureWritebackRejection {
  kNotBorrowed, kNotResolved, kDepthAlias, kMipOrVolume, kStorage,
  kScale, kDimension, kNumericConversion, kCoverage, kPlacement,
  kPhysicalRange, kReadAccess, kOverlappingVersions, kGpuReadback,
  kPlacementAfterWait, kReadAccessAfterWait, kEncoding, kWriteAccess,
  kCount
};

// The generic ordered-copy error does not identify which retained texture
// blocked compaction. Keep a small independent budget for each failure kind,
// so one repeated layout failure cannot hide a later stale-header failure.
bool RejectTextureWriteback(TextureWritebackRejection reason, const TextureResource& resource) {
  static constexpr const char* names[] = {
    "not-borrowed", "not-resolved", "depth-alias", "mip-or-volume", "storage-or-format",
    "scale", "dimension", "numeric-conversion", "coverage", "placement",
    "physical-range", "read-access", "overlapping-versions", "gpu-readback",
    "placement-after-wait", "read-access-after-wait", "encoding", "write-access"
  };
  static_assert(std::size(names) == size_t(TextureWritebackRejection::kCount));
  static std::array<std::atomic<u32>, std::size(names)> counts{};
  const auto index = size_t(reason);
  if (counts[index].fetch_add(1, std::memory_order_relaxed) < 4) {
    REXLOG_WARN("Native GPU: texture writeback rejected reason={} texture={:08X} "
        "base={:08X} mip={:08X} logical={}x{} host={}x{} depth={} levels={} "
        "host_format={} guest_format={} resolved={} guest_current={} depth_alias={} "
        "owned={} surface={} coverage={} placement_matches={}", names[index],
        resource.guest_address, u32(resource.guest_fetch.base_address) << 12,
        u32(resource.guest_fetch.mip_address) << 12, resource.width, resource.height,
        resource.HostWidth(), resource.HostHeight(), resource.depth, resource.levels,
        u32(resource.format), u32(resource.guest_fetch.format), resource.resolved_on_host,
        resource.guest_backing_current, resource.depth_alias_generation,
        resource.owns_guest_memory, resource.surface,
        resource.writeback_coverage.Matches(resource.HostWidth(), resource.HostHeight()),
        TexturePlacementHeaderMatches(resource));
  }
  return false;
}

// The caller holds the resource mutex and the recording gate throughout the
// transaction. Capture every guest snapshot before any GPU wait.
bool PrepareResolvedTextureWriteback(ResolvedTextureWriteback& job) {
  const auto& resource=job.resource;
  const auto& fetch=resource->guest_fetch;
  namespace xenos=rex::graphics::xenos;
  job.format=ResolvedTextureReadbackFormat(*resource);
  job.rgba8=job.format==TextureReadbackFormat::kRgba8;
  const bool floating=job.format==TextureReadbackFormat::kR16Float ||
      job.format==TextureReadbackFormat::kRg16Float || job.format==TextureReadbackFormat::kRgba16Float ||
      job.format==TextureReadbackFormat::kR32Float || job.format==TextureReadbackFormat::kRg32Float ||
      job.format==TextureReadbackFormat::kRgba32Float;
  job.texel_bytes=TextureReadbackTexelBytes(job.format);
  using Reject = TextureWritebackRejection;
  const auto reject = [&](Reject reason) { return RejectTextureWriteback(reason, *resource); };
  if (resource->surface || resource->owns_guest_memory) return reject(Reject::kNotBorrowed);
  if (!resource->resolved_on_host) return reject(Reject::kNotResolved);
  if (resource->depth_alias_generation) return reject(Reject::kDepthAlias);
  if (resource->levels!=1 || resource->depth!=1) return reject(Reject::kMipOrVolume);
  if (!resource->texture || !job.texel_bytes) return reject(Reject::kStorage);
  if (!TextureWritebackScaleSupported(resource->width,resource->height,
          resource->HostWidth(),resource->HostHeight())) return reject(Reject::kScale);
  if (fetch.dimension!=xenos::DataDimension::k2DOrStacked || fetch.stacked) return reject(Reject::kDimension);
  if ((job.rgba8 && (fetch.num_format || fetch.exp_adjust)) ||
      (!floating && (u32(fetch.sign_x) || u32(fetch.sign_y) || u32(fetch.sign_z) || u32(fetch.sign_w))))
    return reject(Reject::kNumericConversion);
  if (!resource->writeback_coverage.Matches(resource->HostWidth(),resource->HostHeight())) return reject(Reject::kCoverage);
  if (!TexturePlacementHeaderMatches(*resource)) return reject(Reject::kPlacement);
  namespace tu=rex::graphics::texture_util;
  const auto layout=tu::GetGuestTextureLayout(fetch.dimension,fetch.pitch,
      resource->width,resource->height,1,fetch.tiled,fetch.format,fetch.packed_mips,true,0);
  const u32 extent=TextureEndianExtent(layout.base.level_data_extent_bytes,TextureEndianXor(u32(fetch.endianness)));
  if (!PoolCopyPhysicalRange(fetch.base_address<<12,extent,job.backing) ||
      job.backing.length>64u*1024*1024) return reject(Reject::kPhysicalRange);
  if (!PoolCopyRangeReadable(job.backing)) return reject(Reject::kReadAccess);
  job.row_pitch=layout.base.row_pitch_bytes;
  if(fetch.packed_mips) {
    u32 oz=0;tu::GetPackedMipOffset(resource->width,resource->height,1,fetch.format,0,job.ox,job.oy,oz);
  }
  auto* guest=REX_KERNEL_MEMORY()->TranslatePhysical<u8*>(job.backing.address);
  job.snapshot.assign(guest,guest+job.backing.length);
  return true;
}

bool EncodeResolvedTextureWriteback(ResolvedTextureWriteback& job,std::span<const u8> rgba) {
  const auto& resource=job.resource;
  const auto& fetch=resource->guest_fetch;
  const u32 texel_bytes=job.texel_bytes;
  const u32 endian=u32(fetch.endianness)==1?1:u32(fetch.endianness)==2?3:u32(fetch.endianness)==3?2:0;
  job.staged=job.snapshot;
  return WritebackTextureRaw(rgba,resource->width,resource->height,resource->HostWidth()*texel_bytes,
      job.staged,endian,job.rgba8 && (u32(fetch.swizzle)&0x1FFu)==0xAu,
      [&](u32 x,u32 y)->int64_t {
        if(fetch.tiled) return rex::graphics::texture_util::GetTiledOffset2D(
            x+job.ox,y+job.oy,job.row_pitch/texel_bytes,TextureReadbackTexelLog2(job.format));
        return u64(y+job.oy)*job.row_pitch+u64(x+job.ox)*texel_bytes;
      },resource->HostWidth(),resource->HostHeight(),&resource->writeback_coverage,texel_bytes);
}

void PublishResolvedTextureWriteback(ResolvedTextureWriteback& job) {
  const auto& resource=job.resource;
  const auto backing=job.backing;
  auto* guest=REX_KERNEL_MEMORY()->TranslatePhysical<u8*>(backing.address);
  // Sizes were validated during encoding, before any resource is published.
  const auto merged=MergeTextureWriteback(std::span<u8>(guest,backing.length),job.staged,job.snapshot);
  resource->guest_backing_current=true;resource->cpu_stamp=std::move(job.cpu_stamp);resource->writeback_coverage.Reset();
  if (merged.cpu_modified) {
    resource->resolved_on_host=false;resource->guest_uploaded=false;
    resource->upload_source_key_valid=false;resource->sampled_valid=false;
    resource->depth_resolve_generation=0;
  }
  REXLOG_INFO("Native GPU: pool source writeback texture={:08X} physical={:08X} bytes={} {}x{}",
      resource->guest_address,backing.address,backing.length,resource->width,resource->height);
}

bool MaterializeResolvedTextures(std::span<const std::shared_ptr<TextureResource>> resources) {
  if(resources.empty()) return true;
  std::vector<std::shared_ptr<TextureResource>> ordered(resources.begin(),resources.end());
  std::sort(ordered.begin(),ordered.end(),[](const auto& a,const auto& b){
    return std::less<const TextureResource*>{}(a.get(),b.get());
  });
  ordered.erase(std::unique(ordered.begin(),ordered.end()),ordered.end());
  std::vector<std::unique_lock<std::mutex>> locks;
  for(const auto& resource:ordered) locks.emplace_back(resource->mutex);
  std::vector<ResolvedTextureWriteback> jobs;
  std::vector<TextureReadbackRequest> requests;
  std::vector<PhysicalCopyRange> ranges;
  for(const auto& resource:ordered) {
    jobs.push_back({resource});
    if(!PrepareResolvedTextureWriteback(jobs.back())) return false;
    ranges.push_back(jobs.back().backing);
    requests.push_back({resource->texture.get(),resource->HostWidth(),resource->HostHeight(),jobs.back().format});
  }
  std::sort(ranges.begin(),ranges.end(),[](auto a,auto b){return a.address<b.address;});
  // Separate allocations are independent, regardless of registry traversal.
  // Overlapping GPU versions still require a write-order/coverage proof.
  for(size_t i=1;i<ranges.size();++i) if(CopyRangesOverlap(ranges[i-1],ranges[i])) {
    for (size_t at : {i - 1, i}) {
      const auto job = std::find_if(jobs.begin(), jobs.end(), [&](const auto& candidate) {
        return candidate.backing.address == ranges[at].address && candidate.backing.length == ranges[at].length;
      });
      RejectTextureWriteback(TextureWritebackRejection::kOverlappingVersions, *job->resource);
    }
    return false;
  }
  std::vector<std::vector<u8>> pixels;
  if(!ReadbackTexturesRaw(HostDevice::Device(),HostDevice::BeginFrameCommands(),requests,pixels,
      []{return HostDevice::Synchronize(SyncReason::kResource);},
      [](auto buffer){HostDevice::RetireResource(std::move(buffer));}))
    return RejectTextureWriteback(TextureWritebackRejection::kGpuReadback, *ordered.front());
  // Validate every header/access and every encoded offset before publishing
  // any byte. A failure keeps all guest bytes and GPU authority intact.
  for(const auto& job:jobs) {
    const auto& resource=job.resource;const auto backing=job.backing;
    if (!TexturePlacementHeaderMatches(*resource))
      return RejectTextureWriteback(TextureWritebackRejection::kPlacementAfterWait, *resource);
    if (!PoolCopyRangeReadable(backing))
      return RejectTextureWriteback(TextureWritebackRejection::kReadAccessAfterWait, *resource);
  }
  for(size_t i=0;i<jobs.size();++i) if(!EncodeResolvedTextureWriteback(jobs[i],pixels[i]))
    return RejectTextureWriteback(TextureWritebackRejection::kEncoding, *jobs[i].resource);
  // Arm every watch after all GPU waits, but before opening host write access.
  // Allocate all stamps before the first publication; the commit only moves
  // them. Invalidate all ranges first, since disjoint allocations may share
  // the same watch page/region. Keep guest fault writers blocked until commit.
  auto transaction=LockPhysicalWriteback();
  for(const auto range:ranges) InvalidateCpuPhysicalMemory(range.address,range.length);
  if(CpuMemoryWatchEnabled()) for(auto& job:jobs) {
    const CpuMemorySpan span{job.backing.address,job.backing.length};
    job.cpu_stamp=WatchCpuMemory({&span,1});
  }
  // A GPU resolve may target storage that CPU code can only read. Open only
  // the host physical mapping, after every wait/validation, for the complete
  // transaction. Guest alias protections and heap metadata remain unchanged.
  auto writable=OpenPhysicalWriteback(REX_KERNEL_MEMORY(),ranges);
  if(!writable) return RejectTextureWriteback(TextureWritebackRejection::kWriteAccess, *ordered.front());
  for(auto& job:jobs) PublishResolvedTextureWriteback(job);
  return true;
}

bool MaterializeResolvedTexture(const std::shared_ptr<TextureResource>& resource) {
  return MaterializeResolvedTextures({&resource,1});
}

bool MaterializePoolCopyTextureDependencies(PhysicalCopyRange source,PhysicalCopyRange destination) {
  auto recording=HostDevice::LockRecording();
  std::vector<std::shared_ptr<TextureResource>> affected;
  {
    std::lock_guard registry(g_textures_mutex);
    for(const auto& [address,resource]:g_textures) {
      std::lock_guard lock(resource->mutex);
      if (((resource->resolved_on_host && !resource->guest_backing_current) || resource->depth_alias_generation) &&
          (TextureDataOverlapsPoolCopy(*resource,source) ||
           (TextureDataOverlapsPoolCopy(*resource,destination) &&
            !TextureDataCoveredByPoolCopy(*resource,destination)))) affected.push_back(resource);
    }
  }
  if(affected.empty()) return true;
  const char* enabled=std::getenv("LEGO_NATIVE_POOL_WRITEBACK");
  if((enabled && *enabled=='0') || HostDevice::BackendName()!="D3D12") return false;
  return MaterializeResolvedTextures(affected);
}

bool MaterializePoolCopyTextureSource(PhysicalCopyRange source) {
  const char* enabled=std::getenv("LEGO_NATIVE_POOL_WRITEBACK");
  if(enabled && *enabled=='0') return false;
  return MaterializePoolCopyTextureDependencies(source,{});
}

bool MaterializePoolCopyTextureDestination(PhysicalCopyRange destination) {
  return MaterializePoolCopyTextureDependencies({},destination);
}

// LockRect reads a physical allocation, even if a different placement header
// owns its latest GPU version. Settle every dependency across base/mip spans
// in one transaction before the SDK returns the guest pointer.
bool MaterializeTextureForCpuRead(u32 guest_address) {
  auto recording=HostDevice::LockRecording();
  const auto resource=FindTexture(guest_address);
  if (!resource) return false;
  std::vector<PhysicalCopyRange> reads;
  {
    std::lock_guard lock(resource->mutex);
    if (!resource->owns_guest_memory && !resource->surface &&
        !TexturePlacementHeaderMatches(*resource)) return false;
    for (const auto& data:TextureDataRanges(*resource)) {
      if (!data.length) continue;
      PhysicalCopyRange physical;
      if (!PoolCopyPhysicalRange(data.address,data.length,physical)) return false;
      reads.push_back(physical);
    }
  }
  if (reads.empty()) return false;
  std::vector<std::shared_ptr<TextureResource>> affected;
  {
    std::lock_guard registry(g_textures_mutex);
    for (const auto& [address,owner]:g_textures) {
      std::lock_guard lock(owner->mutex);
      if (!((owner->resolved_on_host && !owner->guest_backing_current) || owner->depth_alias_generation)) continue;
      if (std::any_of(reads.begin(),reads.end(),[&](auto range){
          return TextureDataOverlapsPoolCopy(*owner,range);
        })) affected.push_back(owner);
    }
  }
  if (affected.empty()) return false;
  const bool done=HostDevice::BackendName()=="D3D12" && MaterializeResolvedTextures(affected);
  static std::atomic<u32> logs{0};
  if (!done && logs.fetch_add(1,std::memory_order_relaxed)<16)
    REXLOG_WARN("Native GPU: CPU lock texture {:08X} {}x{} GPU dependency writeback failed",
        guest_address,resource->width,resource->height);
  return done;
}

namespace {
// True when the copy affects the texture; an overwritten header moves it to `retired`, under g_textures_mutex.
template<class Iterator>
bool InvalidatePoolCopyTexture(Iterator it, PhysicalCopyRange destination,
                               std::vector<std::shared_ptr<TextureResource>>& retired, Iterator* next) {
  const auto& resource = it->second;
  std::lock_guard texture_lock(resource->mutex);
  if (!resource->owns_guest_memory &&
      GuestRangeOverlapsPoolCopy(resource->guest_address, sizeof(D3DTexture), destination)) {
    retired.push_back(std::move(it->second));
    *next = g_textures.erase(it);
    return true;
  }
  *next = std::next(it);
  if (!TextureDataOverlapsPoolCopy(*resource, destination)) return false;
  // A plain CPU upload can remain a comparison candidate: dropping
  // its memory stamp forces a hash of the overwritten backing bytes.
  // Identical pool moves then avoid decoding/uploading the same image.
  // GPU resolves/aliases have no trustworthy CPU-upload comparison.
  if (resource->resolved_on_host || resource->depth_resolve_generation ||
      resource->depth_alias_generation) {
    resource->guest_uploaded = false;
    resource->upload_source_key_valid = false;
  }
  resource->cpu_stamp = {};
  // This is an explicit CPU write into the backing allocation. It
  // supersedes an earlier GPU resolve, even when the header is reused
  // unchanged. Otherwise UploadTextureResource's host-authority guard
  // skips the new bytes and old world textures/depth aliases survive.
  resource->resolved_on_host = false;
  resource->guest_backing_current = false;
  resource->sampled_valid = false;
  resource->writeback_coverage.Reset();
  resource->depth_resolve_generation = 0;
  resource->depth_alias_generation = 0;
  return true;
}

void RetirePoolCopyTextures(std::vector<std::shared_ptr<TextureResource>>& retired) {
  for (auto& resource : retired) {
    InvalidateFramebufferReferences(resource->guest_address);
    HostDevice::UnregisterTexture(resource->descriptor_index);
    HostDevice::UnregisterTexture(resource->resolved_descriptor_index);
    HostDevice::UnregisterTexture(resource->sampled_descriptor_index);
    for (u32 descriptor : resource->previous_surface_descriptors)
      HostDevice::UnregisterTexture(descriptor);
    if (resource->mirror_address) REX_KERNEL_MEMORY()->SystemHeapFree(resource->mirror_address);
    HostDevice::RetireResource(std::move(resource));
  }
}
}  // namespace

u32 InvalidatePoolCopyTextures(PhysicalCopyRange destination) {
  auto recording = HostDevice::LockRecording();
  std::vector<std::shared_ptr<TextureResource>> retired;
  u32 count = 0;
  {
    std::lock_guard lock(g_textures_mutex);
    for (auto it = g_textures.begin(); it != g_textures.end();) {
      auto next = it;
      if (InvalidatePoolCopyTexture(it, destination, retired, &next)) ++count;
      it = next;
    }
  }
  RetirePoolCopyTextures(retired);
  return count;
}

u32 InvalidatePoolCopyTexturesIndexed(PhysicalCopyRange destination) {
  const auto candidates = PoolCopyTextureCandidates(destination);
  if (candidates.empty()) return 0;
  auto recording = HostDevice::LockRecording();
  std::vector<std::shared_ptr<TextureResource>> retired;
  u32 count = 0;
  {
    std::lock_guard lock(g_textures_mutex);
    for (const auto& candidate : candidates) {
      const auto it = g_textures.find(candidate->guest_address);
      // Erased or replaced since the index was built: the full scan would not see it either.
      if (it == g_textures.end() || it->second != candidate) continue;
      auto next = it;
      if (InvalidatePoolCopyTexture(it, destination, retired, &next)) ++count;
    }
  }
  RetirePoolCopyTextures(retired);
  return count;
}

u64 TextureRegistryGeneration() { return g_textures.Generation(); }
u64 TextureFootprintEpoch() { return g_texture_footprint_epoch.load(std::memory_order_acquire); }

}  // namespace legodimensions::gpu_native
