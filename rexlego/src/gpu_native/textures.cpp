#include "gpu_native/textures.h"
#include "gpu_native/color_resolve_swizzle.h"

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
#include "gpu_native/shaders.h"
#include "gpu_native/format.h"
#include "gpu_native/texture_number.h"
#include "gpu_native/texture_swizzle.h"
#include "gpu_native/texture_upload_cache.h"
#include "gpu_native/texture_volume_upload.h"
#include "gpu_native/texture_alpha4_upload.h"
#include "gpu_native/long_probe.h"
#include "gpu_native/memory_watch.h"
#include "gpu_native/depth_alias.h"
#include "gpu_native/texture_depth_upload.h"

namespace legodimensions::gpu_native {
namespace {

constexpr u32 kPitchAlignment = 0x100;

struct FramebufferKey {
  std::array<u32, 4> colors{};
  u32 depth = 0;
  bool operator==(const FramebufferKey&) const = default;
};
struct FramebufferKeyHash {
  size_t operator()(const FramebufferKey& key) const {
    size_t hash = key.depth;
    for (u32 color : key.colors) hash = hash * 16777619u ^ color;
    return hash;
  }
};
struct SurfaceStorage {
  std::unique_ptr<plume::RenderTexture> texture;
  std::unique_ptr<plume::RenderTextureView> view;
};

struct TextureResource {
  u32 guest_address = 0;
  u32 mirror_address = 0;
  u32 mirror_size = 0;
  u32 width = 0;
  u32 height = 0;
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
  u32 descriptor_index = ~u32{0};
  u32 resolved_descriptor_index = ~u32{0};
  bool owns_guest_memory = true;
  u32 adopted_header_type = 0;
  rex::graphics::xenos::xe_gpu_texture_fetch_t guest_fetch{};
  bool guest_uploaded = false;
  u64 guest_content_hash = 0;
  TextureUploadSourceKey upload_source_key{};
  bool upload_source_key_valid = false;
  CpuMemoryStamp cpu_stamp;
  bool resolved_on_host = false;
  // Diagnostic-only cache, serialized by the caller's LockRecording. Keep the
  // global probe deduplication for authority transitions and resource reuse.
  u64 last_authority_probe_signature = 0;
  bool authority_probe_signature_valid = false;
  u32 edram_base = ~0u;
  u32 guest_msaa = 0;
  u64 write_generation = 0;
  u64 depth_resolve_generation = 0;
  u64 depth_alias_generation = 0;
};

std::unique_ptr<plume::RenderTexture> CreateHostTexture(
    const TextureResource& resource, u32 usage, u32 multi_sample);

std::mutex g_textures_mutex;
std::unordered_map<u32, std::shared_ptr<TextureResource>> g_textures;
// Only these resources can contain cached framebuffer dependencies. Serialized
// with framebuffer creation/invalidation/reset by HostDevice::LockRecording;
// weak pointers never extend texture or framebuffer lifetimes.
std::unordered_map<u32, std::weak_ptr<TextureResource>> g_framebuffer_owners;
std::atomic<u32> g_texture_lifecycle_logs{0};
TextureUploadTiming g_upload_timing;  // Serialized by LockRecording.
u64 g_depth_alias_generation = 0;  // Serialized by LockRecording.
std::unordered_map<u32, std::weak_ptr<TextureResource>> g_depth_resolves;
std::unordered_map<u32, std::weak_ptr<TextureResource>> g_edram_colors;

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
    if (!heap->IsRangeCommittedReadable(u32(at), u32(next - at))) return false;
    at = next;
  }
  return true;
}

template <typename T>
T* GuestAt(u32 guest_address) {
  return REX_KERNEL_MEMORY()->TranslateVirtual<T*>(guest_address);
}

std::shared_ptr<TextureResource> FindTexture(u32 guest_address) {
  std::lock_guard lock(g_textures_mutex);
  const auto it = g_textures.find(guest_address);
  return it == g_textures.end() ? nullptr : it->second;
}

