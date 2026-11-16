#include "gpu_native/native_recording.h"
#include "gpu_native/device.h"
#include "gpu_native/d3d12_support.h"
#include "gpu_native/shader_archive.h"
#include <atomic>
#include "gpu_native/scaled_resolve.h"
#include "gpu_native/depth_resample.h"
#include "gpu_native/descriptor_retirement.h"
#include "gpu_native/completion_queue.h"
#include "gpu_native/present_rect.h"
#include "gpu_native/gpu_timestamps.h"
#include "gpu_native/fps_counter.h"
#include "gpu_native/fsr1_present.h"
#include "gpu_native/display_gamma_gpu.h"
#include <rex/perf/counter.h>
#include "gpu_native/native_overlay.h"
#include <rex/ui/overlay/debug_overlay.h>

#include "gpu_native/buffers.h"
#include "gpu_native/queries.h"
#include "gpu_native/draw.h"
#include "gpu_native/fsr3_settings.h"
#include "gpu_native/settings_runtime.h"
#include "gpu_native/shaders.h"
#include "gpu_native/textures.h"
#include "gpu_native/state.h"
#include "gpu_native/vertex_declarations.h"
#include "gpu_native/shaders/copy_color_ps.h"
#include "gpu_native/shaders/copy_vs.h"
#include "gpu_native/shaders/resolve_color_ps.h"
#include "gpu_native/shaders/depth_pack_ps.h"
#include "gpu_native/shaders/depth_restore_ps.h"
#include "gpu_native/shaders/resolve_scaled_ps.h"
#include "gpu_native/shaders/depth_resample_ps.h"
#include "gpu_native/shaders/depth_resample_depth_ps.h"
#include "gpu_native/shaders/depth_resample_stencil_ps.h"
#include "native_gpu_build_info.h"
#include "native_constants.h"
#include "gpu_native/long_probe.h"
#include "gpu_native/memory_watch.h"
#include "gpu_native/renderdoc_probe.h"
#include "gpu_native/pipeline_library.h"
#include "gpu_native/native_worker.h"
#include <intrin.h>
#include <rex/cvar.h>
#include <rex/filesystem.h>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cmath>

#include <array>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <plume_render_interface.h>
#include <plume_render_interface_builders.h>
#include <plume_d3d12.h>
#include <d3d12sdklayers.h>
#include <cstdlib>
#include <rex/logging.h>
#include <rex/ui/window.h>

namespace plume {
std::unique_ptr<RenderInterface> CreateD3D12Interface();
}  // namespace plume

REXCVAR_DEFINE_INT32(native_fps_limit, 240, "Graphics", "Native presentation cap; 0 is uncapped")
    .range(0,1000);
REXCVAR_DEFINE_BOOL(native_vsync, false, "Graphics",
    "Synchronize native presentation to the display refresh rate. Off allows tearing and uncapped presentation.");
REXCVAR_DEFINE_BOOL(native_show_fps, true, "Graphics",
    "Show the native FPS meter in the bottom-left corner. Applies between frames.");
REXCVAR_DEFINE_INT32(native_anisotropic_override, -2, "Graphics",
    "Native texture filtering: -2 inherits the installation setting, -1 uses the game, 0 disables anisotropy, 1..5 force 1x..16x. Applies between frames.")
    .range(-2, 5);

namespace legodimensions::gpu_native {
namespace {

bool NativeVsyncRequested() {
  if (const char* value = std::getenv("LEGO_NATIVE_VSYNC"); value && *value)
    return value[0] != '0';
  return rex::cvar::Query<bool>("native_vsync");
}

int32_t NativeInheritedAnisotropyImpl() {
  const auto parse = [](const std::string& text, int32_t fallback) {
    try { return int32_t(std::stoi(text)); } catch (...) { return fallback; }
  };
  // Preserve existing installations even when the plugin which defines the
  // legacy setting is absent. File IO stays outside the draw/sampler path.
  static const int32_t from_config = [&] {
    std::ifstream config(rex::filesystem::GetExecutableFolder() / "legodimensions.toml");
    std::string line;
    while (std::getline(config, line)) {
      const auto key = line.find_first_not_of(" \t");
      if (key == std::string::npos || line.compare(key, 20, "anisotropic_override") != 0) continue;
      const auto eq = line.find('=', key + 20);
      if (eq != std::string::npos) return parse(line.substr(eq + 1), -1);
    }
    return int32_t{-1};
  }();
  const int32_t inherited = parse(rex::cvar::GetFlagByName("anisotropic_override"), from_config);
  return inherited >= 0 && inherited <= 5 ? inherited : -1;
}
int32_t NativeAnisotropicRequested() {
  try {
    const auto selected = std::stoi(rex::cvar::GetFlagByName("native_anisotropic_override"));
    if (selected >= -1 && selected <= 5) return selected;
  } catch (...) {}
  return NativeInheritedAnisotropyImpl();
}

struct GpuSnapshot {
  std::unique_ptr<plume::RenderBuffer> readback;
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
  u32 row_bytes = 0;
  u32 dxgi_format = 0;
  u32 capture_frame = 0;
  std::filesystem::path path;
};

struct DrawUploadPage {
  std::unique_ptr<plume::RenderBuffer> buffer;
  u8* mapped = nullptr;
  u64 size = 0;
  ~DrawUploadPage() { if (mapped) buffer->unmap(); }
};

const std::filesystem::path& SnapshotDirectory() {
  static const std::filesystem::path path = [] {
    char* value = nullptr;
    size_t length = 0;
    _dupenv_s(&value, &length, "LEGO_GPU_SNAPSHOT_DIR");
    std::filesystem::path result(value ? value : "");
    std::free(value);
    return result;
  }();
  return path;
}

void SaveSnapshots(std::vector<GpuSnapshot>& snapshots) {
  for (auto& sample : snapshots) {
    const auto* data = static_cast<const u8*>(sample.readback->map());
    if (!data) continue;
    const auto& fp = sample.footprint.Footprint;
    u32 header[37]{};  // DDS + DX10 extension, one 2D base level.
    header[0] = 0x20534444; header[1] = 124; header[2] = 0x100F;
    header[3] = fp.Height; header[4] = fp.Width; header[5] = sample.row_bytes;
    header[19] = 32; header[20] = 4; header[21] = 0x30315844;
    header[27] = 0x1000; header[32] = sample.dxgi_format;
    header[33] = 3; header[35] = 1;
    std::ofstream file(sample.path, std::ios::binary);
    file.write(reinterpret_cast<const char*>(header), sizeof(header));
    for (u32 y = 0; y < fp.Height; ++y)
      file.write(reinterpret_cast<const char*>(data + sample.footprint.Offset +
                                               u64(y) * fp.RowPitch), sample.row_bytes);
    if (LongProbeEnabled() && sample.path.filename().string().ends_with("-present.dds") &&
        (sample.dxgi_format==28 || sample.dxgi_format==87)) {
      double sum=0,edge=0; u64 black=0,white=0,count=0,edges=0;
      const auto luminance=[&](u32 x,u32 y) {
        const auto* p=data+sample.footprint.Offset+u64(y)*fp.RowPitch+4*x;
        return (double(p[0])+p[1]+p[2])/(3*255.0);
      };
      for(u32 y=0;y<fp.Height;y+=4) for(u32 x=0;x<fp.Width;x+=4) {
        const double l=luminance(x,y); sum+=l; ++count;
        black+=l<0.005; white+=l>0.995;
        if(x+4<fp.Width) { edge+=std::abs(l-luminance(x+4,y)); ++edges; }
      }
      const double black_fraction=double(black)/count, white_fraction=double(white)/count;
      LongProbeEvent("present_pixels", black_fraction>0.995 || white_fraction>0.995,
          "captured_frame=", sample.capture_frame, "file=", sample.path.filename().string(),
          "mean=", sum/count, "edge=", edge/std::max<u64>(1,edges),
          "black_fraction=", black_fraction, "white_fraction=", white_fraction);
    }
    if (LongProbeEnabled()) LongProbeEvent("snapshot_saved", false,
        "captured_frame=", sample.capture_frame, "file=", sample.path.filename().string(),
        "width=", fp.Width, "height=", fp.Height, "dxgi=", sample.dxgi_format);
    sample.readback->unmap();
    if (file) REXLOG_INFO("Native GPU: saved snapshot {}", sample.path.string());
  }
  snapshots.clear();
}

struct SamplerKeyHash {
  size_t operator()(const NativeSamplerKey& key) const {
    size_t value = 0;
    for (u32 word : key.words) value = (value * 16777619u) ^ word;
    return value;
  }
};

struct CachedNativeSampler {
  u32 descriptor = 0;
  std::unique_ptr<plume::RenderSampler> sampler;
};

u32 NativeCommandSlotCount() {
  const char* value = std::getenv("LEGO_NATIVE_COMMAND_SLOTS");
  if (!value || std::strcmp(value, "3") == 0) return 3;
  if (std::strcmp(value, "12") == 0) return 12;
  REXLOG_WARN("Native GPU: LEGO_NATIVE_COMMAND_SLOTS must be 3 or 12; using 3");
  return 3;
}

struct PresentGammaSurface {
  std::unique_ptr<plume::RenderTexture> texture;
  std::unique_ptr<plume::RenderTextureView> view;
  u32 descriptor = ~u32{0};
};

struct State {
  // Guest kickoffs consume slots independently of swap-chain presentation.
  // Extra slots are an opt-in experiment; every slot keeps its own fence.
  static constexpr u32 kMaxCommandSlots = 12;
  const u32 command_slot_count = NativeCommandSlotCount();

  std::unique_ptr<plume::RenderInterface> render_interface;
  std::unique_ptr<plume::RenderDevice> device;
  std::unique_ptr<plume::RenderCommandQueue> queue;
  std::unique_ptr<plume::RenderSwapChain> swap_chain;
  std::unique_ptr<plume::RenderCommandFence> idle_fence;
  std::unique_ptr<plume::RenderPipelineLayout> pipeline_layout;
  std::unique_ptr<plume::RenderDescriptorSet> texture_descriptors;
  std::unique_ptr<plume::RenderDescriptorSet> sampler_descriptors;
  std::unique_ptr<plume::RenderSampler> default_sampler;
  std::unordered_map<NativeSamplerKey, CachedNativeSampler, SamplerKeyHash> samplers;
  // Only successful immutable sampler IDs are reused. Texture preparation and
  // fetch reconciliation still run every draw before this per-device cache.
  std::array<TextureFetchWords, 32> sampler_last_fetches{};
  std::array<u32, 32> sampler_last_descriptors{};
  std::array<int32_t, 32> sampler_last_overrides{};
  u32 sampler_error_logs = 0;
  std::unique_ptr<plume::RenderBuffer> null_vertex_buffer;
  std::array<std::unique_ptr<plume::RenderTexture>, 3> null_textures;
  std::array<std::unique_ptr<plume::RenderTextureView>, 3> null_texture_views;
  std::unique_ptr<plume::RenderBuffer> null_texture_upload;
  bool null_textures_initialized = false;
  std::unique_ptr<plume::RenderShader> copy_vertex_shader;
  std::unique_ptr<plume::RenderShader> copy_pixel_shader;
  std::unique_ptr<plume::RenderPipeline> copy_pipeline;
  std::unique_ptr<Fsr1Presenter> fsr1_presenter;
  std::unique_ptr<DisplayGammaGpu> gamma_presenter;
  std::shared_ptr<PresentGammaSurface> gamma_surface;
  DisplayGammaTable gamma_table;
  bool gamma_table_valid = false;
  std::unique_ptr<plume::RenderShader> resolve_pixel_shader;
  std::unique_ptr<plume::RenderShader> scaled_resolve_pixel_shader;
  std::array<std::unique_ptr<plume::RenderPipeline>, 5> resolve_pipelines;
  std::array<std::unique_ptr<plume::RenderShader>, 2> depth_alias_shaders;
  std::array<std::unique_ptr<plume::RenderPipeline>, 2> depth_alias_pipelines;
  std::unique_ptr<DepthResampler> depth_resampler;
  std::vector<std::unique_ptr<plume::RenderFramebuffer>> framebuffers;
  std::array<std::unique_ptr<plume::RenderCommandList>, kMaxCommandSlots>
      command_lists;
  std::array<std::unique_ptr<plume::RenderCommandFence>, kMaxCommandSlots>
      frame_fences;
  std::array<std::unique_ptr<plume::RenderCommandSemaphore>, kMaxCommandSlots>
      acquire_semaphores;
  std::vector<std::unique_ptr<plume::RenderCommandSemaphore>>
      render_semaphores;
  std::array<bool, kMaxCommandSlots> frame_submitted{};
  std::array<u64, kMaxCommandSlots> slot_submission{};
  u64 last_submission = 0, completed_submission = 0;
  std::unique_ptr<GpuSubmissionTimestamps> gpu_timestamps;
  CompletionQueue completion_callbacks;
  u64 callbacks_enqueued = 0, callbacks_executed = 0;
  std::array<std::vector<std::unique_ptr<DrawUploadPage>>, kMaxCommandSlots> draw_uploads;
  u32 draw_upload_page = 0;
  u64 draw_upload_offset = 0;
  std::array<std::vector<std::shared_ptr<void>>, kMaxCommandSlots>
      retired_resources;
  u32 frame_slot = 0;
  u32 present_number = 0;
  NativeFpsCounter fps_counter;
  u32 snapshot_number = 0;
  u32 snapshot_draw_number = 0;
  u32 snapshot_start_frame = ~0u;
  std::chrono::steady_clock::time_point snapshot_start_time;
  u32 snapshot_time_window = 0;
  u32 snapshot_selected_frame = ~0u;
  std::chrono::steady_clock::time_point snapshot_trigger_check;
  std::array<std::vector<GpuSnapshot>, kMaxCommandSlots> snapshots;
  bool command_list_open = false;
  u32 long_wait_log_count = 0;
  u32 timing_log_count = 0;
  u64 sync_calls = 0, cpu_resource_wait_skips = 0;
  double sync_ms = 0;
  u64 frame_slot_wait_calls = 0;
  double frame_slot_wait_ms = 0;
  double acquire_cpu_ms = 0;
  double present_submit_cpu_ms = 0;
  double swap_present_cpu_ms = 0;
  std::array<u64, static_cast<u32>(SyncReason::kCount)> sync_reason_calls{};
  std::array<double, static_cast<u32>(SyncReason::kCount)> sync_reason_ms{};
  std::chrono::steady_clock::time_point probe_next_snapshot{};
  std::chrono::steady_clock::time_point probe_next_present{};
  u32 probe_present_burst_remaining = 0;
  std::chrono::steady_clock::time_point probe_last_anomaly_snapshot{};
  u64 probe_snapshot_bytes = 0;
  bool probe_budget_reported = false;
  std::chrono::steady_clock::time_point last_present_time{};
  std::vector<bool> texture_slots;
  DescriptorRetirement retired_descriptors{65536, command_slot_count};
  Backend backend = Backend::kD3D12;
};

constexpr u32 kBindlessTextureCount = 65536;
// Plume's D3D12 sampler heap is 1024 entries. Requesting 2048 makes its
// allocator return INVALID_OFFSET; a subsequent Release-build setSampler then
// writes an invalid CPU descriptor handle inside the display driver.
constexpr u32 kBindlessSamplerCount = 1024;
constexpr u32 kFirstTextureSlot = 3;

std::mutex g_mutex;
std::unique_ptr<State> g_state;
std::atomic<u64> g_state_generation{1};
std::atomic<int32_t> g_anisotropic_override{-1};
// Guards State::texture_slots and g_texture_slot_cursor; taken after g_mutex and around every g_state replacement.
std::mutex g_texture_slots_mutex;
u32 g_texture_slot_cursor = 0;
std::atomic<bool> g_ready{false};  // IsReady's predicate, published with g_state
// Accessed under g_mutex. Do not reset on device recreation: a command-list
// address may be reused, but its recording identity must never be reused.
u64 g_recording_serial = 0;

std::unique_ptr<plume::RenderInterface> CreateInterface(Backend backend) {
  switch (backend) {
    case Backend::kD3D12:
      return plume::CreateD3D12Interface();
    case Backend::kVulkan:
      // The frontend deliberately has a Vulkan slot. Its Plume implementation
      // is compiled back in once the D3D12 command path is stable.
      return nullptr;
  }
  return nullptr;
}

const char* NameOf(Backend backend) {
  return backend == Backend::kD3D12 ? "D3D12" : "Vulkan";
}

bool CreatePipelineLayout(State& state) {
  plume::RenderPipelineLayoutBuilder layout;
  layout.begin(false, true);

  plume::RenderDescriptorSetBuilder textures;
  textures.begin();
  textures.addTexture(0, kBindlessTextureCount);
  textures.end(true, kBindlessTextureCount);
  state.texture_descriptors = textures.create(state.device.get());
  if (!state.texture_descriptors) {
    return false;
  }
  layout.addDescriptorSet(textures);
  layout.addDescriptorSet(textures);
  layout.addDescriptorSet(textures);

  plume::RenderDescriptorSetBuilder samplers;
  samplers.begin();
  samplers.addSampler(0, kBindlessSamplerCount);
  samplers.end(true, kBindlessSamplerCount);
  state.sampler_descriptors = samplers.create(state.device.get());
  if (!state.sampler_descriptors) {
    return false;
  }
  layout.addDescriptorSet(samplers);

  layout.addRootDescriptor(0, 4,
                           plume::RenderRootDescriptorType::CONSTANT_BUFFER);
  layout.addRootDescriptor(1, 4,
                           plume::RenderRootDescriptorType::CONSTANT_BUFFER);
  layout.addRootDescriptor(2, 4,
                           plume::RenderRootDescriptorType::CONSTANT_BUFFER);
  layout.addRootDescriptor(4, 4,
                           plume::RenderRootDescriptorType::CONSTANT_BUFFER);
  // Per-instance VS float constants read by the instanced-constants variants.
  layout.addRootDescriptor(0, 5,
                           plume::RenderRootDescriptorType::SHADER_RESOURCE);
  layout.addPushConstant(3, 4, sizeof(u32) * 4,
                         plume::RenderShaderStageFlag::PIXEL);
  layout.end();
  state.pipeline_layout = layout.create(state.device.get());
  if (!state.pipeline_layout ||
      !static_cast<plume::D3D12PipelineLayout*>(state.pipeline_layout.get())->rootSignature) {
    state.pipeline_layout.reset();
    REXLOG_ERROR("Native GPU: D3D12 rejected the root signature");
    return false;
  }

  plume::RenderSamplerDesc sampler_desc;
  sampler_desc.minFilter = plume::RenderFilter::LINEAR;
  sampler_desc.magFilter = plume::RenderFilter::LINEAR;
  sampler_desc.mipmapMode = plume::RenderMipmapMode::LINEAR;
  sampler_desc.addressU = plume::RenderTextureAddressMode::CLAMP;
  sampler_desc.addressV = plume::RenderTextureAddressMode::CLAMP;
  sampler_desc.addressW = plume::RenderTextureAddressMode::CLAMP;
  state.default_sampler = state.device->createSampler(sampler_desc);
  if (!state.default_sampler) {
    return false;
  }
  state.sampler_descriptors->setSampler(0, state.default_sampler.get());
  state.texture_slots.assign(kBindlessTextureCount, false);
  for (u32 i = 0; i < kFirstTextureSlot; ++i) {
    state.texture_slots[i] = true;
  }
  return true;
}

bool CreatePresentPipeline(State& state) {
  REXLOG_INFO("Native GPU init: creating present shaders");
  state.copy_vertex_shader = state.device->createShader(
      g_copy_vs_dxil, sizeof(g_copy_vs_dxil), "main",
      plume::RenderShaderFormat::DXIL);
  state.copy_pixel_shader = state.device->createShader(
      g_copy_color_ps_dxil, sizeof(g_copy_color_ps_dxil), "main",
      plume::RenderShaderFormat::DXIL);
  if (!state.copy_vertex_shader || !state.copy_pixel_shader) {
    return false;
  }

  plume::RenderGraphicsPipelineDesc desc;
  desc.pipelineLayout = state.pipeline_layout.get();
  desc.vertexShader = state.copy_vertex_shader.get();
  desc.pixelShader = state.copy_pixel_shader.get();
  desc.depthFunction = plume::RenderComparisonFunction::ALWAYS;
  desc.depthEnabled = false;
  desc.depthWriteEnabled = false;
  desc.primitiveTopology = plume::RenderPrimitiveTopology::TRIANGLE_LIST;
  desc.cullMode = plume::RenderCullMode::NONE;
  desc.fillMode = plume::RenderFillMode::SOLID;
  desc.renderTargetCount = 1;
  desc.renderTargetFormat[0] = plume::RenderFormat::B8G8R8A8_UNORM;
  desc.renderTargetBlend[0] = plume::RenderBlendDesc::Copy();
  REXLOG_INFO("Native GPU init: creating present pipeline");
  state.copy_pipeline = state.device->createGraphicsPipeline(desc);
  if (state.copy_pipeline &&
      !static_cast<plume::D3D12GraphicsPipeline*>(state.copy_pipeline.get())->d3d) {
    state.copy_pipeline.reset();
  }
  REXLOG_INFO("Native GPU init: present pipeline {}",
              state.copy_pipeline ? "ready" : "failed");
  return state.copy_pipeline != nullptr;
}

bool CreateFrameRing(State& state) {
  REXLOG_INFO("Native GPU init: creating {} command slots (swap-chain images unchanged)",
              state.command_slot_count);
  const u32 texture_count = state.swap_chain->getTextureCount();
  if (!texture_count) return false;
  // Resize has already discarded the old swap-chain framebuffers. Publish a
  // new ring only when every allocation succeeds, so an empty framebuffer
  // list remains the retry marker after a partial failure.
  decltype(state.framebuffers) framebuffers;
  decltype(state.render_semaphores) render_semaphores;
  decltype(state.command_lists) command_lists;
  decltype(state.frame_fences) frame_fences;
  decltype(state.acquire_semaphores) acquire_semaphores;
  framebuffers.reserve(texture_count);
  render_semaphores.reserve(texture_count);
  for (u32 i = 0; i < texture_count; ++i) {
    const plume::RenderTexture* attachment = state.swap_chain->getTexture(i);
    framebuffers.emplace_back(state.device->createFramebuffer(
        plume::RenderFramebufferDesc(&attachment, 1)));
    render_semaphores.emplace_back(
        state.device->createCommandSemaphore());
    if (!framebuffers.back() || !render_semaphores.back()) {
      return false;
    }
  }

  for (u32 i = 0; i < state.command_slot_count; ++i) {
    command_lists[i] = PrepareNativeCommandList(state.queue->createCommandList());
    frame_fences[i] = state.device->createCommandFence();
    acquire_semaphores[i] = state.device->createCommandSemaphore();
    if (!command_lists[i] || !frame_fences[i] || !acquire_semaphores[i]) {
      return false;
    }
  }
  state.framebuffers = std::move(framebuffers);
  state.render_semaphores = std::move(render_semaphores);
  state.command_lists = std::move(command_lists);
  state.frame_fences = std::move(frame_fences);
  state.acquire_semaphores = std::move(acquire_semaphores);
  for (u32 i = 0; i < state.command_slot_count; ++i) {
    state.frame_submitted[i] = false;
    state.retired_resources[i].clear();
    std::lock_guard slots(g_texture_slots_mutex);
    state.retired_descriptors.CompleteFrame(i,
        [&](u32 index) { state.texture_slots[index] = false; });
  }
  state.frame_slot = 0;
  return true;
}

bool CreateNullVertexBuffer(State& state) {
  REXLOG_INFO("Native GPU init: creating null vertex buffer");
  state.null_vertex_buffer = state.device->createBuffer(
      plume::RenderBufferDesc::VertexBuffer(256,
                                             plume::RenderHeapType::UPLOAD));
  if (!state.null_vertex_buffer) {
    return false;
  }
  void* mapped = state.null_vertex_buffer->map();
  if (!mapped) {
    return false;
  }
  std::memset(mapped, 0, 256);
  state.null_vertex_buffer->unmap();
  return true;
}

bool CreateNullTextures(State& state) {
  REXLOG_INFO("Native GPU init: creating null textures");
  const plume::RenderTextureDimension dimensions[] = {
      plume::RenderTextureDimension::TEXTURE_2D,
      plume::RenderTextureDimension::TEXTURE_3D,
      plume::RenderTextureDimension::TEXTURE_2D};
  const plume::RenderTextureViewDimension view_dimensions[] = {
      plume::RenderTextureViewDimension::TEXTURE_2D,
      plume::RenderTextureViewDimension::TEXTURE_3D,
      plume::RenderTextureViewDimension::TEXTURE_CUBE};
  for (u32 i = 0; i < 3; ++i) {
    REXLOG_INFO("Native GPU init: null texture {} resource", i);
    plume::RenderTextureDesc desc;
    desc.dimension = dimensions[i];
    desc.width = desc.height = desc.depth = desc.mipLevels = 1;
    desc.arraySize = i == 2 ? 6 : 1;
    desc.format = plume::RenderFormat::R8G8B8A8_UNORM;
    if (i == 2) {
      desc.flags = plume::RenderTextureFlag::CUBE;
    }
    state.null_textures[i] = state.device->createTexture(desc);
    if (!state.null_textures[i]) {
      return false;
    }
    plume::RenderTextureViewDesc view_desc;
    view_desc.format = desc.format;
    view_desc.dimension = view_dimensions[i];
    view_desc.mipLevels = 1;
    state.null_texture_views[i] =
        state.null_textures[i]->createTextureView(view_desc);
    if (!state.null_texture_views[i]) {
      return false;
    }
    REXLOG_INFO("Native GPU init: null texture {} descriptor", i);
    state.texture_descriptors->setTexture(
        i, state.null_textures[i].get(),
        plume::RenderTextureLayout::SHADER_READ,
        state.null_texture_views[i].get());
  }
  // Six cube faces plus the 2D and 3D resources, each with a 512-byte aligned
  // placed footprint.
  state.null_texture_upload = state.device->createBuffer(
      plume::RenderBufferDesc::UploadBuffer(8 * 0x200));
  if (!state.null_texture_upload) {
    return false;
  }
  void* mapped = state.null_texture_upload->map();
  if (!mapped) {
    return false;
  }
  std::memset(mapped, 0, 8 * 0x200);
  state.null_texture_upload->unmap();
  return true;
}

void InitializeNullTextures(State& state, plume::RenderCommandList* commands) {
  if (state.null_textures_initialized) {
    return;
  }
  plume::RenderTextureBarrier pre[3];
  for (u32 i = 0; i < 3; ++i) {
    pre[i] = plume::RenderTextureBarrier(
        state.null_textures[i].get(), plume::RenderTextureLayout::COPY_DEST);
  }
  commands->barriers(plume::RenderBarrierStage::COPY, pre, 3);
  u32 footprint = 0;
  commands->copyTextureRegion(
      plume::RenderTextureCopyLocation::Subresource(
          state.null_textures[0].get()),
      plume::RenderTextureCopyLocation::PlacedFootprint(
          state.null_texture_upload.get(), plume::RenderFormat::R8G8B8A8_UNORM,
          1, 1, 1, 64, footprint++ * 0x200));
  commands->copyTextureRegion(
      plume::RenderTextureCopyLocation::Subresource(
          state.null_textures[1].get()),
      plume::RenderTextureCopyLocation::PlacedFootprint(
          state.null_texture_upload.get(), plume::RenderFormat::R8G8B8A8_UNORM,
          1, 1, 1, 64, footprint++ * 0x200));
  for (u32 face = 0; face < 6; ++face) {
    commands->copyTextureRegion(
        plume::RenderTextureCopyLocation::Subresource(
            state.null_textures[2].get(), 0, face),
        plume::RenderTextureCopyLocation::PlacedFootprint(
            state.null_texture_upload.get(),
            plume::RenderFormat::R8G8B8A8_UNORM, 1, 1, 1, 64,
            footprint++ * 0x200));
  }
  plume::RenderTextureBarrier post[3];
  for (u32 i = 0; i < 3; ++i) {
    post[i] = plume::RenderTextureBarrier(
        state.null_textures[i].get(), plume::RenderTextureLayout::SHADER_READ);
  }
  commands->barriers(plume::RenderBarrierStage::GRAPHICS, post, 3);
  state.null_textures_initialized = true;
}

void RefreshCompletedSubmissionsLocked(State& state) {
  if (state.backend != Backend::kD3D12) return;
  for (u32 slot = 0; slot < state.command_slot_count; ++slot) {
    if (!state.frame_submitted[slot]) continue;
    auto* fence = static_cast<plume::D3D12CommandFence*>(state.frame_fences[slot].get());
    const u64 completed = fence->d3d->GetCompletedValue();
    if (state.gpu_timestamps)
      state.gpu_timestamps->Complete(slot, completed,
          fence->fenceValue > 1 ? fence->fenceValue - 1 : 0,
          state.present_number);
    // Plume signals fenceValue, then increments it. UINT64_MAX is removal,
    // never successful completion. Do not consume its auto-reset wait event.
    if (completed != UINT64_MAX && fence->fenceValue > 1 &&
        completed >= fence->fenceValue - 1) {
      state.completed_submission = std::max(state.completed_submission,
                                           state.slot_submission[slot]);
    }
  }
}

void MarkSubmissionLocked(State& state, u32 slot) {
  state.frame_submitted[slot] = true;
  state.slot_submission[slot] = ++state.last_submission;
  if (state.gpu_timestamps)
    state.gpu_timestamps->Submitted(slot, state.slot_submission[slot]);
}

bool WaitForSubmissionLocked(State& state, u32 slot) {
  if (!state.frame_submitted[slot]) return true;
  if (state.backend == Backend::kD3D12) {
    auto* fence = static_cast<plume::D3D12CommandFence*>(state.frame_fences[slot].get());
    if (fence->d3d->GetCompletedValue() == UINT64_MAX) {
      if (state.gpu_timestamps)
        state.gpu_timestamps->Complete(slot, UINT64_MAX,
            fence->fenceValue > 1 ? fence->fenceValue - 1 : 0,
            state.present_number);
      REXLOG_ERROR("Native GPU: cannot wait for submission after device removal");
      return false;
    }
  }
  state.queue->waitForCommandFence(state.frame_fences[slot].get());
  if (state.backend == Backend::kD3D12) {
    auto* fence = static_cast<plume::D3D12CommandFence*>(state.frame_fences[slot].get());
    const u64 completed = fence->d3d->GetCompletedValue();
    if (state.gpu_timestamps)
      state.gpu_timestamps->Complete(slot, completed,
          fence->fenceValue > 1 ? fence->fenceValue - 1 : 0,
          state.present_number);
    if (completed == UINT64_MAX || completed < fence->fenceValue - 1) {
      REXLOG_ERROR("Native GPU: submission GPU wait failed");
      return false;
    }
  }
  // A void Plume wait can return on device removal or an event failure. Only
  // a verified completion permits resource retirement or allocator reuse.
  state.completed_submission = std::max(state.completed_submission,
                                       state.slot_submission[slot]);
  state.frame_submitted[slot] = false;
  return true;
}

bool WaitForSubmittedFramesLocked(State& state) {
  // DXGI's frame-latency object is a present throttle, not a GPU completion
  // fence. Rebuilding/releasing the ring requires all submitted GPU users.
  for (u32 slot = 0; slot < state.command_slot_count; ++slot) {
    if (!WaitForSubmissionLocked(state, slot)) return false;
  }
  RefreshCompletedSubmissionsLocked(state);
  return true;
}

bool RebuildSwapChain(State& state) {
  if (state.command_list_open || !WaitForSubmittedFramesLocked(state)) return false;
  state.framebuffers.clear();
  state.render_semaphores.clear();
  state.frame_submitted.fill(false);
  if (!state.swap_chain->resize() || state.swap_chain->isEmpty()) {
    return false;
  }
  return CreateFrameRing(state);
}

plume::RenderCommandList* BeginFrameCommandsLocked(State& state) {
  if (state.command_list_open) {
    return state.command_lists[state.frame_slot].get();
  }
  // Reserve zero for a closed/invalid view and fail before any ring mutation
  // if the process-wide serial can no longer advance.
  if (g_recording_serial == UINT64_MAX) return nullptr;
  if ((state.framebuffers.empty() || state.swap_chain->needsResize()) &&
      !RebuildSwapChain(state)) {
    return nullptr;
  }

  const u32 slot = state.frame_slot;
  if (state.frame_submitted[slot]) {
    const bool timing_enabled = NativeTextureTimingEnabled();
    const auto wait_start = timing_enabled ? std::chrono::steady_clock::now()
        : std::chrono::steady_clock::time_point{};
    if (!WaitForSubmissionLocked(state, slot)) return nullptr;
    RefreshCompletedSubmissionsLocked(state);
    const auto wait_ms = timing_enabled ? std::chrono::duration<double, std::milli>(
                             std::chrono::steady_clock::now() - wait_start)
                             .count() : 0.0;
    ++state.frame_slot_wait_calls;
    if (timing_enabled) {
      state.frame_slot_wait_ms += wait_ms;
    }
    if (wait_ms > 20.0 && state.long_wait_log_count++ < 20) {
      REXLOG_WARN("Native GPU: frame-slot {} GPU wait took {:.2f} ms", slot,
                  wait_ms);
    }
    SaveSnapshots(state.snapshots[slot]);
  }
  state.retired_resources[slot].clear();
  state.draw_upload_page = 0;
  state.draw_upload_offset = 0;
  {
    std::lock_guard slots(g_texture_slots_mutex);
    state.retired_descriptors.CompleteFrame(slot,
        [&](u32 index) { state.texture_slots[index] = false; });
  }
  auto* commands = state.command_lists[slot].get();
  commands->begin();
  if (state.gpu_timestamps)
    state.gpu_timestamps->Begin(commands, slot, u64(state.present_number) + 1);
  InitializeNullTextures(state, commands);
  ++g_recording_serial;
  state.command_list_open = true;
  return commands;
}

}  // namespace

thread_local DrawPreparationContext* t_draw_preparation = nullptr;
DrawPreparationContext* CurrentDrawPreparation() { return t_draw_preparation; }
std::shared_ptr<void> FindPinnedOwner(unsigned char kind, u32 address) {
  if (!t_draw_preparation || !address) return {};
  for (const auto& pin : t_draw_preparation->pinned)
    if (pin.kind == kind && pin.address == address) return pin.owner;
  return {};
}
DrawPreparationScope::DrawPreparationScope(DrawPreparationContext& context)
    : previous_(std::exchange(t_draw_preparation, &context)) {
  NativeRecordingGate::EnterPreparation();
}
DrawPreparationScope::~DrawPreparationScope() {
  NativeRecordingGate::LeavePreparation();
  t_draw_preparation = previous_;
}
plume::RenderCommandList* NativeResourceCommands(plume::RenderCommandList* commands) {
  return t_draw_preparation ? t_draw_preparation->uploads : commands;
}

std::unique_lock<NativeRecordingGate> HostDevice::LockRecording() {
  // Threaded recording: an unconverted hook first lets every queued task run,
  // so it observes the same host state as the old inline path.
  if (!t_draw_preparation && !NativeRecordingGate::InPreparation()) NativeWorkerDrain(_ReturnAddress());
  return std::unique_lock(NativeRecordingMutex());
}

int32_t NativeInheritedAnisotropy() { return NativeInheritedAnisotropyImpl(); }
void SetNativeActiveAnisotropy(int32_t value) {
  g_anisotropic_override.store(value, std::memory_order_release);
}

bool HostDevice::Create(rex::ui::Window* window, Backend backend) {
  auto recording = LockRecording();
  std::lock_guard lock(g_mutex);
  if (g_state) {
    return true;
  }
  if (!window || !window->GetNativeWindowHandle()) {
    REXLOG_ERROR("Native GPU: no native window handle");
    return false;
  }

  auto state = std::make_unique<State>();
  REXLOG_INFO("Native GPU init: starting {}", NameOf(backend));
  state->backend = backend;
  InitializeLogoCapture();
  if (std::getenv("LEGO_NATIVE_GPU_DEBUG")) {
    ID3D12Debug* debug = nullptr;
    const HRESULT debug_result = D3D12GetDebugInterface(IID_PPV_ARGS(&debug));
    if (SUCCEEDED(debug_result)) {
      debug->EnableDebugLayer();
      if (const char* validation = std::getenv("LEGO_NATIVE_GPU_VALIDATION"); validation && std::strcmp(validation, "1") == 0) {
        ID3D12Debug1* gpu_debug = nullptr;
        if (SUCCEEDED(debug->QueryInterface(IID_PPV_ARGS(&gpu_debug)))) {
          gpu_debug->SetEnableGPUBasedValidation(TRUE);
          gpu_debug->Release();
          REXLOG_INFO("Native GPU: D3D12 GPU-based validation enabled");
        }
      }
      debug->Release();
      REXLOG_INFO("Native GPU: D3D12 debug layer enabled");
    } else {
      REXLOG_WARN("Native GPU: D3D12 debug layer unavailable HRESULT=0x{:08X}",
                  static_cast<u32>(debug_result));
    }
  }
  state->render_interface = CreateInterface(backend);
  if (!state->render_interface) {
    REXLOG_ERROR("Native GPU: failed to create {} interface", NameOf(backend));
    return false;
  }

  // LEGO_NATIVE_ADAPTER names an adapter exactly as DXGI describes it; plume otherwise takes the most video memory.
  const char* preferred_adapter = std::getenv("LEGO_NATIVE_ADAPTER");
  state->device = state->render_interface->createDevice(preferred_adapter ? preferred_adapter : "");
  if (!state->device) {
    REXLOG_ERROR("Native GPU: failed to create {} device", NameOf(backend));
    return false;
  }
  const auto support = QueryD3D12Support(static_cast<plume::D3D12Device*>(state->device.get())->d3d);
  REXLOG_INFO("Native GPU capabilities: shader_model=0x{:X} binding_tier={} wave_ops={} lanes={}-{} native_fp16={} uma={} fsr3_capable={}",
      support.shader_model, support.binding_tier, support.wave_ops, support.wave_min,
      support.wave_max, support.native_fp16, support.uma, support.Fsr3Ready());
  if (!support.BaseReady()) {
    REXLOG_ERROR("Native GPU requires D3D12 shader model 6.0 and resource binding tier 2; this adapter does not report both");
    return false;
  }
  REXLOG_INFO("Native GPU init: device ready");
  if (backend == Backend::kD3D12) PrewarmDxilArchives();
  if (!CreatePipelineLayout(*state)) {
    REXLOG_ERROR("Native GPU: failed to create bindless pipeline layout");
    return false;
  }
  REXLOG_INFO("Native GPU init: pipeline layout ready");
  state->queue = state->device->createCommandQueue(plume::RenderCommandListType::DIRECT);
  state->idle_fence = state->device->createCommandFence();
  if (!state->queue || !state->idle_fence) {
    REXLOG_ERROR("Native GPU: failed to create command queue or fence");
    return false;
  }

  auto native_window = static_cast<HWND>(window->GetNativeWindowHandle());
  state->swap_chain = state->queue->createSwapChain(plume::RenderSwapChainDesc(
      native_window, plume::RenderFormat::B8G8R8A8_UNORM, 3));
  if (!state->swap_chain || !state->swap_chain->resize() || state->swap_chain->isEmpty()) {
    REXLOG_ERROR("Native GPU: failed to create or size the swap chain");
    return false;
  }
  // Vsync off; framerate_limit paces presents. (The nvwgf2umx.dll fast-fail
  // seen with vsync off came from the FPS overlay's multi-rect clears, see
  // PresentTexture.) LEGO_NATIVE_VSYNC=1 turns vsync on.
  const bool vsync_on = NativeVsyncRequested();
  state->swap_chain->setVsyncEnabled(vsync_on);
  REXLOG_INFO("Native GPU init: swap chain ready (vsync {})", vsync_on ? "on" : "off");
  if (backend == Backend::kD3D12) {
    StartPipelineLibrary(state->device.get());
    PreparePipelineLibrary(state->pipeline_layout.get());
  }
  if (!CreatePresentPipeline(*state) || !CreateNullVertexBuffer(*state) ||
      !CreateNullTextures(*state) || !CreateFrameRing(*state)) {
    REXLOG_ERROR("Native GPU: failed to create present pipeline or frame ring");
    StopPipelineLibrary();
    return false;
  }
  if (backend == Backend::kD3D12 && StartupPipelinePreparationEnabled()) {
    auto* device = static_cast<plume::D3D12Device*>(state->device.get())->d3d;
    state->depth_resampler = DepthResampler::Create(device,
        g_copy_vs_dxil, g_depth_resample_ps_dxil, g_depth_resample_depth_ps_dxil,
        g_depth_resample_stencil_ps_dxil);
    const auto overlay_count = PrepareNativeOverlayPipelines(device, DXGI_FORMAT_B8G8R8A8_UNORM);
    REXLOG_INFO("Native GPU: startup depth resampler ready={}, overlay pipelines={}/2",
                bool(state->depth_resampler), overlay_count);
  }

  const auto& description = state->device->getDescription();
  if (backend == Backend::kD3D12) {
    const char* path = std::getenv("LEGO_NATIVE_GPU_TIMESTAMPS");
    if (path && *path) {
      state->gpu_timestamps = GpuSubmissionTimestamps::Create(
          state->device.get(), state->queue.get(), state->command_slot_count,
          path, kNativeGpuBuildFingerprint);
      if (!state->gpu_timestamps)
        REXLOG_WARN("Native GPU: timestamp diagnostic unavailable (allocation, frequency or output failure)");
      else
        REXLOG_INFO("Native GPU: per-submission GPU timestamps enabled, frequency={} Hz (queue elapsed, not GPU utilization or whole-frame time)",
                    state->gpu_timestamps->Frequency());
    }
  }
  REXLOG_INFO("Native GPU [{}]: {} ready on '{}' ({}x{}, {} images)",
              kNativeGpuBuildFingerprint, NameOf(backend), description.name,
              state->swap_chain->getWidth(), state->swap_chain->getHeight(),
              state->swap_chain->getTextureCount());
  {
    std::lock_guard slots(g_texture_slots_mutex);
    g_state = std::move(state);
    g_texture_slot_cursor = 0;
  }
  g_anisotropic_override.store(NativeAnisotropicRequested(), std::memory_order_release);
  g_state_generation.fetch_add(1, std::memory_order_release);
  g_ready.store(g_state->device && g_state->queue && g_state->swap_chain, std::memory_order_release);
  StartNativeWorker();
  SetNativeOverlayDeviceReady(true);
  StartTextureResources();
  REXLOG_INFO("Native write tracking: pixel textures={} buffers={} (vertex textures remain content-checked)",
      CpuMemoryWatchEnabled(), CpuBufferMemoryWatchEnabled());
  return true;
}

void HostDevice::Shutdown() {
  // Drain and join before taking the lock the worker needs for its tasks.
  StopNativeWorker();
  auto recording = LockRecording();
  std::lock_guard lock(g_mutex);
  if (!g_state) {
    return;
  }
  for (const auto& commands : g_state->command_lists)
    if (commands) NativeImmediateCommands(commands.get());
  WaitForSubmittedFramesLocked(*g_state);
  if (g_state->gpu_timestamps)
    g_state->gpu_timestamps->Shutdown(g_state->present_number);
  if (g_state->completion_callbacks.size())
    REXLOG_INFO("Native GPU: cancelling {} completion callbacks at title shutdown",
                g_state->completion_callbacks.size());
  ResetNativeOverlayGpu();
  // All GPU users must be idle before Plume resources are released.
  ResetDrawResources();
  ResetQueryResources();
  ResetShaderResources();
  ResetBufferResources();
  ResetTextureResources();
  ResetDrawBindings();
  ResetVertexDeclarations();
  ShutdownCpuMemoryWatch();
  StopPipelineLibrary();
  g_ready.store(false, std::memory_order_release);
  {
    std::lock_guard slots(g_texture_slots_mutex);
    g_state.reset();
  }
  g_state_generation.fetch_add(1, std::memory_order_release);
}

bool HostDevice::IsReady() {
  return g_ready.load(std::memory_order_acquire);
}

DrawDeviceView HostDevice::CurrentDrawDeviceView() {
  if (t_draw_preparation) {
    auto view = t_draw_preparation->view;
    view.commands = t_draw_preparation->uploads;
    return view;
  }
  std::lock_guard lock(g_mutex);
  if (!g_state) return {};
  const auto& state = *g_state;
  return {state.device.get(), state.pipeline_layout.get(),
          state.texture_descriptors.get(), state.sampler_descriptors.get(),
          state.null_vertex_buffer.get(),
          state.command_list_open ? state.command_lists[state.frame_slot].get() : nullptr,
          state.command_list_open ? g_recording_serial : 0};
}

DrawDeviceView HostDevice::ImmutableDrawDeviceView() {
  // The view only changes with the device state; the generation proves the cached one.
  static thread_local DrawDeviceView cached;
  static thread_local u64 cached_generation = 0;
  const auto generation = g_state_generation.load(std::memory_order_acquire);
  if (cached_generation == generation) return cached;
  std::lock_guard lock(g_mutex);
  cached_generation = g_state_generation.load(std::memory_order_relaxed);
  if (!g_state) { cached = {}; return cached; }
  const auto& state = *g_state;
  cached = {state.device.get(), state.pipeline_layout.get(),
            state.texture_descriptors.get(), state.sampler_descriptors.get(),
            state.null_vertex_buffer.get(), nullptr, 0};
  return cached;
}

plume::RenderDevice* HostDevice::Device() {
  // The pointer only changes with the device state; the generation-checked view avoids g_mutex,
  // which the recorder holds across submissions.
  return ImmutableDrawDeviceView().device;
}

plume::RenderCommandQueue* HostDevice::Queue() {
  std::lock_guard lock(g_mutex);
  return g_state ? g_state->queue.get() : nullptr;
}

plume::RenderSwapChain* HostDevice::SwapChain() {
  std::lock_guard lock(g_mutex);
  return g_state ? g_state->swap_chain.get() : nullptr;
}

plume::RenderPipelineLayout* HostDevice::PipelineLayout() {
  std::lock_guard lock(g_mutex);
  return g_state ? g_state->pipeline_layout.get() : nullptr;
}

plume::RenderDescriptorSet* HostDevice::TextureDescriptorSet() {
  std::lock_guard lock(g_mutex);
  return g_state ? g_state->texture_descriptors.get() : nullptr;
}

plume::RenderDescriptorSet* HostDevice::SamplerDescriptorSet() {
  std::lock_guard lock(g_mutex);
  return g_state ? g_state->sampler_descriptors.get() : nullptr;
}

namespace {
u32 RegisterSamplerLocked(State& state, const TextureFetchWords& fetch, int32_t anisotropic_override) {
  auto desc = DecodeTextureSampler(fetch);
  ApplyAnisotropicOverride(desc, fetch, anisotropic_override);
  const auto key = TextureSamplerKey(desc);
  if (const auto it = state.samplers.find(key); it != state.samplers.end()) return it->second.descriptor;
  // Slot 0 remains the copy/resolve fallback. Never overwrite a descriptor
  // referenced by an open or in-flight command list, and never evict samplers.
  const u32 slot = u32(state.samplers.size()) + 1;
  if (slot >= kBindlessSamplerCount) {
    if (state.sampler_error_logs++ < 4) REXLOG_ERROR("Native GPU: sampler heap exhausted");
    return 0;
  }
  auto sampler = state.device->createSampler(desc);
  if (!sampler) {
    if (state.sampler_error_logs++ < 4) REXLOG_ERROR("Native GPU: sampler creation failed");
    return 0;
  }
  state.sampler_descriptors->setSampler(slot, sampler.get());
  state.samplers.emplace(key, CachedNativeSampler{slot, std::move(sampler)});
  if (slot <= 48) REXLOG_INFO(
      "Native sampler {} uvw={},{},{} filter={},{},{} aniso={} bias={} lod={}..{} border={}",
      slot, u32(desc.addressU), u32(desc.addressV), u32(desc.addressW), u32(desc.minFilter),
      u32(desc.magFilter), u32(desc.mipmapMode), desc.anisotropyEnabled ? desc.maxAnisotropy : 0,
      desc.mipLODBias, desc.minLOD, desc.maxLOD, u32(desc.borderColor));
  return slot;
}

} // namespace

u32 HostDevice::RegisterSampler(const TextureFetchWords& fetch) {
  std::lock_guard lock(g_mutex);
  if (!g_state || !g_state->sampler_descriptors) return 0;
  return RegisterSamplerLocked(*g_state, fetch, g_anisotropic_override.load(std::memory_order_acquire));
}

void HostDevice::RegisterSamplers(
    const std::array<TextureFetchWords, 32>& fetches, u32 mask,
    std::array<u32, 32>& descriptors) {
  if (!mask) return;
  // Both fetch words and the frame's filtering setting select the immutable
  // descriptor. A cache hit must match both, including after Apply/Revert.
  const int32_t anisotropic_override = g_anisotropic_override.load(std::memory_order_acquire);
  struct CachedSampler { u64 generation = 0; TextureFetchWords fetch{}; u32 descriptor = 0;
                         int32_t anisotropic_override = -1; };
  static thread_local std::array<CachedSampler, 256> cached;
  static const bool cache_enabled = [] {
    const char* value = std::getenv("LEGO_NATIVE_SAMPLER_THREAD_CACHE");
    return !value || value[0] != '0';
  }();
  const u64 generation = g_state_generation.load(std::memory_order_acquire);
  const auto entry_for = [&](const TextureFetchWords& fetch) -> CachedSampler& {
    u64 hash = 0xCBF29CE484222325ull;
    for (const u32 word : fetch) hash = (hash ^ word) * 0x100000001B3ull;
    return cached[(hash ^ (hash >> 29)) & 255];
  };
  if (cache_enabled) {
    u32 missing = 0;
    for (u32 i = 0; i < fetches.size(); ++i) {
      if (!(mask & (1u << i))) continue;
      const auto& entry = entry_for(fetches[i]);
      if (entry.descriptor && entry.generation == generation && entry.fetch == fetches[i] &&
          entry.anisotropic_override == anisotropic_override) descriptors[i] = entry.descriptor;
      else missing |= 1u << i;
    }
    if (!missing) return;
    mask = missing;
  }
  static const bool no_batch = std::getenv("LEGO_NATIVE_NO_SAMPLER_BATCH") != nullptr;
  if (no_batch) {
    for (u32 i = 0; i < fetches.size(); ++i)
      if (mask & (1u << i)) descriptors[i] = RegisterSampler(fetches[i]);
    return;
  }
  std::lock_guard lock(g_mutex);
  // CPU-only replay shows a benefit, but actual gameplay hit rate/FPS has not
  // been measured. Keep this experiment opt-in for a same-build comparison.
  static const bool fetch_cache = [] {
    const char* value=std::getenv("LEGO_NATIVE_SAMPLER_FETCH_CACHE");
    return value && *value && std::strcmp(value,"0")!=0;
  }();
  if (fetch_cache && g_state && g_state->sampler_descriptors) {
    auto& state=*g_state;
    for (u32 i=0;i<fetches.size();++i) {
      if (!(mask & (1u<<i))) continue;
      if (state.sampler_last_descriptors[i] && state.sampler_last_fetches[i]==fetches[i] &&
          state.sampler_last_overrides[i]==anisotropic_override) {
        descriptors[i]=state.sampler_last_descriptors[i];continue;
      }
      descriptors[i]=RegisterSamplerLocked(state,fetches[i],anisotropic_override);
      state.sampler_last_descriptors[i]=descriptors[i];
      state.sampler_last_overrides[i]=anisotropic_override;
      if (descriptors[i]) state.sampler_last_fetches[i]=fetches[i];
    }
    return;
  }
  for (u32 i = 0; i < fetches.size(); ++i) {
    if (!(mask & (1u << i))) continue;
    descriptors[i] = g_state && g_state->sampler_descriptors
        ? RegisterSamplerLocked(*g_state, fetches[i],anisotropic_override) : 0;
    // Stored under the generation read before the lock: a device swapped meanwhile never matches it.
    if (cache_enabled && descriptors[i]) entry_for(fetches[i]) = {generation, fetches[i], descriptors[i], anisotropic_override};
  }
}

plume::RenderBuffer* HostDevice::NullVertexBuffer() {
  std::lock_guard lock(g_mutex);
  return g_state ? g_state->null_vertex_buffer.get() : nullptr;
}

// Callers hold g_mutex; the bit read takes the slot lock.
static bool TextureSlotLive(const State& state, u32 index) {
  std::lock_guard slots(g_texture_slots_mutex);
  return index < state.texture_slots.size() && state.texture_slots[index];
}

u32 HostDevice::RegisterTexture(plume::RenderTexture* texture,
                                plume::RenderTextureView* view) {
  if (!texture || !view) {
    return ~u32{0};
  }
  // The recorder holds g_mutex across submission and present; the slot table has its own lock.
  std::lock_guard slots(g_texture_slots_mutex);
  if (!g_state || !g_state->texture_descriptors) {
    return ~u32{0};
  }
  const u32 count = u32(g_state->texture_slots.size());
  const u32 span = count > kFirstTextureSlot ? count - kFirstTextureSlot : 0;
  const u32 cursor = g_texture_slot_cursor >= kFirstTextureSlot && g_texture_slot_cursor < count
      ? g_texture_slot_cursor - kFirstTextureSlot : 0;
  for (u32 step = 0; step < span; ++step) {
    const u32 slot = kFirstTextureSlot + (cursor + step) % span;
    if (g_state->texture_slots[slot]) {
      continue;
    }
    g_state->texture_slots[slot] = true;
    g_texture_slot_cursor = slot + 1;
    g_state->texture_descriptors->setTexture(
        slot, texture, plume::RenderTextureLayout::SHADER_READ, view);
    return slot;
  }
  REXLOG_ERROR("Native GPU: bindless texture heap exhausted");
  return ~u32{0};
}

void HostDevice::UnregisterTexture(u32 descriptor_index) {
  if (t_draw_preparation) {
    t_draw_preparation->unregister.push_back(descriptor_index);
    return;
  }
  // Queued tapes may still name the slot: free it behind them.
  if (descriptor_index != ~u32{0} && NativeFullDrawPreparationEnabled() && NativeWorkerStarted() &&
      NativeWorkerShouldQueue()) {
    NativeWorkerSubmit([descriptor_index] { UnregisterTexture(descriptor_index); }, false, false, false,
                       NativeTaskStage::kRecorder);
    return;
  }
  std::lock_guard lock(g_mutex);
  std::lock_guard slots(g_texture_slots_mutex);
  if (!g_state || descriptor_index < kFirstTextureSlot ||
      descriptor_index >= g_state->texture_slots.size()) {
    return;
  }
  if (!g_state->texture_slots[descriptor_index]) return;
  u32 live_frames = 0;
  for (u32 slot = 0; slot < g_state->command_slot_count; ++slot) {
    if (g_state->frame_submitted[slot] ||
        (g_state->command_list_open && slot == g_state->frame_slot))
      live_frames |= 1u << slot;
  }
  if (g_state->retired_descriptors.Retire(descriptor_index, live_frames))
    g_state->texture_slots[descriptor_index] = false;
}

plume::RenderCommandList* HostDevice::BeginFrameCommands() {
  if (t_draw_preparation) return t_draw_preparation->uploads;
  auto recording = LockRecording();
  std::lock_guard lock(g_mutex);
  if (!g_state) {
    return nullptr;
  }
  return BeginFrameCommandsLocked(*g_state);
}

DrawUploadSlice HostDevice::AllocateDrawUpload(u32 size) {
  if (t_draw_preparation) {
    auto& context = *t_draw_preparation;
    const u64 reserved = (u64(size) + 255) & ~u64{255};
    if (reserved && reserved <= context.capacity - context.used) {
      auto result = context.allocation;
      result.offset += context.used;
      result.mapped = static_cast<u8*>(result.mapped) + context.used;
      context.used += u32(reserved);
      return result;
    }
    auto buffer = std::shared_ptr<plume::RenderBuffer>(context.view.device->createBuffer(
        plume::RenderBufferDesc::UploadBuffer(reserved)).release());
    if (!buffer) return {};
    auto* mapped = buffer->map();
    if (!mapped) return {};
    const DrawUploadSlice result{buffer.get(), mapped, 0};
    context.retired.push_back(std::move(buffer));
    return result;
  }
  auto recording = LockRecording();
  std::lock_guard lock(g_mutex);
  if (!g_state || !size || !BeginFrameCommandsLocked(*g_state)) return {};
  auto& state = *g_state;
  auto& pages = state.draw_uploads[state.frame_slot];
  const u64 reserved = (u64(size) + 255) & ~u64{255};
  while (true) {
    if (state.draw_upload_page >= pages.size()) {
      auto page = std::make_unique<DrawUploadPage>();
      page->size = std::max(u64{4 * 1024 * 1024}, reserved);
      page->buffer = state.device->createBuffer(plume::RenderBufferDesc::UploadBuffer(page->size));
      if (!page->buffer) return {};
      page->mapped = static_cast<u8*>(page->buffer->map());
      if (!page->mapped) return {};
      pages.push_back(std::move(page));
    }
    auto& page = *pages[state.draw_upload_page];
    if (state.draw_upload_offset + reserved <= page.size) {
      const u64 offset = state.draw_upload_offset;
      state.draw_upload_offset += reserved;
      return {page.buffer.get(), page.mapped + offset, offset};
    }
    ++state.draw_upload_page;
    state.draw_upload_offset = 0;
  }
}

bool HostDevice::Synchronize(SyncReason reason) {
  // Preparation records into a tape; the open list belongs to the recorder.
  if (t_draw_preparation) {
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true)) REXLOG_WARN("Native GPU: synchronize refused during draw preparation");
    return false;
  }
  const auto sync_start = std::chrono::steady_clock::now();
  auto recording = LockRecording();
  std::lock_guard lock(g_mutex);
  if (!g_state) {
    return false;
  }

  State& state = *g_state;
  if (state.command_list_open) {
    const u32 slot = state.frame_slot;
    auto* commands = state.command_lists[slot].get();
    if (state.gpu_timestamps)
      state.gpu_timestamps->End(commands, slot, "synchronize", static_cast<u32>(reason));
    commands->end();
    state.command_list_open = false;
    state.queue->executeCommandLists(NativeImmediateCommands(commands), state.frame_fences[slot].get());
    MarkSubmissionLocked(state, slot);
  }

  const auto wait_start = std::chrono::steady_clock::now();
  bool waited = false;
  for (u32 slot = 0; slot < state.command_slot_count; ++slot) {
    if (!state.frame_submitted[slot]) {
      continue;
    }
    if (!WaitForSubmissionLocked(state, slot)) return false;
    RefreshCompletedSubmissionsLocked(state);
    state.retired_resources[slot].clear();
    {
      std::lock_guard slots(g_texture_slots_mutex);
      state.retired_descriptors.CompleteFrame(slot,
          [&](u32 index) { state.texture_slots[index] = false; });
    }
    SaveSnapshots(state.snapshots[slot]);
    waited = true;
  }
  if (waited) {
    const auto wait_ms = std::chrono::duration<double, std::milli>(
                             std::chrono::steady_clock::now() - wait_start)
                             .count();
    if (wait_ms > 20.0 && state.long_wait_log_count++ < 20) {
      REXLOG_WARN("Native GPU: completion callback GPU wait took {:.2f} ms",
                  wait_ms);
    }
  }
  if (NativeTextureTimingEnabled()) {
    ++state.sync_calls;
    const double elapsed = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - sync_start).count();
    state.sync_ms += elapsed;
    const auto index = static_cast<u32>(reason);
    if (index < state.sync_reason_calls.size()) {
      ++state.sync_reason_calls[index];
      state.sync_reason_ms[index] += elapsed;
    }
  }
  return true;
}