std::shared_ptr<TextureResource> AdoptTexture(u32 guest_address) {
  if (!guest_address || !HostDevice::IsReady()) {
    return nullptr;
  }
  if (const auto existing = FindTexture(guest_address)) {
    // Diagnostic only: the authoritative binding is the device fetch shadow,
    // so don't replace a cached resource solely because its header changed.
    // Capture both header versions; draw.cpp also traces VS fetch constants.
    static const bool trace_headers = std::getenv("LEGO_GPU_SNAPSHOT_DIR") != nullptr;
    if (trace_headers && !existing->owns_guest_memory) {
      const auto* guest = GuestAt<const D3DTexture>(guest_address);
      std::array<u32, 6> cached{}, current{};
      std::memcpy(cached.data(), &existing->guest_fetch, sizeof(cached));
      for (u32 i = 0; i < current.size(); ++i) current[i] = guest->format.dword[i];
      if (current != cached) {
        if (LongProbeEnabled() && LongProbeOnce(
            (u64(guest_address)<<32) ^ XXH3_64bits(current.data(),sizeof(current))))
          LongProbeEvent("stale_header_at_use", true, "guest=", guest_address,
              "cached=", LongProbeHex(cached), "current=", LongProbeHex(current));
        static std::mutex trace_mutex;
        static std::unordered_map<u32, std::array<u32, 6>> last_changed;
        static u32 logs = 0;
        std::lock_guard trace_lock(trace_mutex);
        const auto found = last_changed.find(guest_address);
        if (logs < 32 && (found == last_changed.end() || found->second != current)) {
          last_changed[guest_address] = current;
          ++logs;
          REXLOG_WARN("Native GPU: adopted texture header changed guest={:08X} common={:08X} "
              "cached={:08X},{:08X},{:08X},{:08X},{:08X},{:08X} "
              "current={:08X},{:08X},{:08X},{:08X},{:08X},{:08X}",
              guest_address, u32(guest->resource.common),
              cached[0], cached[1], cached[2], cached[3], cached[4], cached[5],
              current[0], current[1], current[2], current[3], current[4], current[5]);
        }
      }
    }
    return existing;
  }

  const auto* guest = GuestAt<const D3DTexture>(guest_address);
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
    view_desc.mipLevels = resource->levels;
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
  if (std::getenv("LEGO_NATIVE_RENDERDOC_CAPTURE")) {
    static std::atomic<u32> capture_logs{0};
    if (capture_logs.fetch_add(1) < 1024)
      REXLOG_INFO("Native texture capture: guest={:08X} base={:08X} format={} srv={} resolved_srv={} endian={}",
          guest_address, u32(resource->guest_fetch.base_address << 12), resource->guest_format,
          resource->descriptor_index, resource->resolved_descriptor_index, u32(resource->guest_fetch.endianness));
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
    description.width = resource.width;
    description.height = resource.height;
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
          resource.sampled_texture.get(), resource.width, resource.height, scale,
          ColorResolveDestination::kFloatRGBA32, false))
    return false;
  resource.sampled_valid = true;
  HostDevice::SnapshotTexture(resource.sampled_texture.get(),
      "sampled-" + std::to_string(resource.guest_address), false);
  return true;
}