void HostDevice::RecordCpuResourceWaitSkip() {
  std::lock_guard lock(g_mutex);
  if (g_state) ++g_state->cpu_resource_wait_skips;
}

bool HostDevice::EnqueueCompletionCallback(std::function<void()> callback) {
  auto recording = LockRecording();
  std::lock_guard lock(g_mutex);
  if (!g_state || !callback) return false;
  State& state = *g_state;
  // An open list's fence will be assigned on submission. Otherwise all work
  // preceding this insertion is in the latest already submitted list.
  state.completion_callbacks.Push(state.last_submission + u64(state.command_list_open),
                                  std::move(callback));
  ++state.callbacks_enqueued;
  return true;
}

void HostDevice::PollCompletionCallbacks(bool wait_for_recording) {
  // Callbacks are guest code: never run them on the native recording worker.
  if (OnNativeWorkerThread()) return;
  // A callback may enter another hook that polls. Keep the original FIFO
  // order without holding a renderer mutex while executing guest code.
  static thread_local bool executing = false;
  if (executing) return;
  static std::mutex execution_mutex;
  std::unique_lock execution_lock(execution_mutex, std::try_to_lock);
  if (!execution_lock.owns_lock()) return;
  struct Guard { bool& flag; Guard(bool& f) : flag(f) { flag = true; }
                 ~Guard() { flag = false; } } guard(executing);
  static const bool batched = [] {
    const char* value = std::getenv("LEGO_NATIVE_BATCH_COMPLETION_CALLBACKS");
    return value && value[0] == '1';
  }();
  std::array<std::function<void()>, 128> callbacks;
  for (;;) {
    size_t count = 0;
    {
      // Completion state only: no need to drain queued recording first.
      std::unique_lock recording(NativeRecordingMutex(), std::defer_lock);
      if (wait_for_recording) recording.lock();
      else if (!recording.try_lock()) return;
      std::lock_guard lock(g_mutex);
      if (!g_state) return;
      RefreshCompletedSubmissionsLocked(*g_state);
      const size_t limit = batched ? callbacks.size() : 1;
      while (count < limit) {
        auto callback = g_state->completion_callbacks.PopReady(g_state->completed_submission);
        if (!callback) break;
        callbacks[count++] = std::move(callback);
      }
      if (!count) return;
      g_state->callbacks_executed += count;
    }
    for (size_t i = 0; i < count; ++i) {
      callbacks[i]();
      callbacks[i] = {};
    }
  }
}

bool HostDevice::SubmitRecordedWork() {
  if (t_draw_preparation) return false;
  auto recording = LockRecording();
  std::lock_guard lock(g_mutex);
  if (!g_state) {
    return false;
  }
  State& state = *g_state;
  if (!state.command_list_open) {
    return true;
  }
  const u32 slot = state.frame_slot;
  auto* commands = state.command_lists[slot].get();
  if (state.gpu_timestamps)
    state.gpu_timestamps->End(commands, slot, "kickoff");
  commands->end();
  state.command_list_open = false;
  state.queue->executeCommandLists(NativeImmediateCommands(commands), state.frame_fences[slot].get());
  MarkSubmissionLocked(state, slot);
  state.frame_slot = (slot + 1) % state.command_slot_count;
  return true;
}

void HostDevice::RetireResource(std::shared_ptr<void> resource) {
  if (t_draw_preparation) {
    if (resource) t_draw_preparation->retired.push_back(std::move(resource));
    return;
  }
  // Queued tapes may still reference it: retire behind them.
  if (resource && NativeFullDrawPreparationEnabled() && NativeWorkerStarted() && NativeWorkerShouldQueue()) {
    NativeWorkerSubmit([resource] { RetireResource(resource); }, false, false, false, NativeTaskStage::kRecorder);
    return;
  }
  auto recording = LockRecording();
  if (!resource) {
    return;
  }
  std::lock_guard lock(g_mutex);
  if (!g_state) {
    return;
  }
  // The current slot may be unused after SubmitRecordedWork advanced it.
  // Retiring only there lets BeginFrameCommands destroy resources before an
  // older submitted list has finished. Keep a reference at EVERY live fence.
  for (u32 slot = 0; slot < g_state->command_slot_count; ++slot) {
    if (g_state->frame_submitted[slot] ||
        (g_state->command_list_open && slot == g_state->frame_slot))
      g_state->retired_resources[slot].push_back(resource);
  }
}