void InvalidateFramebufferReferences(u32 guest_address) {
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
        if (key.depth == guest_address ||
            std::find(key.colors.begin(), key.colors.end(), guest_address) != key.colors.end()) {
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
  desc.committed = resource.surface || IsDepthFormat(resource.format) ||
                   usage != 0 || multi_sample != 0;
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

bool EnsureDepthSamplingMirror(TextureResource& resource) {
  if (resource.sampled_texture && resource.sampled_view &&
      resource.sampled_descriptor_index != ~0u) return true;
  plume::RenderTextureDesc desc;
  desc.dimension = plume::RenderTextureDimension::TEXTURE_2D;
  desc.width = resource.width; desc.height = resource.height;
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
      resource.sampled_valid && CpuMemoryUnchanged(resource.cpu_stamp)) {
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
  const auto* source = address < 0x20000000u
      ? REX_KERNEL_MEMORY()->TranslatePhysical<const u8*>(address)
      : REX_KERNEL_MEMORY()->TranslateVirtual<const u8*>(address);
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
      resource.upload_source_key_valid && resource.upload_source_key == key) {
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
  if (!EnsureDepthSamplingMirror(resource)) return false;
  auto upload = std::shared_ptr<plume::RenderBuffer>(HostDevice::Device()->createBuffer(
      plume::RenderBufferDesc::UploadBuffer(static_cast<u32>(bytes))).release());
  if (!upload) return false;
  auto* mapped = upload->map();
  if (!mapped) return false;
  std::memcpy(mapped, decoded.data(), decoded.size());
  upload->unmap();
  commands->barriers(plume::RenderBarrierStage::COPY,
      plume::RenderTextureBarrier(resource.sampled_texture.get(), plume::RenderTextureLayout::COPY_DEST));
  commands->copyTextureRegion(
      plume::RenderTextureCopyLocation::Subresource(resource.sampled_texture.get(), 0, 0),
      plume::RenderTextureCopyLocation::PlacedFootprint(upload.get(), plume::RenderFormat::R32_FLOAT,
          resource.width, resource.height, 1, pitch / 4, 0));
  commands->barriers(plume::RenderBarrierStage::GRAPHICS,
      plume::RenderTextureBarrier(resource.sampled_texture.get(), plume::RenderTextureLayout::SHADER_READ));
  HostDevice::RetireResource(std::move(upload));
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
  static std::atomic<u32> capture_logs{0};
  if (std::getenv("LEGO_NATIVE_RENDERDOC_CAPTURE") && capture_logs.fetch_add(1) < 256)
    REXLOG_INFO("Native surface capture: guest={:08X} extent={}x{} depth={} format={:08X} msaa={} parameters={:08X} base={} hi={:08X}",
        address, width, height, depth, guest_format, multi_sample, parameters, base,
        parameters ? u32(*GuestAt<const be_u32>(parameters + 4)) : 0u);
  resource->texture->setName((std::string("LEGO surface ") + std::to_string(address) +
      " base " + std::to_string(base)).c_str());
  const u32 bias = parameters && !depth ? u32(*GuestAt<const be_u32>(parameters + 8)) & 63 : 0;
  if (!render_format && (guest_format & 0x3FE00) == 0x7E00) render_format = 1;
  auto* guest = GuestAt<D3DSurface>(address);
  // +28 is RB_COLOR_INFO for color surfaces, RB_DEPTH_INFO for depth.
  guest->depth_info = base | ((depth ? (texture_format == 23 ? 1u : 0u) : render_format) << 16) |
      (depth ? 0u : bias << 20);
  static std::atomic<u32> logs{0};
  if (!depth && logs.fetch_add(1) < 24)
    REXLOG_INFO("Native GPU: surface color info {:08X} guest_fmt={:08X} rt_fmt={} bias={} scale={}",
        address, guest_format, render_format, ColorExponentBias(guest->depth_info),
        ColorOutputScale(guest->depth_info));
  return address;
}

float SurfaceColorOutputScale(u32 guest_address) {
  const auto resource = FindTexture(guest_address);
  return resource && resource->surface && !IsDepthFormat(resource->format)
      ? ColorOutputScale(GuestAt<const D3DSurface>(guest_address)->depth_info) : 1.0f;
}

void MarkSurfaceWritten(u32 guest_address) {
  if (!DepthAliasesEnabled()) return;
  const auto resource = FindTexture(guest_address);
  if (!resource || !resource->surface) return;
  resource->write_generation = ++g_depth_alias_generation;
  if (resource->edram_base != ~0u && resource->guest_msaa == 0 &&
      resource->format == plume::RenderFormat::R8G8B8A8_UNORM)
    g_edram_colors[resource->edram_base] = resource;
}

bool PrepareSurfaceDepthAlias(u32 guest_address) {
  if (!DepthAliasesEnabled()) return true;
  const auto destination = FindTexture(guest_address);
  if (!destination || !destination->surface || destination->edram_base == ~0u ||
      destination->guest_msaa != 0 || (destination->guest_format & 63) != 22 ||
      destination->HostWidth() % 80) return true;
  const auto found = g_edram_colors.find(destination->edram_base);
  const auto source = found == g_edram_colors.end() ? nullptr : found->second.lock();
  if (!source || source->write_generation <= destination->write_generation ||
      source->HostWidth() != destination->HostWidth() ||
      source->HostHeight() != destination->HostHeight()) return true;
  if (!HostDevice::TransferDepthAlias(source->texture.get(), source->descriptor_index,
      destination->texture.get(), destination->HostWidth(), destination->HostHeight(), true)) return false;
  destination->write_generation = source->write_generation;
  static u32 logs = 0;
  if (logs++ < 24) REXLOG_INFO("Native GPU: EDRAM color->depth alias {:08X}->{:08X} base={} {}x{}",
      source->guest_address, guest_address, destination->edram_base,
      destination->HostWidth(), destination->HostHeight());
  return true;
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

void RefreshTextureHeader(u32 guest_address) {
  auto recording = HostDevice::LockRecording();
  const auto resource = FindTexture(guest_address);
  if (!resource || resource->owns_guest_memory || resource->surface) return;
  const auto* header = GuestAt<const D3DTexture>(guest_address);
  std::array<u32, 6> current{}, cached{};
  for (u32 i = 0; i < current.size(); ++i) current[i] = header->format.dword[i];
  bool address_only = false;
  {
    std::lock_guard lock(resource->mutex);
    std::memcpy(cached.data(), &resource->guest_fetch, sizeof(cached));
    const bool same_type = resource->adopted_header_type == (u32(header->resource.common) & 0x1Fu);
    if (cached == current && same_type) return;
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
      std::memcpy(&resource->guest_fetch, current.data(), sizeof(current));
      resource->upload_source_key_valid = false;
      resource->cpu_stamp = {};
      resource->depth_alias_generation = 0;
      // Keep resolved host storage authoritative. Its guest allocation may
      // move, but uploading CPU memory would erase the native resolve result.
      if (!resource->resolved_on_host) resource->guest_uploaded = false;
    }
  }
  if (!address_only) {
    // The same 52-byte header may now describe another size/format/mip chain.
    // Re-adopt it lazily with fresh storage, retaining old lists at their fences.
    {
      std::lock_guard lock(g_textures_mutex);
      const auto it = g_textures.find(guest_address);
      if (it == g_textures.end() || it->second != resource) return;
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
}

TextureResourceView ResolveTextureResource(u32 guest_address) {
  const auto resource = AdoptTexture(guest_address);
  if (!resource) {
    return {};
  }
  if ((resource->resolved_on_host || resource->guest_uploaded) && resource->sampled_valid)
    return {resource->sampled_texture.get(), resource->sampled_view.get(),
            IsDepthFormat(resource->format) ? plume::RenderFormat::R32_FLOAT
                                          : plume::RenderFormat::R32G32B32A32_FLOAT,
            resource->sampled_descriptor_index,
            resource->width, resource->height, resource->d3d_type, false, false};
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
          IsDepthFormat(resource->format)};
}

bool UploadTextureResource(u32 guest_address,
                           plume::RenderCommandList* commands,
                           bool require_content_hash) {
  auto recording = HostDevice::LockRecording();
  UploadTimer timer;
  const auto resource = FindTexture(guest_address);
  // Render targets and native resolves already have authoritative host
  // storage, including resolved depth sampling mirrors. No CPU upload is
  // required; reporting success lets callers reject genuine upload failures.
  if (resource && commands && resource->texture &&
      (resource->surface || resource->resolved_on_host)) return true;
  if (resource && resource->guest_format == 22 && IsDepthFormat(resource->format))
    return UploadGuestDepthTexture(*resource, commands, require_content_hash);
  if (DepthAliasesEnabled() && resource && commands && !resource->surface &&
      !resource->owns_guest_memory && !resource->resolved_on_host && resource->guest_format == 6 &&
      resource->guest_fetch.num_format == 0 && resource->guest_fetch.exp_adjust == 0 &&
      !(u32(resource->guest_fetch.sign_x) | u32(resource->guest_fetch.sign_y) |
        u32(resource->guest_fetch.sign_z) | u32(resource->guest_fetch.sign_w))) {
    const auto layout = AliasLayout(*resource);
    const auto found = g_depth_resolves.find(layout.physical_base);
    const auto depth = found == g_depth_resolves.end() ? nullptr : found->second.lock();
    if (depth && depth->sampled_valid && depth->depth_resolve_generation &&
        CompatibleDepthAlias(layout, AliasLayout(*depth))) {
      if (resource->depth_alias_generation != depth->depth_resolve_generation) {
        if (!HostDevice::TransferDepthAlias(depth->sampled_texture.get(), depth->sampled_descriptor_index,
            resource->texture.get(), resource->width, resource->height, false)) return false;
        resource->depth_alias_generation = depth->depth_resolve_generation;
        // Keep the raw-storage SRV swizzle: this conversion writes packed bytes,
        // unlike a color resolve, which writes logical RGBA values.
        static u32 logs = 0;
        if (logs++ < 24) REXLOG_INFO("Native GPU: depth raw texture alias {:08X}->{:08X} physical={:08X}",
            depth->guest_address, guest_address, layout.physical_base);
      }
      return true;
    }
    if (resource->depth_alias_generation) {
      resource->depth_alias_generation = 0;
      resource->guest_uploaded = false;
    }
  }
  if (resource && commands && !resource->owns_guest_memory &&
      !resource->surface && !IsDepthFormat(resource->format)) {
    namespace tu = rex::graphics::texture_util;
    using rex::graphics::xenos::DataDimension;
    std::lock_guard lock(resource->mutex);
    if (resource->resolved_on_host) return true;
    static const bool audit_texture_watch = std::getenv("LEGO_NATIVE_AUDIT_TEXTURE_WATCH") != nullptr;
    const bool capture_pending = TextureUploadCapturePending(*resource);
    const bool watch_hit = !capture_pending && !require_content_hash && resource->guest_uploaded &&
        CpuMemoryUnchanged(resource->cpu_stamp);
    if (watch_hit && !audit_texture_watch) {
      if (timer.enabled) ++g_upload_timing.source_hits;
      return true;
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
    const auto guest_bytes = [](u32 address) {
      return address < 0x20000000u
          ? REX_KERNEL_MEMORY()->TranslatePhysical<const u8*>(address)
          : REX_KERNEL_MEMORY()->TranslateVirtual<const u8*>(address);
    };
    const auto* base_bytes = guest_bytes(fetch.base_address << 12);
    const u32 mip_size = resource->levels > 1 ? layout.mips_total_extent_bytes : 0;
    const auto* mip_bytes = mip_size ? guest_bytes(fetch.mip_address << 12) : nullptr;
    TextureUploadSourceKey source_key{};
    const CpuMemorySpan source_spans[] = {
      {u32(fetch.base_address << 12), layout.base.level_data_extent_bytes},
      {u32(fetch.mip_address << 12), mip_size}};
    auto cpu_stamp = CpuMemoryWatchEnabled() ? WatchCpuMemory(source_spans) : CpuMemoryStamp{};
    const auto source_start = timer.enabled ? std::chrono::steady_clock::now()
                                            : std::chrono::steady_clock::time_point{};
    const bool source_key_valid =
        ReadableUploadSpan(fetch.base_address << 12, layout.base.level_data_extent_bytes) &&
        (!mip_size || (fetch.mip_address && ReadableUploadSpan(fetch.mip_address << 12, mip_size)));
    if (LongProbeEnabled() && !source_key_valid && LongProbeOnce(0xBAD0000000000000ull | guest_address))
      LongProbeEvent("unreadable_texture_source", true, "guest=", guest_address,
          "base=", u32(fetch.base_address<<12), "mip=", u32(fetch.mip_address<<12));
    // Every format must have resident base/mip storage before decoding. The
    // layout extents include tiled padding, cube faces and packed mip tails;
    // a failed hash-span check must never fall through to unchecked CPU reads.
    if (!source_key_valid) return false;
    if (source_key_valid) {
      const auto hash_start = timer.enabled ? std::chrono::steady_clock::now()
                                            : std::chrono::steady_clock::time_point{};
      source_key = MakeTextureUploadSourceKey(
          {base_bytes, layout.base.level_data_extent_bytes}, {mip_bytes, mip_size},
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
        g_upload_timing.hashed_bytes += u64(layout.base.level_data_extent_bytes) + mip_size;
        g_upload_timing.source_ms += std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - source_start).count();
      }
      if (!capture_pending && resource->guest_uploaded && resource->upload_source_key_valid &&
          resource->upload_source_key == source_key) {
        if (timer.enabled) ++g_upload_timing.source_hits;
        resource->cpu_stamp = std::move(cpu_stamp);
        return true;
      }
    }
    resource->upload_source_key_valid = false;
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
    const u32 endian_xor = static_cast<u32>(fetch.endianness) == 1 ? 1
        : static_cast<u32>(fetch.endianness) == 2 ? 3
        : static_cast<u32>(fetch.endianness) == 3 ? 2 : 0;
    for (u32 subresource = 0; subresource < faces * resource->levels; ++subresource) {
      const u32 face = subresource / resource->levels;
      const u32 level = subresource % resource->levels;
      destination_offset = AlignUp(destination_offset, 512);
      const u32 packed_level = std::min(level, layout.packed_level);
      const auto& guest_level = level == 0 ? layout.base : layout.mips[packed_level];
      const u32 base = (level == 0 ? fetch.base_address << 12
          : (fetch.mip_address << 12) + layout.mip_offsets_bytes[packed_level]) +
          face * guest_level.array_slice_stride_bytes;
      // D3D resource headers retain the CPU allocation address, unlike PM4
      // fetch constants. In particular, the E/F physical aliases include a
      // 4 KB bias in Memory::TranslateVirtual/GetPhysicalAddress. Masking them
      // with TranslatePhysical reads the preceding page and scrambles tiles.
      // Keep support for headers that already contain a physical address.
      const auto* source = base < 0x20000000u
          ? REX_KERNEL_MEMORY()->TranslatePhysical<const u8*>(base)
          : REX_KERNEL_MEMORY()->TranslateVirtual<const u8*>(base);
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
            {source, guest_level.array_slice_data_extent_bytes},
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
            {source, guest_level.array_slice_data_extent_bytes},
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
          {source, guest_level.array_slice_data_extent_bytes},
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
        source_key_valid ? std::span<const u8>(base_bytes, layout.base.level_data_extent_bytes) : std::span<const u8>{},
        source_key_valid ? std::span<const u8>(mip_bytes, mip_size) : std::span<const u8>{});
    if (timer.enabled) g_upload_timing.converted_bytes += upload_size;
    const u64 content_hash = XXH3_64bits(mapped, upload_size);
    if (LongProbeEnabled() && (!resource->guest_uploaded || resource->guest_content_hash != content_hash))
      LongProbeEvent("texture_upload_changed", false, "guest=", guest_address,
          "width=", resource->width, "height=", resource->height,
          "hash=", content_hash, "previous_hash=", resource->guest_content_hash,
          "bytes=", upload_size, "base=", u32(fetch.base_address<<12),
          "mip=", u32(fetch.mip_address<<12));
    if (resource->guest_uploaded && resource->guest_content_hash == content_hash) {
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
    commands->barriers(plume::RenderBarrierStage::COPY,
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
      commands->copyTextureRegion(
          plume::RenderTextureCopyLocation::Subresource(resource->texture.get(), level, face),
          plume::RenderTextureCopyLocation::PlacedFootprint(upload.get(), resource->format,
              width, height, volume ? MipDimension(resource->depth, level) : 1,
              RowPitch(resource->format, width) / host_block_bytes * host_block_width,
              destination_offset));
      destination_offset += LevelSize(*resource, level);
    }
    commands->barriers(plume::RenderBarrierStage::GRAPHICS,
        plume::RenderTextureBarrier(resource->texture.get(), plume::RenderTextureLayout::SHADER_READ));
    HostDevice::RetireResource(std::move(upload));
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

bool ResolveTextureFromSurface(u32 destination_texture, u32 source_surface,
                               u32 destination_level,
                               u32 destination_slice, u32 resolve_flags,
                               const ResolveRect* rectangle, const ResolvePoint* point) {
  auto recording = HostDevice::LockRecording();
  if (!PrepareSurfaceDepthAlias(source_surface)) return false;
  RefreshTextureHeader(destination_texture);
  const auto destination = FindTexture(destination_texture);
  const auto adopted_destination =
      destination ? destination : AdoptTexture(destination_texture);
  const auto source = FindTexture(source_surface);
  if (source && adopted_destination && std::getenv("LEGO_NATIVE_RENDERDOC_CAPTURE")) {
    static std::unordered_set<u64> captured;
    if (captured.size() < 128 && captured.insert((u64(source_surface) << 32) | destination_texture).second)
      REXLOG_INFO("Native resolve capture: source={:08X} destination={:08X} base={:08X} format={} flags={:08X}",
          source_surface, destination_texture, u32(adopted_destination->guest_fetch.base_address << 12),
          adopted_destination->guest_format, resolve_flags);
  }
  const auto region = source && adopted_destination &&
      destination_level < adopted_destination->levels ? CheckedResolveRegion(
      source->HostWidth(), source->HostHeight(),
      MipDimension(adopted_destination->width, destination_level),
      MipDimension(adopted_destination->height, destination_level), rectangle, point)
      : std::nullopt;
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
        dst.sampled_texture.get(), dst.width, dst.height, 1.0f,
        ColorResolveDestination::kDepthFloat32, true, &*region)) return false;
    dst.resolved_on_host = true; dst.sampled_valid = true;
    if (DepthAliasesEnabled()) {
      dst.depth_resolve_generation = ++g_depth_alias_generation;
      g_depth_resolves[AliasLayout(dst).physical_base] = adopted_destination;
    }
    HostDevice::SnapshotTexture(dst.sampled_texture.get(),
        "depth-atlas-" + std::to_string(destination_texture), false);
    static std::atomic<u32> logs{0};
    if (logs.fetch_add(1) < 16)
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
        static_cast<plume::D3D12CommandList*>(commands)->d3d->CopyResource(dst, src);
        const plume::RenderTextureBarrier after[] = {
          {source->texture.get(), plume::RenderTextureLayout::DEPTH_WRITE},
          {adopted_destination->texture.get(), plume::RenderTextureLayout::SHADER_READ}};
        commands->barriers(plume::RenderBarrierStage::GRAPHICS, after, 2);
        adopted_destination->resolved_on_host = true;
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
        static_cast<i32>(resolve_flags) >> 26, fetch.num_format != 0);
    if (!HostDevice::ResolveHdrColor(source->texture.get(), source->descriptor_index,
            adopted_destination->texture.get(), adopted_destination->width,
            adopted_destination->height, scale, ColorResolveDestination::kUnormRGBA16,
            true, &*region))
      return false;
    adopted_destination->resolved_on_host = true;
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
                                              fetch.num_format != 0);
    if (!HostDevice::ResolveHdrColor(source->texture.get(), source->descriptor_index,
                                    adopted_destination->texture.get(),
                                    adopted_destination->width, adopted_destination->height, scale,
                                    adopted_destination->format == plume::RenderFormat::R16G16_UNORM
                                        ? ColorResolveDestination::kUnormRG16
                                        : ColorResolveDestination::kUnormRGBA16,
                                    true, &*region)) return false;
    adopted_destination->resolved_on_host = true;
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
  resolved_destination->resolved_on_host = true;
  return UpdateResolvedSampling(*resolved_destination);
}

bool PromoteTiledSurface(u32 guest_address, u32 width, u32 height) {
  auto recording = HostDevice::LockRecording();
  if (!guest_address) return true;
  const auto resource = FindTexture(guest_address);
  if (!resource || !resource->surface || resource->width != width ||
      resource->height > height || !width || !height) return false;
  if (resource->HostWidth() == width && resource->HostHeight() == height) return true;
  TextureResource description;
  description.width = width; description.height = height;
  description.surface = true; description.format = resource->format;
  description.d3d_type = resource->d3d_type;
  auto texture = CreateHostTexture(description, 1, 0);
  if (!texture) return false;
  texture->setName("LEGO full logical tiled surface");
  plume::RenderTextureViewDesc view_desc;
  view_desc.format = resource->format;
  view_desc.dimension = plume::RenderTextureViewDimension::TEXTURE_2D;
  view_desc.mipLevels = 1;
  auto view = texture->createTextureView(view_desc);
  const u32 descriptor = HostDevice::RegisterTexture(texture.get(), view.get());
  if (descriptor == ~u32{0}) return false;
  // Cached framebuffers contain physical texture pointers, not guest addresses.
  InvalidateFramebufferReferences(guest_address);
  auto retired = std::make_shared<SurfaceStorage>();
  {
    std::lock_guard lock(resource->mutex);
    retired->texture = std::move(resource->texture);
    retired->view = std::move(resource->view);
    resource->previous_surface_descriptors.push_back(resource->descriptor_index);
    resource->texture = std::move(texture);
    resource->view = std::move(view);
    resource->descriptor_index = descriptor;
    resource->host_width = width; resource->host_height = height;
  }
  HostDevice::RetireResource(std::move(retired));
  REXLOG_INFO("Native GPU: promoted tiled surface {:08X} guest={}x{} host={}x{} depth={}",
      guest_address, resource->width, resource->height, width, height, IsDepthFormat(resource->format));
  return true;
}

plume::RenderFramebuffer* ResolveFramebuffer(u32 render_target, u32 depth_stencil) {
  return ResolveFramebuffer(std::array<u32, 4>{render_target, 0, 0, 0}, depth_stencil);
}

plume::RenderFramebuffer* ResolveFramebuffer(
    const std::array<u32, 4>& render_targets, u32 depth_stencil) {
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
  const FramebufferKey key{render_targets, depth_stencil};
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
  g_framebuffer_owners[owner->guest_address] = owner;
  owner->framebuffers.emplace(key, std::move(framebuffer));
  return result;
}

bool IsNativeTexture(u32 guest_address) {
  std::lock_guard lock(g_textures_mutex);
  return g_textures.contains(guest_address);
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
  auto recording = HostDevice::LockRecording();
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
  if (g_texture_lifecycle_logs.fetch_add(1, std::memory_order_relaxed) < 64) {
    REXLOG_INFO("Native GPU: released {} 0x{:08X}",
                released->surface ? "surface" : "texture", guest_address);
  }
  auto* memory = REX_KERNEL_MEMORY();
  InvalidateFramebufferReferences(guest_address);
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
  return 0;
}

void ResetTextureResources() {
  auto recording = HostDevice::LockRecording();
  g_framebuffer_owners.clear();
  g_depth_resolves.clear();
  g_edram_colors.clear();
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
bool TextureDataOverlapsPoolCopy(const TextureResource& resource, PhysicalCopyRange range) {
  if (resource.surface) return false;  // EDRAM has no physical-memory backing.
  if (resource.owns_guest_memory)
    return GuestRangeOverlapsPoolCopy(resource.mirror_address, resource.mirror_size, range);
  namespace tu = rex::graphics::texture_util;
  const auto& fetch = resource.guest_fetch;
  if (!fetch.base_address || !resource.levels) return false;
  const auto layout = tu::GetGuestTextureLayout(fetch.dimension, fetch.pitch,
      resource.width, resource.height,
      fetch.dimension == rex::graphics::xenos::DataDimension::k3D ? resource.depth : 1,
      fetch.tiled, fetch.format, fetch.packed_mips, true, resource.levels - 1);
  return GuestRangeOverlapsPoolCopy(fetch.base_address << 12, layout.base.level_data_extent_bytes, range) ||
      (resource.levels > 1 && fetch.mip_address &&
       GuestRangeOverlapsPoolCopy(fetch.mip_address << 12, layout.mips_total_extent_bytes, range));
}
}  // namespace

bool PoolCopyHasHostTextureSource(PhysicalCopyRange source) {
  auto recording = HostDevice::LockRecording();
  std::lock_guard lock(g_textures_mutex);
  for (const auto& [address, resource] : g_textures) {
    std::lock_guard texture_lock(resource->mutex);
    if ((resource->resolved_on_host || resource->depth_alias_generation) &&
        (TextureDataOverlapsPoolCopy(*resource, source) ||
         GuestRangeOverlapsPoolCopy(address, sizeof(D3DTexture), source)))
      return true;
  }
  return false;
}

u32 InvalidatePoolCopyTextures(PhysicalCopyRange destination) {
  auto recording = HostDevice::LockRecording();
  std::vector<std::shared_ptr<TextureResource>> retired;
  u32 count = 0;
  {
    std::lock_guard lock(g_textures_mutex);
    for (auto it = g_textures.begin(); it != g_textures.end();) {
      const auto& resource = it->second;
      std::lock_guard texture_lock(resource->mutex);
      if (!resource->owns_guest_memory &&
          GuestRangeOverlapsPoolCopy(resource->guest_address, sizeof(D3DTexture), destination)) {
        retired.push_back(std::move(it->second));
        it = g_textures.erase(it);
        ++count;
      } else {
        if (TextureDataOverlapsPoolCopy(*resource, destination)) {
          resource->guest_uploaded = false;
          resource->upload_source_key_valid = false;
          resource->depth_alias_generation = 0;
          ++count;
        }
        ++it;
      }
    }
  }
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
  return count;
}

}  // namespace legodimensions::gpu_native