bool HostDevice::ResolveHdrColor(plume::RenderTexture* source, u32 descriptor_index,
                                 plume::RenderTexture* destination,
                                 u32 width, u32 height, float scale,
                                 ColorResolveDestination destination_format, bool source_is_target,
                                 const ResolveRegion* region, const ResolveRegion* source_area) {
  auto recording = LockRecording();
  // Acquire the list before g_mutex: BeginFrameCommands owns that mutex too.
  auto* commands = BeginFrameCommands();
  if (!commands || !source || !destination || source == destination) return false;
  const ResolveRegion area = region ? *region : ResolveRegion{0, 0, width, height, 0, 0};
  const bool resample=destination_format==ColorResolveDestination::kScaledRGBA8;
  const ResolveRegion input=source_area ? *source_area : area;
  if (!area.width || !area.height || !input.width || !input.height) return false;
  DrawUploadSlice scaled_constants;
  if (resample) {
    const auto mapping=ScaledResolveMapping(input,area);
    if (!mapping) return false;
    scaled_constants=AllocateDrawUpload(sizeof(*mapping));
    if (!scaled_constants) return false;
    std::memcpy(scaled_constants.mapped,&*mapping,sizeof(*mapping));
  }
  const i32 dx = i32(area.left) - i32(area.x), dy = i32(area.top) - i32(area.y);
  if (dx < -32768 || dx > 32767 || dy < -32768 || dy > 32767) return false;
  std::shared_ptr<plume::RenderFramebuffer> framebuffer;
  {
    std::lock_guard lock(g_mutex);
    if (!g_state || !TextureSlotLive(*g_state, descriptor_index)) return false;
    auto& state = *g_state;
    const size_t format_index = static_cast<size_t>(destination_format);
    if (format_index >= state.resolve_pipelines.size()) return false;
    auto& pipeline = state.resolve_pipelines[format_index];
    auto& shader=resample ? state.scaled_resolve_pixel_shader : state.resolve_pixel_shader;
    if (!shader) {
      shader = state.device->createShader(
          resample ? g_resolve_scaled_ps_dxil : g_resolve_color_ps_dxil,
          resample ? sizeof(g_resolve_scaled_ps_dxil) : sizeof(g_resolve_color_ps_dxil), "main",
          plume::RenderShaderFormat::DXIL);
      if (!shader) return false;
    }
    if (!pipeline) {
      plume::RenderGraphicsPipelineDesc desc;
      desc.pipelineLayout = state.pipeline_layout.get();
      desc.vertexShader = state.copy_vertex_shader.get();
      desc.pixelShader = shader.get();
      desc.depthFunction = plume::RenderComparisonFunction::ALWAYS;
      desc.depthEnabled = false;
      desc.depthWriteEnabled = false;
      desc.primitiveTopology = plume::RenderPrimitiveTopology::TRIANGLE_LIST;
      desc.cullMode = plume::RenderCullMode::NONE;
      desc.fillMode = plume::RenderFillMode::SOLID;
      desc.renderTargetCount = 1;
      constexpr plume::RenderFormat formats[] = {
          plume::RenderFormat::R16G16B16A16_UNORM,
          plume::RenderFormat::R32G32B32A32_FLOAT,
          plume::RenderFormat::R16G16_UNORM,
          plume::RenderFormat::R32_FLOAT,
          plume::RenderFormat::R8G8B8A8_UNORM};
      desc.renderTargetFormat[0] = formats[format_index];
      desc.renderTargetBlend[0] = plume::RenderBlendDesc::Copy();
      pipeline = state.device->createGraphicsPipeline(desc);
      if (!pipeline || !static_cast<plume::D3D12GraphicsPipeline*>(pipeline.get())->d3d) {
        pipeline.reset();
        REXLOG_ERROR("Native GPU: D3D12 rejected the color resolve pipeline (format={})",
                     format_index);
        return false;
      }
    }
    plume::RenderFramebufferDesc desc;
    const plume::RenderTexture* attachments[] = {destination};
    desc.colorAttachments = attachments;
    desc.colorAttachmentsCount = 1;
    framebuffer = state.device->createFramebuffer(desc);
    if (!framebuffer) return false;
    const plume::RenderTextureBarrier barriers[] = {
      {source, plume::RenderTextureLayout::SHADER_READ},
      {destination, plume::RenderTextureLayout::COLOR_WRITE}};
    commands->barriers(plume::RenderBarrierStage::GRAPHICS, barriers, 2);
    commands->setFramebuffer(framebuffer.get());
    commands->setViewports(plume::RenderViewport(float(area.x), float(area.y),
        float(area.width), float(area.height)));
    commands->setScissors(plume::RenderRect(area.x, area.y,
        area.x + area.width, area.y + area.height));
    commands->setGraphicsPipelineLayout(state.pipeline_layout.get());
    for (u32 set = 0; set < 3; ++set)
      commands->setGraphicsDescriptorSet(state.texture_descriptors.get(), set);
    commands->setGraphicsDescriptorSet(state.sampler_descriptors.get(), 3);
    commands->setPipeline(pipeline.get());
    struct { u32 index, index2; float scale, unused; } constants{
        descriptor_index, (u32(dx) & 0xFFFFu) | ((u32(dy) & 0xFFFFu) << 16), scale, 0};
    if (resample) {
      commands->setGraphicsRootDescriptor(
          scaled_constants.buffer->at(scaled_constants.offset),3);
    }
    commands->setGraphicsPushConstants(0, &constants, 0, sizeof(constants));
    commands->drawInstanced(3, 1, 0, 0);
    commands->setFramebuffer(nullptr);
    const plume::RenderTextureBarrier after[] = {
      {source, source_is_target ? (destination_format == ColorResolveDestination::kDepthFloat32
                                     ? plume::RenderTextureLayout::DEPTH_WRITE
                                     : plume::RenderTextureLayout::COLOR_WRITE)
                               : plume::RenderTextureLayout::SHADER_READ},
      {destination, plume::RenderTextureLayout::SHADER_READ}};
    commands->barriers(plume::RenderBarrierStage::GRAPHICS, after, 2);
  }
  RetireResource(std::move(framebuffer));
  return true;
}

bool HostDevice::ResampleDepthSurface(plume::RenderTexture* source,
                                     plume::RenderTexture* destination) {
  auto recording=LockRecording();
  if(!source || !destination || source==destination) return false;
  auto* raw_source=static_cast<plume::D3D12Texture*>(source)->d3d;
  auto* raw_destination=static_cast<plume::D3D12Texture*>(destination)->d3d;
  if(!DepthResampler::Supports(raw_source,raw_destination)) return false;
  auto* commands=BeginFrameCommands();
  if(!commands) return false;
  DepthResampler* resampler=nullptr;
  {
    std::lock_guard lock(g_mutex);
    if(!g_state) return false;
    if(!g_state->depth_resampler) g_state->depth_resampler=DepthResampler::Create(
        static_cast<plume::D3D12Device*>(g_state->device.get())->d3d,
        g_copy_vs_dxil,g_depth_resample_ps_dxil,g_depth_resample_depth_ps_dxil,
        g_depth_resample_stencil_ps_dxil);
    resampler=g_state->depth_resampler.get();
  }
  if(!resampler) return false;
  commands->setFramebuffer(nullptr);
  const plume::RenderTextureBarrier barriers[]{
      {source,plume::RenderTextureLayout::SHADER_READ},
      {destination,plume::RenderTextureLayout::DEPTH_WRITE}};
  commands->barriers(plume::RenderBarrierStage::GRAPHICS,barriers,2);
  auto* native=static_cast<plume::D3D12CommandList*>(NativeImmediateCommands(commands));
  native->resetSamplePositions();
  auto lease=resampler->Record(native->d3d,raw_source,raw_destination);
  if(!lease) return false;
  // Raw recording changes IA/stencil as well as heaps, roots and PSOs. Force
  // Plume's next ordinary draw to restore those values even with caches on.
  native->activeTopology=D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
  native->activeStencilRef=~u32{0};
  native->notifyDescriptorHeapWasChangedExternally();
  native->notifyCommandStateWasChangedExternally();
  RetireResource(std::move(lease));
  return true;
}

bool HostDevice::TransferDepthAlias(plume::RenderTexture* source, u32 descriptor_index,
                                   plume::RenderTexture* destination,
                                   u32 width, u32 height, bool restore,
                                   float depth_half_tile) {
  auto recording = LockRecording();
  auto* commands = BeginFrameCommands();
  if (!commands || !source || !destination || source == destination || !width || !height ||
      (restore && (!(depth_half_tile > 0) || !std::isfinite(depth_half_tile) ||
                   std::fmod(float(width), 2 * depth_half_tile) != 0)))
    return false;
  std::shared_ptr<plume::RenderFramebuffer> framebuffer;
  {
    std::lock_guard lock(g_mutex);
    if (!g_state || !TextureSlotLive(*g_state, descriptor_index)) return false;
    auto& state = *g_state;
    auto& shader = state.depth_alias_shaders[restore];
    auto& pipeline = state.depth_alias_pipelines[restore];
    if (!shader) shader = state.device->createShader(
        restore ? g_depth_restore_ps_dxil : g_depth_pack_ps_dxil,
        restore ? sizeof(g_depth_restore_ps_dxil) : sizeof(g_depth_pack_ps_dxil),
        "main", plume::RenderShaderFormat::DXIL);
    if (!shader) return false;
    if (!pipeline) {
      plume::RenderGraphicsPipelineDesc desc;
      desc.pipelineLayout = state.pipeline_layout.get();
      desc.vertexShader = state.copy_vertex_shader.get();
      desc.pixelShader = shader.get();
      desc.depthFunction = plume::RenderComparisonFunction::ALWAYS;
      desc.depthEnabled = restore;
      desc.depthWriteEnabled = restore;
      desc.depthTargetFormat = restore ? plume::RenderFormat::D32_FLOAT_S8_UINT
                                      : plume::RenderFormat::UNKNOWN;
      desc.primitiveTopology = plume::RenderPrimitiveTopology::TRIANGLE_LIST;
      desc.cullMode = plume::RenderCullMode::NONE;
      desc.fillMode = plume::RenderFillMode::SOLID;
      desc.renderTargetCount = restore ? 0 : 1;
      desc.renderTargetFormat[0] = plume::RenderFormat::R8G8B8A8_UNORM;
      desc.renderTargetBlend[0] = plume::RenderBlendDesc::Copy();
      pipeline = state.device->createGraphicsPipeline(desc);
      if (!pipeline || !static_cast<plume::D3D12GraphicsPipeline*>(pipeline.get())->d3d) {
        pipeline.reset();
        REXLOG_ERROR("Native GPU: D3D12 rejected the depth alias pipeline (restore={})",
                     restore);
        return false;
      }
    }
    plume::RenderFramebufferDesc desc;
    const plume::RenderTexture* attachments[] = {destination};
    if (restore) desc.depthAttachment = destination;
    else { desc.colorAttachments = attachments; desc.colorAttachmentsCount = 1; }
    framebuffer = state.device->createFramebuffer(desc);
    if (!framebuffer) return false;
    const plume::RenderTextureBarrier barriers[] = {
        {source, plume::RenderTextureLayout::SHADER_READ},
        {destination, restore ? plume::RenderTextureLayout::DEPTH_WRITE
                              : plume::RenderTextureLayout::COLOR_WRITE}};
    commands->barriers(plume::RenderBarrierStage::GRAPHICS, barriers, 2);
    commands->setFramebuffer(framebuffer.get());
    commands->setViewports(plume::RenderViewport(0, 0, float(width), float(height)));
    commands->setScissors(plume::RenderRect(0, 0, width, height));
    commands->setGraphicsPipelineLayout(state.pipeline_layout.get());
    for (u32 set = 0; set < 3; ++set)
      commands->setGraphicsDescriptorSet(state.texture_descriptors.get(), set);
    commands->setGraphicsDescriptorSet(state.sampler_descriptors.get(), 3);
    commands->setPipeline(pipeline.get());
    const struct { u32 index, half_count; float half_tile, unused; } constants{
        descriptor_index, restore ? u32(std::round(float(width) / depth_half_tile)) : 0,
        depth_half_tile, 0};
    commands->setGraphicsPushConstants(0, &constants, 0, sizeof(constants));
    commands->drawInstanced(3, 1, 0, 0);
    commands->setFramebuffer(nullptr);
    if (!restore) commands->barriers(plume::RenderBarrierStage::GRAPHICS,
        plume::RenderTextureBarrier(destination, plume::RenderTextureLayout::SHADER_READ));
  }
  RetireResource(std::move(framebuffer));
  return true;
}

namespace {
// framerate_limit is a GPU-plugin cvar that native mode never registers (same
// as anisotropic_override), so read it from legodimensions.toml next to the exe.
int32_t ReadConfigInt(const char* name, int32_t fallback) {
  const std::string text = rex::cvar::GetFlagByName(name);
  if (!text.empty()) {
    try { return int32_t(std::stoi(text)); } catch (...) { return fallback; }
  }
  std::ifstream config(rex::filesystem::GetExecutableFolder() / "legodimensions.toml");
  const size_t length = std::strlen(name);
  std::string line;
  while (std::getline(config, line)) {
    const auto key = line.find_first_not_of(" 	");
    if (key == std::string::npos || line.compare(key, length, name) != 0) continue;
    const auto eq = line.find_first_not_of(" 	", key + length);
    if (eq == std::string::npos || line[eq] != '=') continue;
    const auto digits = line.find_first_of("-0123456789", eq + 1);
    if (digits == std::string::npos) return fallback;
    try { return int32_t(std::stoi(line.substr(digits))); } catch (...) { return fallback; }
  }
  return fallback;
}

// Legacy resolver is used only without the coordinated native-menu runtime.
int32_t LegacyFrameRateLimit() {
  static const int32_t override_limit = [] {
    const char* forced = std::getenv("LEGO_NATIVE_FPS_LIMIT");
    const int32_t value = forced ? std::clamp(std::atoi(forced),0,1000) : -1;
    REXLOG_INFO("Native GPU: presentation cap override={} (-1 uses live native_fps_limit)", value);
    return value;
  }();
  // The settings UI writes under the CVar registry lock. Copy once per
  // presentation, then release that lock before any pacing wait.
  return override_limit >= 0 ? override_limit : rex::cvar::Query<int32_t>("native_fps_limit");
}
void LimitFrameRate(int32_t limit) {
  using clock = std::chrono::steady_clock;
  static clock::time_point next = clock::now();
  static int32_t previous_limit = -1;
  if (previous_limit != limit) { next = clock::now(); previous_limit = limit; }
  if (limit <= 0) return;
  const auto period = std::chrono::duration_cast<clock::duration>(
      std::chrono::duration<double>(1.0 / limit));
  static HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr,
      CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
  auto now = clock::now();
  if (next > now) {
    // Sleep most of the gap on a high-resolution timer, spin the last 0.5 ms.
    const auto sleep_for = next - now - std::chrono::microseconds(500);
    if (timer && sleep_for > clock::duration::zero()) {
      LARGE_INTEGER due;
      due.QuadPart = -std::chrono::duration_cast<std::chrono::nanoseconds>(sleep_for).count() / 100;
      if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE))
        WaitForSingleObject(timer, INFINITE);
    }
    while (clock::now() < next) YieldProcessor();
    now = next;
  }
  // Do not bank time after a slow frame: never schedule in the past.
  next = (std::max)(next + period, now);
}
}  // namespace

void HostDevice::SetDisplayGamma(const DisplayGammaTable& table) {
  auto recording = LockRecording();
  std::lock_guard lock(g_mutex);
  if (!g_state || (g_state->gamma_table_valid && g_state->gamma_table == table)) return;
  g_state->gamma_table = table;
  g_state->gamma_table_valid = true;
  REXLOG_INFO("Native GPU: display gamma updated from title ramp black={:08X} mid={:08X} white={:08X}",
              table.words[0], table.words[128], table.words[255]);
}

bool HostDevice::PresentTexture(plume::RenderTexture* texture,
     u32 descriptor_index, float sample_offset_x, float sample_offset_y) {
  auto recording = LockRecording();
  const auto settings_runtime = AcquireNativeSettingsRuntime();
  const auto settings_frame = settings_runtime ? settings_runtime->Active() : nullptr;
  const auto* settings = settings_frame ? &settings_frame->values : nullptr;
  const int32_t fps_limit = settings ? settings->fps_limit : LegacyFrameRateLimit();
  LimitFrameRate(fps_limit);
  const int32_t requested_anisotropy = settings ? settings->anisotropy : NativeAnisotropicRequested();
  if (g_anisotropic_override.exchange(requested_anisotropy, std::memory_order_acq_rel) != requested_anisotropy)
    REXLOG_INFO("Native GPU: texture filtering changed to {} at presentation boundary", requested_anisotropy);
  const bool timing_enabled = NativeTextureTimingEnabled();
  static const bool metrics_enabled = [] {
    const char* path = std::getenv("LEGO_NATIVE_FRAME_METRICS");
    return path && *path;
  }();
  const auto present_start = timing_enabled ? std::chrono::steady_clock::now()
      : std::chrono::steady_clock::time_point{};
  if (!texture || descriptor_index == ~u32{0}) {
    return false;
  }
  SnapshotTexture(texture, "present", false);

  std::lock_guard lock(g_mutex);
  if (!g_state || !g_state->copy_pipeline || !TextureSlotLive(*g_state, descriptor_index)) {
    return false;
  }
  State& state = *g_state;
  if (!state.command_list_open &&
      (state.framebuffers.empty() || state.swap_chain->needsResize())) {
    if (!RebuildSwapChain(state)) {
      REXLOG_WARN("Native GPU: swap chain resize deferred");
      return false;
    }
  }

  plume::RenderCommandList* commands = BeginFrameCommandsLocked(state);
  if (!commands) {
    return false;
  }
  const u32 slot = state.frame_slot;

  u32 image_index = 0;
  const auto acquire_start = timing_enabled ? std::chrono::steady_clock::now()
      : std::chrono::steady_clock::time_point{};
  const bool acquired = state.swap_chain->acquireTexture(
      state.acquire_semaphores[slot].get(), &image_index);
  if (timing_enabled) state.acquire_cpu_ms += std::chrono::duration<double, std::milli>(
      std::chrono::steady_clock::now() - acquire_start).count();
  if (!acquired || image_index >= state.framebuffers.size()) {
    // Close and submit any recorded uploads/draws so the allocator remains in
    // a valid state while the window is minimized or the swap chain changes.
    if (state.gpu_timestamps)
      state.gpu_timestamps->End(commands, slot, "acquire_failed");
    commands->end();
    state.command_list_open = false;
    state.queue->executeCommandLists(NativeImmediateCommands(commands), state.frame_fences[slot].get());
    MarkSubmissionLocked(state, slot);
    state.frame_slot = (slot + 1) % state.command_slot_count;
    return false;
  }

  plume::RenderTexture* back = state.swap_chain->getTexture(image_index);
  const plume::RenderTextureBarrier barriers[] = {
      plume::RenderTextureBarrier(texture,
                                  plume::RenderTextureLayout::SHADER_READ),
      plume::RenderTextureBarrier(back,
                                  plume::RenderTextureLayout::COLOR_WRITE),
  };
  commands->barriers(plume::RenderBarrierStage::GRAPHICS, barriers, 2);
  commands->setFramebuffer(state.framebuffers[image_index].get());

  // The reference renderer applies the display ramp before spatial output
  // scaling. Keep FSR 1 and FSR 3's scene processing unchanged and apply the
  // original XDK-converted ramp once to the resolved input of presentation.
  if (state.gamma_table_valid) {
    const auto input = static_cast<plume::D3D12Texture*>(texture)->d3d->GetDesc();
    const bool supported = input.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
        input.DepthOrArraySize == 1 && input.MipLevels == 1 && input.SampleDesc.Count == 1 &&
        (input.Format == DXGI_FORMAT_R8G8B8A8_UNORM || input.Format == DXGI_FORMAT_B8G8R8A8_UNORM);
    if (supported && (!state.gamma_surface ||
        static_cast<plume::D3D12Texture*>(state.gamma_surface->texture.get())->desc.width != input.Width ||
        static_cast<plume::D3D12Texture*>(state.gamma_surface->texture.get())->desc.height != input.Height)) {
      auto replacement = std::make_shared<PresentGammaSurface>();
      plume::RenderTextureDesc desc;
      desc.dimension = plume::RenderTextureDimension::TEXTURE_2D;
      desc.width = u32(input.Width); desc.height = input.Height;
      desc.depth = desc.arraySize = desc.mipLevels = 1;
      desc.format = plume::RenderFormat::R8G8B8A8_UNORM;
      desc.flags = plume::RenderTextureFlag::UNORDERED_ACCESS;
      replacement->texture = state.device->createTexture(desc);
      plume::RenderTextureViewDesc view;
      view.dimension = plume::RenderTextureViewDimension::TEXTURE_2D;
      view.format = desc.format; view.mipLevels = 1;
      if (replacement->texture && static_cast<plume::D3D12Texture*>(replacement->texture.get())->d3d)
        replacement->view = replacement->texture->createTextureView(view);
      if (replacement->view) replacement->descriptor = RegisterTexture(replacement->texture.get(), replacement->view.get());
      if (replacement->descriptor != ~u32{0}) {
        // g_mutex is already held. Preserve the outgoing surface at every
        // live fence and use the same deferred descriptor retirement policy
        // as UnregisterTexture, without recursively taking g_mutex.
        if (auto old = state.gamma_surface) {
          u32 live = 0;
          for (u32 i = 0; i < state.command_slot_count; ++i) {
            if (state.frame_submitted[i] || (state.command_list_open && i == slot)) {
              live |= 1u << i;
              state.retired_resources[i].push_back(old);
            }
          }
          std::lock_guard slots(g_texture_slots_mutex);
          if (state.retired_descriptors.Retire(old->descriptor, live)) state.texture_slots[old->descriptor] = false;
        }
        state.gamma_surface = std::move(replacement);
      }
    }
    bool applied = false;
    if (supported && state.gamma_surface) {
      auto* output = state.gamma_surface->texture.get();
      const auto output_desc = static_cast<plume::D3D12Texture*>(output)->d3d->GetDesc();
      if (output_desc.Width == input.Width && output_desc.Height == input.Height) {
        if (!state.gamma_presenter) state.gamma_presenter = std::make_unique<DisplayGammaGpu>();
        commands->barriers(plume::RenderBarrierStage::COMPUTE,
            plume::RenderTextureBarrier(output, plume::RenderTextureLayout::GENERAL));
        FlushNativeCommands(commands);
        auto* raw = static_cast<plume::D3D12CommandList*>(NativeImmediateCommands(commands));
        applied = state.gamma_presenter->Apply(
            static_cast<plume::D3D12Device*>(state.device.get())->d3d, raw->d3d,
            static_cast<plume::D3D12Texture*>(texture)->d3d,
            static_cast<plume::D3D12Texture*>(output)->d3d, slot, state.gamma_table);
        raw->notifyDescriptorHeapWasChangedExternally();
        raw->notifyCommandStateWasChangedExternally();
        if (applied) {
          commands->barriers(plume::RenderBarrierStage::GRAPHICS,
              plume::RenderTextureBarrier(output, plume::RenderTextureLayout::SHADER_READ));
          texture = output; descriptor_index = state.gamma_surface->descriptor;
        }
        commands->setFramebuffer(state.framebuffers[image_index].get());
      }
    }
    static u32 gamma_failures = 0;
    if (!applied && gamma_failures++ < 8)
      REXLOG_WARN("Native GPU: display gamma presentation unavailable format={} size={}x{}", u32(input.Format), input.Width, input.Height);
  }

  const u32 width = state.swap_chain->getWidth();
  const u32 height = state.swap_chain->getHeight();
  QueueNativeOverlayFrame(width, height);
  const auto source_desc = static_cast<plume::D3D12Texture*>(texture)->d3d->GetDesc();
  const auto present_rect = FitPresentRect(u32(source_desc.Width), source_desc.Height,
                                         width, height);
  // Clear the whole acquired image, including bars left by a previous size.
  commands->clearColor(0, plume::RenderColor(0.0f, 0.0f, 0.0f, 1.0f));
  bool spatial_presented = false;
  const bool fsr1_requested = !NativeFsr3Enabled() && (settings ? settings->fsr1 : rex::cvar::GetFlagByName("present_effect") == "fsr");
  if (fsr1_requested) {
    if (!state.fsr1_presenter) state.fsr1_presenter = std::make_unique<Fsr1Presenter>();
    const double requested = settings ? settings->fsr1_sharpness_reduction : rex::cvar::Query<double>("present_fsr_sharpness_reduction");
    const float sharpness = std::isfinite(requested) ? float(std::clamp(requested, 0.0, 2.0)) : .2f;
    const auto passes = settings ? settings->fsr1_passes : std::clamp(rex::cvar::Query<int32_t>("present_fsr_max_upsampling_passes"), 1, 4);
    FlushNativeCommands(commands);
    auto* raw = static_cast<plume::D3D12CommandList*>(NativeImmediateCommands(commands));
    spatial_presented = state.fsr1_presenter->Draw(
        static_cast<plume::D3D12Device*>(state.device.get())->d3d, raw->d3d,
        static_cast<plume::D3D12Texture*>(texture)->d3d,
        static_cast<plume::D3D12Texture*>(back)->d3d, slot, present_rect, sharpness, uint32_t(passes),
        settings ? settings->present_dither : rex::cvar::Query<bool>("present_dither"));
    static uint32_t spatial_failures = 0;
    if (!spatial_presented && spatial_failures++ < 8)
      REXLOG_WARN("Native FSR 1: presentation setup failed; using the native spatial copy");
    raw->notifyDescriptorHeapWasChangedExternally();
    raw->notifyCommandStateWasChangedExternally();
    commands->setFramebuffer(state.framebuffers[image_index].get());
  }
  if (!spatial_presented) {
    commands->setViewports(
        plume::RenderViewport(float(present_rect.x), float(present_rect.y),
                              float(present_rect.width), float(present_rect.height)));
    commands->setScissors(plume::RenderRect(0, 0, width, height));
    commands->setGraphicsPipelineLayout(state.pipeline_layout.get());
    commands->setGraphicsDescriptorSet(state.texture_descriptors.get(), 0);
    commands->setGraphicsDescriptorSet(state.texture_descriptors.get(), 1);
    commands->setGraphicsDescriptorSet(state.texture_descriptors.get(), 2);
    commands->setGraphicsDescriptorSet(state.sampler_descriptors.get(), 3);
    commands->setPipeline(state.copy_pipeline.get());
    const struct {
      u32 descriptor_index;
      float sample_offset_x;
      float multiplier;
      float sample_offset_y;
    } push_constants = {descriptor_index, sample_offset_x, 1.0f, sample_offset_y};
    commands->setGraphicsPushConstants(0, &push_constants, 0, sizeof(push_constants));
    commands->drawInstanced(3, 1, 0, 0);
  }
  // Digits only, measured from native presents rather than UI refreshes.
  // Rectangular clears draw the tiny counter without a font, shader or upload.
  {
    constexpr u16 digits[] = {0x7B6F,0x2492,0x73E7,0x73CF,0x5BC9,
                             0x79CF,0x79EF,0x7249,0x7BEF,0x7BCF};
    static const bool no_overlay = std::getenv("LEGO_NATIVE_NO_FPS_OVERLAY") != nullptr;
    const bool show_fps = settings ? settings->show_fps : REXCVAR_GET(native_show_fps);
    if (show_fps && !no_overlay && width >= 64 && height >= 32) {
      // Capped to 4 digits: a 5+ digit reading (brief spikes with vsync off)
      // overflowed the old 60-entry arrays and handed garbage rects to
      // ClearRenderTargetView, which fast-failed nvwgf2umx.dll (2026-10-10).
      u32 value = std::min(state.fps_counter.Value(), 9999u), divisor = 1;
      while (value / divisor >= 10) divisor *= 10;
      std::array<plume::RenderRect, 4 * 15> pixels, shadows;
      u32 used = 0;
      i32 x = 8, y = i32(height) - 23;
      do {
        const u16 glyph = digits[(value / divisor) % 10];
        for (u32 row = 0; row < 5; ++row) for (u32 col = 0; col < 3; ++col) {
          if (!(glyph & (1u << (14 - row * 3 - col)))) continue;
          const i32 left = x + i32(col) * 3, top = y + i32(row) * 3;
          if (used >= pixels.size() || left + 4 > i32(width) || top + 4 > i32(height)) continue;
          pixels[used] = plume::RenderRect(left, top, left + 3, top + 3);
          shadows[used++] = plume::RenderRect(left + 1, top + 1, left + 4, top + 4);
        }
        x += 12;
        divisor /= 10;
      } while (divisor);
      static const bool batched = std::getenv("LEGO_NATIVE_FPS_OVERLAY_BATCHED") != nullptr;
      if (batched) {
        commands->clearColor(0, plume::RenderColor(0, 0, 0, 1), shadows.data(), used);
        commands->clearColor(0, plume::RenderColor(1, 1, 1, 1), pixels.data(), used);
      } else {
        for (u32 i = 0; i < used; ++i)
          commands->clearColor(0, plume::RenderColor(0, 0, 0, 1), &shadows[i], 1);
        for (u32 i = 0; i < used; ++i)
          commands->clearColor(0, plume::RenderColor(1, 1, 1, 1), &pixels[i], 1);
      }
    }
  }
  auto* overlay_commands = static_cast<plume::D3D12CommandList*>(NativeImmediateCommands(commands));
  RenderNativeOverlays(static_cast<plume::D3D12Device*>(state.device.get())->d3d,
      overlay_commands->d3d, u32(static_cast<plume::D3D12Texture*>(back)->d3d->GetDesc().Format),
      slot, g_recording_serial, width, height);
  overlay_commands->notifyDescriptorHeapWasChangedExternally();
  overlay_commands->notifyCommandStateWasChangedExternally();
  commands->setFramebuffer(nullptr);
  commands->barriers(
      plume::RenderBarrierStage::NONE,
      plume::RenderTextureBarrier(back, plume::RenderTextureLayout::PRESENT));
  if (state.gpu_timestamps)
    state.gpu_timestamps->End(commands, slot, "present", GpuSubmissionTimestamps::kNoImage, image_index);
  commands->end();
  state.command_list_open = false;

  const plume::RenderCommandList* lists[] = {NativeImmediateCommands(commands)};
  plume::RenderCommandSemaphore* waits[] = {
      state.acquire_semaphores[slot].get()};
  plume::RenderCommandSemaphore* signals[] = {
      state.render_semaphores[image_index].get()};
  const auto submit_start = timing_enabled ? std::chrono::steady_clock::now()
      : std::chrono::steady_clock::time_point{};
  state.queue->executeCommandLists(lists, 1, waits, 1, signals, 1,
                                   state.frame_fences[slot].get());
  if (timing_enabled) state.present_submit_cpu_ms += std::chrono::duration<double, std::milli>(
      std::chrono::steady_clock::now() - submit_start).count();
  MarkSubmissionLocked(state, slot);
  const auto swap_start = (timing_enabled || metrics_enabled) ? std::chrono::steady_clock::now()
      : std::chrono::steady_clock::time_point{};
  // LEGO_NATIVE_PRESENT=<sync>,<flags> overrides the Present call for the
  // vsync-off investigation (e.g. 0,0 = unthrottled without tearing,
  // 0,512 = DXGI_PRESENT_ALLOW_TEARING).
  bool presented;
  // Read the live host setting, independently of the SDK guest-vblank cvar.
  // Reapply after focus/size changes as well as settings edits; neither
  // Alt+Enter nor Alt+Tab changes the requested synchronization policy.
  const bool requested_vsync = settings ? settings->vsync : NativeVsyncRequested();
  state.swap_chain->setVsyncEnabled(requested_vsync);
  auto* presentation_chain = static_cast<plume::D3D12SwapChain*>(state.swap_chain.get());
  const auto presentation_window = presentation_chain->getWindow();
  const bool presentation_focused = GetForegroundWindow() ==
      presentation_window;
  const bool presentation_borderless =
      (GetWindowLongPtrW(presentation_window, GWL_STYLE) & WS_POPUP) != 0;
  const auto presentation_monitor = MonitorFromWindow(presentation_window, MONITOR_DEFAULTTONEAREST);
  // Stable small IDs retain monitor identity in numeric CSV readers without
  // converting a pointer-sized HMONITOR to an imprecise floating-point value.
  static std::vector<HMONITOR> presentation_monitors;
  auto monitor_it = std::find(presentation_monitors.begin(), presentation_monitors.end(), presentation_monitor);
  if (monitor_it == presentation_monitors.end()) {
    presentation_monitors.push_back(presentation_monitor);
    monitor_it = presentation_monitors.end() - 1;
  }
  const u32 presentation_monitor_index = u32(monitor_it - presentation_monitors.begin()) + 1;
  const auto configured_scene_extent = NativeConfiguredSceneExtent(1280,720);
  const int32_t presentation_fps_limit = fps_limit;
  static u32 presentation_refresh_hz = 0;
  {
    auto* chain = presentation_chain;
    const std::array<u64,12> presentation_state{
        width, height, configured_scene_extent.width, configured_scene_extent.height,
        u32(source_desc.Width), source_desc.Height, u64(presentation_focused),
        u64(requested_vsync), u64(presentation_borderless), presentation_monitor_index,
        u64(presentation_fps_limit), u64(spatial_presented)};
    static std::array<u64,12> previous_presentation_state{};
    if (previous_presentation_state != presentation_state) {
      previous_presentation_state = presentation_state;
      BOOL exclusive = FALSE;
      const HRESULT queried = chain->d3d->GetFullscreenState(&exclusive, nullptr);
      REXLOG_INFO("Native presentation: frame={} output={}x{} scene={}x{} source={}x{} focus={} borderless={} monitor={} fps_limit_cvar={} vsync={} tearing={} exclusive={} query={:08X}",
          state.present_number, width, height, configured_scene_extent.width, configured_scene_extent.height,
          u32(source_desc.Width), source_desc.Height, presentation_focused, presentation_borderless,
          presentation_monitor_index, presentation_fps_limit, requested_vsync,
          (chain->swapChainFlags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) != 0,
          bool(exclusive), u32(queried));
      MONITORINFOEXW monitor{};
      monitor.cbSize = sizeof(monitor);
      DEVMODEW display{};
      display.dmSize = sizeof(display);
      presentation_refresh_hz = 0;
      if (GetMonitorInfoW(presentation_monitor, &monitor) &&
          EnumDisplaySettingsW(monitor.szDevice, ENUM_CURRENT_SETTINGS, &display)) {
        presentation_refresh_hz = display.dmDisplayFrequency;
        REXLOG_INFO("Native presentation display: desktop={}x{} refresh={}Hz borderless={}",
            display.dmPelsWidth, display.dmPelsHeight, display.dmDisplayFrequency,
            presentation_borderless);
      }
    }
  }
  static const std::array<int, 2> present_mode = [] {
    int sync = -1, flags = 0;
    if (const char* v = std::getenv("LEGO_NATIVE_PRESENT")) std::sscanf(v, "%d,%d", &sync, &flags);
    REXLOG_INFO("Native GPU: present override sync={} flags={}", sync, flags);
    return std::array<int, 2>{sync, flags};
  }();
  if (present_mode[0] >= 0) {
    auto* chain = static_cast<plume::D3D12SwapChain*>(state.swap_chain.get());
    const HRESULT hr = chain->d3d->Present(UINT(present_mode[0]), UINT(present_mode[1]));
    static std::atomic<u32> failures{0};
    if (FAILED(hr) && failures.fetch_add(1) < 8)
      REXLOG_ERROR("Native GPU: Present({}, {}) failed {:08X}", present_mode[0], present_mode[1], u32(hr));
    presented = SUCCEEDED(hr);
  } else {
    presented = state.swap_chain->present(image_index, signals, 1);
  }
  if (presented) state.fps_counter.RecordPresent(std::chrono::duration<double>(
      std::chrono::steady_clock::now().time_since_epoch()).count());
  if (presented && settings_runtime) settings_runtime->Presented(settings_frame,
      {width, height, present_mode[0] >= 0 ? present_mode[0] != 0 : requested_vsync},
      !fsr1_requested || spatial_presented);
  if (presented) {
    const double fps = state.fps_counter.Value();
    const double frame_ms=state.fps_counter.FrameTimeMilliseconds();
    PublishNativeOverlayStats(fps, frame_ms, u64(state.present_number) + 1);
    // Native presents bypass CommandProcessor's XE_SWAP packet and its
    // profiler flip. F3 reads this snapshot, so publish the actual interval.
    rex::perf::SetCounter(rex::perf::CounterId::kFrameTimeUs,int64_t(frame_ms*1000.0+0.5));
    rex::perf::SetCounter(rex::perf::CounterId::kFps,int64_t(fps));
    rex::perf::ResetFrameCounters();
  }
  if (state.gpu_timestamps)
    state.gpu_timestamps->PresentResult(slot, presented);
  if (timing_enabled || metrics_enabled) state.swap_present_cpu_ms += std::chrono::duration<double, std::milli>(
      std::chrono::steady_clock::now() - swap_start).count();
  ReportLogoCapture();
  if (const auto native = legodimensions::PollNativeConstantPublisher()) {
    REXLOG_INFO("Native constant publisher: live mode={} frame={}",
        *native ? "native" : "legacy", state.present_number);
  }
  PipelineLibraryFrame();
  ++state.present_number;
  g_probe_frame.store(state.present_number);
  if (timing_enabled || metrics_enabled) {
    const auto now = std::chrono::steady_clock::now();
    const auto uploads = ConsumeTextureUploadTiming();
    const auto draws = ConsumeDrawTiming();
    const auto buffers = ConsumeBufferUploadTiming();
    if (state.last_present_time.time_since_epoch().count() != 0) {
      const double interval_ms = std::chrono::duration<double, std::milli>(
          now - state.last_present_time).count();
      static std::ofstream frame_metrics = [] {
        const auto* path = std::getenv("LEGO_NATIVE_FRAME_METRICS");
        std::ofstream stream;
        if (path && *path) {
          stream.open(path);
          stream << "frame,interval_ms,texture_ms,draw_calls,constants_ms,vertices_ms,buffer_hash_ms,texture_hash_bytes,buffer_hash_bytes,present_cpu_ms,sync_calls,sync_ms,cpu_resource_wait_skips";
          for (const char* name : {"other", "idle", "fence", "resource", "callback", "query_begin", "query_release"})
            stream << ",sync_" << name << "_calls,sync_" << name << "_ms";
          stream << ",callbacks_enqueued,callbacks_executed,callbacks_pending"
                 << ",bindings_ms,begin_ms,pipeline_ms,issue_ms,tail_ms,buffer_converted_bytes"
                 << ",frame_slot_wait_calls,frame_slot_wait_ms,acquire_cpu_ms,present_submit_cpu_ms,swap_present_cpu_ms"
                 << ",buffer_watch_hits,buffer_watch_audits,buffer_watch_mismatches"
                 << ",constants_shadow_ms,constants_upload_ms,constants_binding_ms"
                 << ",constants_texture_binding_ms,constants_restore_ms,detailed_timing_enabled"
                 << ",pipeline_creations,pipeline_creation_ms,temporal_pipeline_creations,temporal_pipeline_creation_ms"
                 << ",fsr_enabled,scene_render_width,scene_render_height"
                 << ",output_width,output_height,presentation_focused,native_vsync,present_timing_enabled"
                 << ",presentation_borderless,presentation_monitor_index,presentation_refresh_hz,native_fps_limit_cvar"
                 << ",present_source_width,present_source_height,spatial_fsr_presented\n";
        }
        return stream;
      }();
      if (frame_metrics.is_open()) {
        frame_metrics << state.present_number << ',' << interval_ms << ',' << uploads.cpu_ms << ','
            << draws.calls << ',' << draws.stages_ms[3] << ',' << draws.stages_ms[4] << ','
            << buffers.hash_ms << ',' << uploads.hashed_bytes << ',' << buffers.hashed_bytes << ','
            << (timing_enabled ? std::chrono::duration<double, std::milli>(now - present_start).count() : 0.0) << ','
            << state.sync_calls << ',' << state.sync_ms << ',' << state.cpu_resource_wait_skips;
        for (u32 i = 0; i < state.sync_reason_calls.size(); ++i)
          frame_metrics << ',' << state.sync_reason_calls[i] << ',' << state.sync_reason_ms[i];
        frame_metrics << ',' << state.callbacks_enqueued << ',' << state.callbacks_executed
                      << ',' << state.completion_callbacks.size()
                      << ',' << draws.stages_ms[0] << ',' << draws.stages_ms[1]
                      << ',' << draws.stages_ms[2] << ',' << draws.stages_ms[5]
                      << ',' << draws.stages_ms[6] << ',' << buffers.converted_bytes
                      << ',' << state.frame_slot_wait_calls << ',' << state.frame_slot_wait_ms
                      << ',' << state.acquire_cpu_ms << ',' << state.present_submit_cpu_ms
                      << ',' << state.swap_present_cpu_ms
                      << ',' << buffers.watch_hits << ',' << buffers.watch_audits
                      << ',' << buffers.watch_mismatches;
        for (double elapsed : draws.constants_ms) frame_metrics << ',' << elapsed;
        frame_metrics << ',' << timing_enabled
                      << ',' << draws.pipeline_creations << ',' << draws.pipeline_creation_ms
                      << ',' << draws.temporal_pipeline_creations << ',' << draws.temporal_pipeline_creation_ms
                      << ',' << NativeFsr3Enabled() << ',' << configured_scene_extent.width << ',' << configured_scene_extent.height
                      << ',' << width << ',' << height << ',' << presentation_focused << ',' << requested_vsync
                      << ',' << (timing_enabled || metrics_enabled)
                      << ',' << presentation_borderless << ',' << presentation_monitor_index << ',' << presentation_refresh_hz
                      << ',' << presentation_fps_limit << ',' << u32(source_desc.Width) << ',' << source_desc.Height
                      << ',' << spatial_presented << '\n';
        if (state.present_number % 120 == 0) frame_metrics.flush();
      }
      if (LongProbeEnabled()) LongProbeEvent("frame", interval_ms > 2000.0,
          "interval_ms=", interval_ms, "texture_cpu_ms=", uploads.cpu_ms,
          "texture_calls=", uploads.calls, "source_hits=", uploads.source_hits,
          "converted_bytes=", uploads.converted_bytes, "hashed_bytes=", uploads.hashed_bytes,
          "draw_calls=", draws.calls, "bindings_ms=", draws.stages_ms[0],
          "pipeline_ms=", draws.stages_ms[2], "constants_ms=", draws.stages_ms[3],
          "constants_shadow_ms=", draws.constants_ms[0], "constants_upload_ms=", draws.constants_ms[1],
          "constants_binding_ms=", draws.constants_ms[2], "constants_texture_binding_ms=", draws.constants_ms[3],
          "constants_restore_ms=", draws.constants_ms[4],
          "vertices_ms=", draws.stages_ms[4], "issue_ms=", draws.stages_ms[5],
          "buffer_hash_ms=", buffers.hash_ms, "buffer_hashed_bytes=", buffers.hashed_bytes,
          "presented=", presented);
      if (timing_enabled && interval_ms > 40.0 && (state.timing_log_count++ < 16 ||
                                state.timing_log_count % 30 == 0)) {
        REXLOG_INFO("Native timing: frame={} interval={:.2f}ms texture_cpu={:.2f}ms "
            "calls={} source_hits={} converted={} bytes source={:.2f}ms hash={:.2f}ms hashed={}",
            state.present_number, interval_ms, uploads.cpu_ms, uploads.calls,
            uploads.source_hits, uploads.converted_bytes, uploads.source_ms, uploads.hash_ms,
            uploads.hashed_bytes);
        REXLOG_INFO("Native draw timing: calls={} bindings={:.2f} begin={:.2f} "
            "pipeline={:.2f} constants={:.2f} vertices={:.2f} issue={:.2f} tail={:.2f}ms",
            draws.calls, draws.stages_ms[0], draws.stages_ms[1], draws.stages_ms[2],
            draws.stages_ms[3], draws.stages_ms[4], draws.stages_ms[5], draws.stages_ms[6]);
        REXLOG_INFO("Native constants timing: shadow/shared={:.2f} upload={:.2f} "
            "descriptor/root={:.2f} textures={:.2f} framebuffer={:.2f}ms",
            draws.constants_ms[0], draws.constants_ms[1], draws.constants_ms[2],
            draws.constants_ms[3], draws.constants_ms[4]);
        REXLOG_INFO("Native buffer timing: calls={} hash={:.2f}ms hashed={} converted={} watch_hits={} watch_audits={} watch_mismatches={}",
            buffers.calls, buffers.hash_ms, buffers.hashed_bytes, buffers.converted_bytes,
            buffers.watch_hits, buffers.watch_audits, buffers.watch_mismatches);
        REXLOG_INFO("Native synchronization timing: calls={} cpu_resource_skips={} time={:.2f}ms",
                    state.sync_calls, state.cpu_resource_wait_skips, state.sync_ms);
        REXLOG_INFO("Native synchronization reasons ms: other={:.2f} idle={:.2f} fence={:.2f} resource={:.2f} callback={:.2f} query_begin={:.2f} query_release={:.2f}",
            state.sync_reason_ms[0], state.sync_reason_ms[1], state.sync_reason_ms[2],
            state.sync_reason_ms[3], state.sync_reason_ms[4], state.sync_reason_ms[5], state.sync_reason_ms[6]);
        REXLOG_INFO("Native callback queue: enqueued={} executed={} pending={}",
            state.callbacks_enqueued, state.callbacks_executed, state.completion_callbacks.size());
      }
    }
    state.last_present_time = now;
    state.sync_calls = 0;
    state.cpu_resource_wait_skips = 0;
    state.sync_ms = 0;
    state.frame_slot_wait_calls = 0;
    state.frame_slot_wait_ms = state.acquire_cpu_ms = 0;
    state.present_submit_cpu_ms = state.swap_present_cpu_ms = 0;
    state.sync_reason_calls.fill(0);
    state.sync_reason_ms.fill(0);
    state.callbacks_enqueued = state.callbacks_executed = 0;
  }
  state.snapshot_number = 0;
  state.snapshot_draw_number = 0;
  state.frame_slot = (slot + 1) % state.command_slot_count;
  if (!presented) {
    LongProbeEvent("present_failed", true, "frame=", state.present_number);
    REXLOG_ERROR("Native GPU: swap-chain present failed");
  }
  if (std::getenv("LEGO_NATIVE_GPU_DEBUG")) {
    auto* native = static_cast<plume::D3D12Device*>(state.device.get());
    ID3D12InfoQueue* info = nullptr;
    if (SUCCEEDED(native->d3d->QueryInterface(IID_PPV_ARGS(&info)))) {
      const auto count = info->GetNumStoredMessagesAllowedByRetrievalFilter();
      for (UINT64 i = 0; i < count; ++i) {
        SIZE_T size = 0;
        info->GetMessage(i, nullptr, &size);
        std::vector<u8> storage(size);
        auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
        if (SUCCEEDED(info->GetMessage(i, message, &size)) &&
            message->Severity <= D3D12_MESSAGE_SEVERITY_WARNING) {
          REXLOG_WARN("Native D3D12 [{}]: {}", static_cast<u32>(message->ID), message->pDescription);
        }
      }
      info->ClearStoredMessages();
      info->Release();
    }
  }
  return presented;
}

void HostDevice::BeginTiledPassSnapshot() {
  static const bool start_at_tiling = std::getenv("LEGO_GPU_SNAPSHOT_START_AT_TILING") != nullptr;
  if (!start_at_tiling) return;
  auto recording = LockRecording();
  std::lock_guard lock(g_mutex);
  if (g_state && g_state->snapshot_start_frame == ~0u) {
    g_state->snapshot_start_frame = g_state->present_number;
    g_state->snapshot_start_time = std::chrono::steady_clock::now();
  }
}

bool HostDevice::LongProbeSnapshotActive() {
  if (!LongProbeEnabled()) return false;
  return SnapshotFrameActive();
}

bool HostDevice::SnapshotFrameActive() {
  std::lock_guard lock(g_mutex);
  return g_state && g_state->present_number == g_state->snapshot_selected_frame;
}

void HostDevice::SnapshotTexture(plume::RenderTexture* texture,
                                 std::string_view label, bool render_target) {
  if (t_draw_preparation) {
    RecordNativeCommand(t_draw_preparation->view.commands,
        [texture, name = std::string(label), render_target](auto*) {
          HostDevice::SnapshotTexture(texture, name, render_target);
        });
    return;
  }

  if (!texture || SnapshotDirectory().empty()) return;
  auto recording = LockRecording();
  auto* commands = BeginFrameCommands();
  if (!commands) return;
  std::lock_guard lock(g_mutex);
  if (!g_state) return;
  auto& state = *g_state;
  static const bool start_at_tiling = std::getenv("LEGO_GPU_SNAPSHOT_START_AT_TILING") != nullptr;
  if (state.snapshot_start_frame == ~0u && start_at_tiling) return;
  auto* native_texture = static_cast<plume::D3D12Texture*>(texture)->d3d;
  const auto desc = native_texture->GetDesc();
  // Startup can present hundreds of empty loading frames before any draws.
  // Anchor this cutscene diagnostic to its first actual HDR resolve instead.
  if (state.snapshot_start_frame == ~0u && render_target &&
      desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT) {
    state.snapshot_start_frame = state.present_number;
    state.snapshot_start_time = std::chrono::steady_clock::now();
  }
  if (state.snapshot_start_frame == ~0u) return;
  const u32 frame = state.present_number - state.snapshot_start_frame;
  // Opt-in diagnostic runs can capture later menu/hub frames without restart.
  // Check at most twice a second; consume the trigger once for a whole frame.
  const auto now = std::chrono::steady_clock::now();
  if (now - state.snapshot_trigger_check >= std::chrono::milliseconds(500)) {
    state.snapshot_trigger_check = now;
    std::error_code trigger_error;
    if (std::filesystem::remove(SnapshotDirectory() / "capture-next-frame", trigger_error))
      // The trigger can be noticed in the middle of a pass. Capture the next
      // frame from its first snapshot, not just the remainder of this one.
      state.snapshot_selected_frame = state.present_number + 1;
    const auto burst_path = SnapshotDirectory() / "capture-present-frames";
    std::ifstream burst_file(burst_path);
    u32 requested_frames = 0;
    if (burst_file >> requested_frames) {
      burst_file.close();
      if (std::filesystem::remove(burst_path, trigger_error)) {
        state.probe_present_burst_remaining = std::min(requested_frames, 240u);
        if (LongProbeEnabled()) LongProbeEvent("present_burst_armed", false,
            "frames=", state.probe_present_burst_remaining);
      }
    }
  }
  static const bool time_windows = [] {
    char* value = nullptr;
    size_t length = 0;
    _dupenv_s(&value, &length, "LEGO_GPU_SNAPSHOT_TIME_WINDOWS");
    const bool enabled = value && length > 1;
    std::free(value);
    return enabled;
  }();
  // Diagnostic environment options are selected at process startup. Avoid a
  // complete CRT environment scan on every scene resolve / snapshot attempt.
  static const bool manual_only = std::getenv("LEGO_GPU_SNAPSHOT_MANUAL_ONLY") != nullptr;
  if (manual_only) {
    // Manual mode must also suppress long-probe periodic/anomaly readbacks.
    // Explicit present bursts remain available for title-transition captures.
    const bool present_burst = label == "present" && state.probe_present_burst_remaining;
    if (present_burst) --state.probe_present_burst_remaining;
    if (state.present_number != state.snapshot_selected_frame && !present_burst) return;
  } else if (LongProbeEnabled()) {
    const bool requested = g_probe_capture_requested.exchange(false);
    const bool anomaly_due = requested && now-state.probe_last_anomaly_snapshot >= std::chrono::seconds(10);
    if (now >= state.probe_next_snapshot || anomaly_due) {
      state.snapshot_selected_frame = state.present_number + 1;
      state.probe_next_snapshot = now + std::chrono::seconds(30);
      if (anomaly_due) state.probe_last_anomaly_snapshot = now;
      LongProbeEvent("snapshot_scheduled", false, "target_frame=", state.snapshot_selected_frame,
          "reason=", anomaly_due ? "anomaly_candidate" : "periodic");
    }
    const bool present_burst = label=="present" && state.probe_present_burst_remaining;
    if (present_burst) --state.probe_present_burst_remaining;
    const bool present_sample = present_burst ||
        (label=="present" && now>=state.probe_next_present);
    if (present_sample) state.probe_next_present=now+std::chrono::seconds(5);
    if (state.present_number != state.snapshot_selected_frame && !present_sample) return;
  } else if (time_windows) {
    // Capture across scene changes even when fast stars and slow geometry
    // make fixed frame indices cluster in the first seconds of the cutscene.
    constexpr double windows[] = {0, 8, 16, 24, 32, 48};
    const double elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - state.snapshot_start_time).count();
    if (state.snapshot_time_window < std::size(windows) &&
        elapsed >= windows[state.snapshot_time_window]) {
      state.snapshot_selected_frame = state.present_number;
      do { ++state.snapshot_time_window; }
      while (state.snapshot_time_window < std::size(windows) &&
             elapsed >= windows[state.snapshot_time_window]);
    }
    if (state.present_number != state.snapshot_selected_frame) return;
  } else if (state.present_number != state.snapshot_selected_frame &&
             frame != 0 && frame != 2 && frame != 8 && frame != 32 &&
             frame != 64 && frame != 128 && frame != 256) return;
  // A busy character pass can issue hundreds of draws. Reserve captures for
  // its final resolve and postprocessing rather than exhausting them on meshes.
  static const bool fsr_only=std::getenv("LEGO_GPU_SNAPSHOT_FSR_ONLY")!=nullptr;
  if (fsr_only && label!="pre-fsr-color" && label!="present" && !label.starts_with("fsr-")) return;
  if (label.starts_with("draw-") && state.snapshot_draw_number++ >= 16) return;
  if (state.snapshot_number >= 128) return;
  if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
      desc.SampleDesc.Count != 1 || desc.DepthOrArraySize != 1) return;
  // Only color formats inspected by this startup diagnostic. Never read the
  // combined depth/stencil planes as a packed color texture.
  const u32 format = u32(desc.Format);
  if (format != 2 && format != 28 && format != 87 && format != 10 && format != 11 && format != 13 &&
      format != 34 && format != 35 && format != 41 && format != 61) return;
  std::error_code error;
  std::filesystem::create_directories(SnapshotDirectory(), error);
  if (error) return;
  GpuSnapshot sample;
  UINT64 total_bytes = 0, row_bytes = 0;
  UINT rows = 0;
  auto* native_device = static_cast<plume::D3D12Device*>(state.device.get())->d3d;
  native_device->GetCopyableFootprints(&desc, 0, 1, 0, &sample.footprint,
                                       &rows, &row_bytes, &total_bytes);
  if (rows != desc.Height || total_bytes > 64 * 1024 * 1024) return;
  if (LongProbeEnabled() && state.probe_snapshot_bytes + total_bytes > 12ull*1024*1024*1024) {
    if (!state.probe_budget_reported) {
      state.probe_budget_reported = true;
      LongProbeEvent("snapshot_budget_reached", false, "limit_bytes=", 12ull*1024*1024*1024,
          "event_and_timing_logging_continues=1");
    }
    return;
  }
  sample.readback = state.device->createBuffer(plume::RenderBufferDesc::ReadbackBuffer(total_bytes));
  if (!sample.readback) return;
  state.probe_snapshot_bytes += total_bytes;
  sample.row_bytes = u32(row_bytes);
  sample.dxgi_format = format;
  sample.capture_frame = state.present_number;
  sample.path = SnapshotDirectory() / ("f" + std::to_string(frame) + "-" +
      std::to_string(state.snapshot_number++) + "-" + std::string(label) + ".dds");
  commands->setFramebuffer(nullptr);
  commands->barriers(plume::RenderBarrierStage::COPY,
      plume::RenderTextureBarrier(texture, plume::RenderTextureLayout::COPY_SOURCE));
  D3D12_TEXTURE_COPY_LOCATION source{};
  source.pResource = native_texture;
  source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  D3D12_TEXTURE_COPY_LOCATION target{};
  target.pResource = static_cast<plume::D3D12Buffer*>(sample.readback.get())->d3d;
  target.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  target.PlacedFootprint = sample.footprint;
  RecordNativeCommand(commands, [target, source](auto* list) {
    static_cast<plume::D3D12CommandList*>(list)->d3d->CopyTextureRegion(&target, 0, 0, 0, &source, nullptr);
  });
  commands->barriers(plume::RenderBarrierStage::GRAPHICS,
      plume::RenderTextureBarrier(texture, render_target
          ? plume::RenderTextureLayout::COLOR_WRITE : plume::RenderTextureLayout::SHADER_READ));
  state.snapshots[state.frame_slot].push_back(std::move(sample));
}

std::string_view HostDevice::BackendName() {
  std::lock_guard lock(g_mutex);
  return g_state ? NameOf(g_state->backend) : "none";
}

}  // namespace legodimensions::gpu_native
