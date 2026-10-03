#include "gpu_native/device.h"
#include "gpu_native/descriptor_retirement.h"
#include "gpu_native/completion_queue.h"
#include "gpu_native/present_rect.h"

#include "gpu_native/buffers.h"
#include "gpu_native/queries.h"
#include "gpu_native/draw.h"
#include "gpu_native/shaders.h"
#include "gpu_native/textures.h"
#include "gpu_native/state.h"
#include "gpu_native/vertex_declarations.h"
#include "gpu_native/shaders/copy_color_ps.h"
#include "gpu_native/shaders/copy_vs.h"
#include "gpu_native/shaders/resolve_color_ps.h"
#include "gpu_native/shaders/depth_pack_ps.h"
#include "gpu_native/shaders/depth_restore_ps.h"
#include "native_gpu_build_info.h"
#include "gpu_native/long_probe.h"
#include "gpu_native/memory_watch.h"
#include "gpu_native/renderdoc_probe.h"
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

namespace legodimensions::gpu_native {
namespace {

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

struct State {
  static constexpr u32 kFramesInFlight = 3;

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
  u32 sampler_error_logs = 0;
  std::unique_ptr<plume::RenderBuffer> null_vertex_buffer;
  std::array<std::unique_ptr<plume::RenderTexture>, 3> null_textures;
  std::array<std::unique_ptr<plume::RenderTextureView>, 3> null_texture_views;
  std::unique_ptr<plume::RenderBuffer> null_texture_upload;
  bool null_textures_initialized = false;
  std::unique_ptr<plume::RenderShader> copy_vertex_shader;
  std::unique_ptr<plume::RenderShader> copy_pixel_shader;
  std::unique_ptr<plume::RenderPipeline> copy_pipeline;
  std::unique_ptr<plume::RenderShader> resolve_pixel_shader;
  std::array<std::unique_ptr<plume::RenderPipeline>, 4> resolve_pipelines;
  std::array<std::unique_ptr<plume::RenderShader>, 2> depth_alias_shaders;
  std::array<std::unique_ptr<plume::RenderPipeline>, 2> depth_alias_pipelines;
  std::vector<std::unique_ptr<plume::RenderFramebuffer>> framebuffers;
  std::array<std::unique_ptr<plume::RenderCommandList>, kFramesInFlight>
      command_lists;
  std::array<std::unique_ptr<plume::RenderCommandFence>, kFramesInFlight>
      frame_fences;
  std::array<std::unique_ptr<plume::RenderCommandSemaphore>, kFramesInFlight>
      acquire_semaphores;
  std::vector<std::unique_ptr<plume::RenderCommandSemaphore>>
      render_semaphores;
  std::array<bool, kFramesInFlight> frame_submitted{};
  std::array<u64, kFramesInFlight> slot_submission{};
  u64 last_submission = 0, completed_submission = 0;
  CompletionQueue completion_callbacks;
  u64 callbacks_enqueued = 0, callbacks_executed = 0;
  std::array<std::vector<std::unique_ptr<DrawUploadPage>>, kFramesInFlight> draw_uploads;
  u32 draw_upload_page = 0;
  u64 draw_upload_offset = 0;
  std::array<std::vector<std::shared_ptr<void>>, kFramesInFlight>
      retired_resources;
  u32 frame_slot = 0;
  u32 present_number = 0;
  u32 snapshot_number = 0;
  u32 snapshot_draw_number = 0;
  u32 snapshot_start_frame = ~0u;
  std::chrono::steady_clock::time_point snapshot_start_time;
  u32 snapshot_time_window = 0;
  u32 snapshot_selected_frame = ~0u;
  std::chrono::steady_clock::time_point snapshot_trigger_check;
  std::array<std::vector<GpuSnapshot>, kFramesInFlight> snapshots;
  bool command_list_open = false;
  u32 long_wait_log_count = 0;
  u32 timing_log_count = 0;
  u64 sync_calls = 0, cpu_resource_wait_skips = 0;
  double sync_ms = 0;
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
  DescriptorRetirement retired_descriptors{65536};
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
  layout.addPushConstant(3, 4, sizeof(u32) * 4,
                         plume::RenderShaderStageFlag::PIXEL);
  layout.end();
  state.pipeline_layout = layout.create(state.device.get());
  if (!state.pipeline_layout) {
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
  REXLOG_INFO("Native GPU init: present pipeline {}",
              state.copy_pipeline ? "ready" : "failed");
  return state.copy_pipeline != nullptr;
}

bool CreateFrameRing(State& state) {
  REXLOG_INFO("Native GPU init: creating frame ring");
  const u32 texture_count = state.swap_chain->getTextureCount();
  state.framebuffers.clear();
  state.render_semaphores.clear();
  state.framebuffers.reserve(texture_count);
  state.render_semaphores.reserve(texture_count);
  for (u32 i = 0; i < texture_count; ++i) {
    const plume::RenderTexture* attachment = state.swap_chain->getTexture(i);
    state.framebuffers.emplace_back(state.device->createFramebuffer(
        plume::RenderFramebufferDesc(&attachment, 1)));
    state.render_semaphores.emplace_back(
        state.device->createCommandSemaphore());
    if (!state.framebuffers.back() || !state.render_semaphores.back()) {
      return false;
    }
  }

  for (u32 i = 0; i < State::kFramesInFlight; ++i) {
    state.command_lists[i] = state.queue->createCommandList();
    state.frame_fences[i] = state.device->createCommandFence();
    state.acquire_semaphores[i] = state.device->createCommandSemaphore();
    state.frame_submitted[i] = false;
    state.retired_resources[i].clear();
    state.retired_descriptors.CompleteFrame(i,
        [&](u32 index) { state.texture_slots[index] = false; });
    if (!state.command_lists[i] || !state.frame_fences[i] ||
        !state.acquire_semaphores[i]) {
      return false;
    }
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
  for (u32 slot = 0; slot < State::kFramesInFlight; ++slot) {
    if (!state.frame_submitted[slot]) continue;
    auto* fence = static_cast<plume::D3D12CommandFence*>(state.frame_fences[slot].get());
    const u64 completed = fence->d3d->GetCompletedValue();
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
}

bool WaitForSubmittedFramesLocked(State& state) {
  // DXGI's frame-latency object is a present throttle, not a GPU completion
  // fence. Rebuilding/releasing the ring requires all submitted GPU users.
  for (u32 slot = 0; slot < State::kFramesInFlight; ++slot) {
    if (!state.frame_submitted[slot]) continue;
    if (state.backend == Backend::kD3D12) {
      auto* fence = static_cast<plume::D3D12CommandFence*>(state.frame_fences[slot].get());
      if (fence->d3d->GetCompletedValue() == UINT64_MAX) {
        REXLOG_ERROR("Native GPU: cannot rebuild frame ring after device removal");
        return false;
      }
    }
    state.queue->waitForCommandFence(state.frame_fences[slot].get());
    if (state.backend == Backend::kD3D12) {
      auto* fence = static_cast<plume::D3D12CommandFence*>(state.frame_fences[slot].get());
      const u64 completed = fence->d3d->GetCompletedValue();
      if (completed == UINT64_MAX || completed < fence->fenceValue - 1) {
        REXLOG_ERROR("Native GPU: frame-ring GPU wait failed");
        return false;
      }
    }
    state.completed_submission = std::max(state.completed_submission, state.slot_submission[slot]);
    state.frame_submitted[slot] = false;
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
  if ((state.framebuffers.empty() || state.swap_chain->needsResize()) &&
      !RebuildSwapChain(state)) {
    return nullptr;
  }

  const u32 slot = state.frame_slot;
  if (state.frame_submitted[slot]) {
    const auto wait_start = std::chrono::steady_clock::now();
    state.queue->waitForCommandFence(state.frame_fences[slot].get());
    RefreshCompletedSubmissionsLocked(state);
    if (state.backend != Backend::kD3D12)
      state.completed_submission = std::max(state.completed_submission, state.slot_submission[slot]);
    const auto wait_ms = std::chrono::duration<double, std::milli>(
                             std::chrono::steady_clock::now() - wait_start)
                             .count();
    if (wait_ms > 20.0 && state.long_wait_log_count++ < 20) {
      REXLOG_WARN("Native GPU: frame-slot {} GPU wait took {:.2f} ms", slot,
                  wait_ms);
    }
    state.frame_submitted[slot] = false;
    SaveSnapshots(state.snapshots[slot]);
  }
  state.retired_resources[slot].clear();
  state.draw_upload_page = 0;
  state.draw_upload_offset = 0;
  state.retired_descriptors.CompleteFrame(slot,
      [&](u32 index) { state.texture_slots[index] = false; });
  auto* commands = state.command_lists[slot].get();
  commands->begin();
  InitializeNullTextures(state, commands);
  state.command_list_open = true;
  return commands;
}

}  // namespace

std::unique_lock<std::recursive_mutex> HostDevice::LockRecording() {
  static std::recursive_mutex recording_mutex;
  return std::unique_lock(recording_mutex);
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

  state->device = state->render_interface->createDevice();
  if (!state->device) {
    REXLOG_ERROR("Native GPU: failed to create {} device", NameOf(backend));
    return false;
  }
  REXLOG_INFO("Native GPU init: device ready");
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
  REXLOG_INFO("Native GPU init: swap chain ready");
  if (!CreatePresentPipeline(*state) || !CreateNullVertexBuffer(*state) ||
      !CreateNullTextures(*state) || !CreateFrameRing(*state)) {
    REXLOG_ERROR("Native GPU: failed to create present pipeline or frame ring");
    return false;
  }

  const auto& description = state->device->getDescription();
  REXLOG_INFO("Native GPU [{}]: {} ready on '{}' ({}x{}, {} images)",
              kNativeGpuBuildFingerprint, NameOf(backend), description.name,
              state->swap_chain->getWidth(), state->swap_chain->getHeight(),
              state->swap_chain->getTextureCount());
  g_state = std::move(state);
  REXLOG_INFO("Native pixel texture page cache: {} (vertex textures and buffers remain content-checked)",
              CpuMemoryWatchEnabled());
  return true;
}

void HostDevice::Shutdown() {
  auto recording = LockRecording();
  std::lock_guard lock(g_mutex);
  if (!g_state) {
    return;
  }
  WaitForSubmittedFramesLocked(*g_state);
  if (g_state->completion_callbacks.size())
    REXLOG_INFO("Native GPU: cancelling {} completion callbacks at title shutdown",
                g_state->completion_callbacks.size());
  // All GPU users must be idle before Plume resources are released.
  ResetDrawResources();
  ResetQueryResources();
  ResetShaderResources();
  ResetBufferResources();
  ResetTextureResources();
  ResetDrawBindings();
  ResetVertexDeclarations();
  ShutdownCpuMemoryWatch();
  g_state.reset();
}

bool HostDevice::IsReady() {
  std::lock_guard lock(g_mutex);
  return g_state && g_state->device && g_state->queue && g_state->swap_chain;
}

plume::RenderDevice* HostDevice::Device() {
  std::lock_guard lock(g_mutex);
  return g_state ? g_state->device.get() : nullptr;
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

u32 HostDevice::RegisterSampler(const TextureFetchWords& fetch) {
  std::lock_guard lock(g_mutex);
  if (!g_state || !g_state->sampler_descriptors) return 0;
  auto& state = *g_state;
  const auto desc = DecodeTextureSampler(fetch);
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

plume::RenderBuffer* HostDevice::NullVertexBuffer() {
  std::lock_guard lock(g_mutex);
  return g_state ? g_state->null_vertex_buffer.get() : nullptr;
}

u32 HostDevice::RegisterTexture(plume::RenderTexture* texture,
                                plume::RenderTextureView* view) {
  if (!texture || !view) {
    return ~u32{0};
  }
  std::lock_guard lock(g_mutex);
  if (!g_state || !g_state->texture_descriptors) {
    return ~u32{0};
  }
  for (u32 slot = kFirstTextureSlot; slot < g_state->texture_slots.size();
       ++slot) {
    if (g_state->texture_slots[slot]) {
      continue;
    }
    g_state->texture_slots[slot] = true;
    g_state->texture_descriptors->setTexture(
        slot, texture, plume::RenderTextureLayout::SHADER_READ, view);
    return slot;
  }
  REXLOG_ERROR("Native GPU: bindless texture heap exhausted");
  return ~u32{0};
}

void HostDevice::UnregisterTexture(u32 descriptor_index) {
  std::lock_guard lock(g_mutex);
  if (!g_state || descriptor_index < kFirstTextureSlot ||
      descriptor_index >= g_state->texture_slots.size()) {
    return;
  }
  if (!g_state->texture_slots[descriptor_index]) return;
  u32 live_frames = 0;
  for (u32 slot = 0; slot < State::kFramesInFlight; ++slot) {
    if (g_state->frame_submitted[slot] ||
        (g_state->command_list_open && slot == g_state->frame_slot))
      live_frames |= 1u << slot;
  }
  if (g_state->retired_descriptors.Retire(descriptor_index, live_frames))
    g_state->texture_slots[descriptor_index] = false;
}

plume::RenderCommandList* HostDevice::BeginFrameCommands() {
  auto recording = LockRecording();
  std::lock_guard lock(g_mutex);
  if (!g_state) {
    return nullptr;
  }
  return BeginFrameCommandsLocked(*g_state);
}

DrawUploadSlice HostDevice::AllocateDrawUpload(u32 size) {
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
    commands->end();
    state.command_list_open = false;
    state.queue->executeCommandLists(commands, state.frame_fences[slot].get());
    MarkSubmissionLocked(state, slot);
  }

  const auto wait_start = std::chrono::steady_clock::now();
  bool waited = false;
  for (u32 slot = 0; slot < State::kFramesInFlight; ++slot) {
    if (!state.frame_submitted[slot]) {
      continue;
    }
    state.queue->waitForCommandFence(state.frame_fences[slot].get());
    RefreshCompletedSubmissionsLocked(state);
    if (state.backend != Backend::kD3D12)
      state.completed_submission = std::max(state.completed_submission, state.slot_submission[slot]);
    state.frame_submitted[slot] = false;
    state.retired_resources[slot].clear();
    state.retired_descriptors.CompleteFrame(slot,
        [&](u32 index) { state.texture_slots[index] = false; });
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
  auto recording = LockRecording();
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

void HostDevice::PollCompletionCallbacks() {
  // A callback may enter another hook that polls. Keep the original FIFO
  // order without holding a renderer mutex while executing guest code.
  static thread_local bool executing = false;
  if (executing) return;
  static std::mutex execution_mutex;
  std::unique_lock execution_lock(execution_mutex, std::try_to_lock);
  if (!execution_lock.owns_lock()) return;
  struct Guard { bool& flag; Guard(bool& f) : flag(f) { flag = true; }
                 ~Guard() { flag = false; } } guard(executing);
  for (;;) {
    std::function<void()> callback;
    {
      auto recording = LockRecording();
      std::lock_guard lock(g_mutex);
      if (!g_state) return;
      RefreshCompletedSubmissionsLocked(*g_state);
      callback = g_state->completion_callbacks.PopReady(g_state->completed_submission);
      if (!callback) return;
      ++g_state->callbacks_executed;
    }
    callback();
  }
}

bool HostDevice::SubmitRecordedWork() {
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
  commands->end();
  state.command_list_open = false;
  state.queue->executeCommandLists(commands, state.frame_fences[slot].get());
  MarkSubmissionLocked(state, slot);
  state.frame_slot = (slot + 1) % State::kFramesInFlight;
  return true;
}

void HostDevice::RetireResource(std::shared_ptr<void> resource) {
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
  for (u32 slot = 0; slot < State::kFramesInFlight; ++slot) {
    if (g_state->frame_submitted[slot] ||
        (g_state->command_list_open && slot == g_state->frame_slot))
      g_state->retired_resources[slot].push_back(resource);
  }
}

bool HostDevice::ResolveHdrColor(plume::RenderTexture* source, u32 descriptor_index,
                                 plume::RenderTexture* destination,
                                 u32 width, u32 height, float scale,
                                 ColorResolveDestination destination_format, bool source_is_target,
                                 const ResolveRegion* region) {
  auto recording = LockRecording();
  // Acquire the list before g_mutex: BeginFrameCommands owns that mutex too.
  auto* commands = BeginFrameCommands();
  if (!commands || !source || !destination || source == destination) return false;
  const ResolveRegion area = region ? *region : ResolveRegion{0, 0, width, height, 0, 0};
  const i32 dx = i32(area.left) - i32(area.x), dy = i32(area.top) - i32(area.y);
  if (dx < -32768 || dx > 32767 || dy < -32768 || dy > 32767) return false;
  std::shared_ptr<plume::RenderFramebuffer> framebuffer;
  {
    std::lock_guard lock(g_mutex);
    if (!g_state || descriptor_index >= g_state->texture_slots.size() ||
        !g_state->texture_slots[descriptor_index]) return false;
    auto& state = *g_state;
    const size_t format_index = static_cast<size_t>(destination_format);
    if (format_index >= state.resolve_pipelines.size()) return false;
    auto& pipeline = state.resolve_pipelines[format_index];
    if (!state.resolve_pixel_shader) {
      state.resolve_pixel_shader = state.device->createShader(
          g_resolve_color_ps_dxil, sizeof(g_resolve_color_ps_dxil), "main",
          plume::RenderShaderFormat::DXIL);
      if (!state.resolve_pixel_shader) return false;
    }
    if (!pipeline) {
      plume::RenderGraphicsPipelineDesc desc;
      desc.pipelineLayout = state.pipeline_layout.get();
      desc.vertexShader = state.copy_vertex_shader.get();
      desc.pixelShader = state.resolve_pixel_shader.get();
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
          plume::RenderFormat::R32_FLOAT};
      desc.renderTargetFormat[0] = formats[format_index];
      desc.renderTargetBlend[0] = plume::RenderBlendDesc::Copy();
      pipeline = state.device->createGraphicsPipeline(desc);
      if (!pipeline || !static_cast<plume::D3D12GraphicsPipeline*>(pipeline.get())->d3d)
        return false;
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
    const struct { u32 index, index2; float scale, unused; } constants{
        descriptor_index, (u32(dx) & 0xFFFFu) | ((u32(dy) & 0xFFFFu) << 16), scale, 0};
    commands->setGraphicsPushConstants(0, &constants);
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

bool HostDevice::TransferDepthAlias(plume::RenderTexture* source, u32 descriptor_index,
                                   plume::RenderTexture* destination,
                                   u32 width, u32 height, bool restore) {
  auto recording = LockRecording();
  auto* commands = BeginFrameCommands();
  if (!commands || !source || !destination || source == destination || !width || !height)
    return false;
  std::shared_ptr<plume::RenderFramebuffer> framebuffer;
  {
    std::lock_guard lock(g_mutex);
    if (!g_state || descriptor_index >= g_state->texture_slots.size() ||
        !g_state->texture_slots[descriptor_index]) return false;
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
      if (!pipeline || !static_cast<plume::D3D12GraphicsPipeline*>(pipeline.get())->d3d)
        return false;
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
    const struct { u32 index, index2; float unused1, unused2; } constants{descriptor_index, 0, 0, 0};
    commands->setGraphicsPushConstants(0, &constants);
    commands->drawInstanced(3, 1, 0, 0);
    commands->setFramebuffer(nullptr);
    if (!restore) commands->barriers(plume::RenderBarrierStage::GRAPHICS,
        plume::RenderTextureBarrier(destination, plume::RenderTextureLayout::SHADER_READ));
  }
  RetireResource(std::move(framebuffer));
  return true;
}

bool HostDevice::PresentTexture(plume::RenderTexture* texture,
                                u32 descriptor_index) {
  auto recording = LockRecording();
  const auto present_start = std::chrono::steady_clock::now();
  if (!texture || descriptor_index == ~u32{0}) {
    return false;
  }
  SnapshotTexture(texture, "present", false);

  std::lock_guard lock(g_mutex);
  if (!g_state || !g_state->copy_pipeline ||
      descriptor_index >= g_state->texture_slots.size() ||
      !g_state->texture_slots[descriptor_index]) {
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
  if (!state.swap_chain->acquireTexture(
          state.acquire_semaphores[slot].get(), &image_index) ||
      image_index >= state.framebuffers.size()) {
    // Close and submit any recorded uploads/draws so the allocator remains in
    // a valid state while the window is minimized or the swap chain changes.
    commands->end();
    state.command_list_open = false;
    state.queue->executeCommandLists(commands, state.frame_fences[slot].get());
    MarkSubmissionLocked(state, slot);
    state.frame_slot = (slot + 1) % State::kFramesInFlight;
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

  const u32 width = state.swap_chain->getWidth();
  const u32 height = state.swap_chain->getHeight();
  const auto source_desc = static_cast<plume::D3D12Texture*>(texture)->d3d->GetDesc();
  const auto present_rect = FitPresentRect(u32(source_desc.Width), source_desc.Height,
                                         width, height);
  // Clear the whole acquired image, including bars left by a previous size.
  commands->clearColor(0, plume::RenderColor(0.0f, 0.0f, 0.0f, 1.0f));
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
    u32 descriptor_index_2;
    float multiplier;
    float unused;
  } push_constants = {descriptor_index, 0, 1.0f, 0.0f};
  commands->setGraphicsPushConstants(0, &push_constants);
  commands->drawInstanced(3, 1, 0, 0);
  commands->setFramebuffer(nullptr);
  commands->barriers(
      plume::RenderBarrierStage::NONE,
      plume::RenderTextureBarrier(back, plume::RenderTextureLayout::PRESENT));
  commands->end();
  state.command_list_open = false;

  const plume::RenderCommandList* lists[] = {commands};
  plume::RenderCommandSemaphore* waits[] = {
      state.acquire_semaphores[slot].get()};
  plume::RenderCommandSemaphore* signals[] = {
      state.render_semaphores[image_index].get()};
  state.queue->executeCommandLists(lists, 1, waits, 1, signals, 1,
                                   state.frame_fences[slot].get());
  MarkSubmissionLocked(state, slot);
  const bool presented = state.swap_chain->present(image_index, signals, 1);
  ReportLogoCapture();
  ++state.present_number;
  g_probe_frame.store(state.present_number);
  if (NativeTextureTimingEnabled()) {
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
                 << ",bindings_ms,begin_ms,pipeline_ms,issue_ms,tail_ms,buffer_converted_bytes\n";
        }
        return stream;
      }();
      if (frame_metrics.is_open()) {
        frame_metrics << state.present_number << ',' << interval_ms << ',' << uploads.cpu_ms << ','
            << draws.calls << ',' << draws.stages_ms[3] << ',' << draws.stages_ms[4] << ','
            << buffers.hash_ms << ',' << uploads.hashed_bytes << ',' << buffers.hashed_bytes << ','
            << std::chrono::duration<double, std::milli>(now - present_start).count() << ','
            << state.sync_calls << ',' << state.sync_ms << ',' << state.cpu_resource_wait_skips;
        for (u32 i = 0; i < state.sync_reason_calls.size(); ++i)
          frame_metrics << ',' << state.sync_reason_calls[i] << ',' << state.sync_reason_ms[i];
        frame_metrics << ',' << state.callbacks_enqueued << ',' << state.callbacks_executed
                      << ',' << state.completion_callbacks.size()
                      << ',' << draws.stages_ms[0] << ',' << draws.stages_ms[1]
                      << ',' << draws.stages_ms[2] << ',' << draws.stages_ms[5]
                      << ',' << draws.stages_ms[6] << ',' << buffers.converted_bytes << '\n';
        if (state.present_number % 120 == 0) frame_metrics.flush();
      }
      if (LongProbeEnabled()) LongProbeEvent("frame", interval_ms > 2000.0,
          "interval_ms=", interval_ms, "texture_cpu_ms=", uploads.cpu_ms,
          "texture_calls=", uploads.calls, "source_hits=", uploads.source_hits,
          "converted_bytes=", uploads.converted_bytes, "hashed_bytes=", uploads.hashed_bytes,
          "draw_calls=", draws.calls, "bindings_ms=", draws.stages_ms[0],
          "pipeline_ms=", draws.stages_ms[2], "constants_ms=", draws.stages_ms[3],
          "vertices_ms=", draws.stages_ms[4], "issue_ms=", draws.stages_ms[5],
          "buffer_hash_ms=", buffers.hash_ms, "buffer_hashed_bytes=", buffers.hashed_bytes,
          "presented=", presented);
      if (interval_ms > 40.0 && (state.timing_log_count++ < 16 ||
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
        REXLOG_INFO("Native buffer timing: calls={} hash={:.2f}ms hashed={} converted={}",
            buffers.calls, buffers.hash_ms, buffers.hashed_bytes, buffers.converted_bytes);
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
    state.sync_reason_calls.fill(0);
    state.sync_reason_ms.fill(0);
    state.callbacks_enqueued = state.callbacks_executed = 0;
  }
  state.snapshot_number = 0;
  state.snapshot_draw_number = 0;
  state.frame_slot = (slot + 1) % State::kFramesInFlight;
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
  if (!std::getenv("LEGO_GPU_SNAPSHOT_START_AT_TILING")) return;
  auto recording = LockRecording();
  std::lock_guard lock(g_mutex);
  if (g_state && g_state->snapshot_start_frame == ~0u) {
    g_state->snapshot_start_frame = g_state->present_number;
    g_state->snapshot_start_time = std::chrono::steady_clock::now();
  }
}

bool HostDevice::LongProbeSnapshotActive() {
  if (!LongProbeEnabled()) return false;
  std::lock_guard lock(g_mutex);
  return g_state && g_state->present_number == g_state->snapshot_selected_frame;
}

void HostDevice::SnapshotTexture(plume::RenderTexture* texture,
                                 std::string_view label, bool render_target) {
  if (!texture || SnapshotDirectory().empty()) return;
  auto recording = LockRecording();
  auto* commands = BeginFrameCommands();
  if (!commands) return;
  std::lock_guard lock(g_mutex);
  if (!g_state) return;
  auto& state = *g_state;
  if (state.snapshot_start_frame == ~0u &&
      std::getenv("LEGO_GPU_SNAPSHOT_START_AT_TILING")) return;
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
  if (LongProbeEnabled()) {
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
  if (label.starts_with("draw-") && state.snapshot_draw_number++ >= 16) return;
  if (state.snapshot_number >= 128) return;
  if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
      desc.SampleDesc.Count != 1 || desc.DepthOrArraySize != 1) return;
  // Only color formats inspected by this startup diagnostic. Never read the
  // combined depth/stencil planes as a packed color texture.
  const u32 format = u32(desc.Format);
  if (format != 2 && format != 28 && format != 87 && format != 10 && format != 11 && format != 13 &&
      format != 34 && format != 35 && format != 41) return;
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
  static_cast<plume::D3D12CommandList*>(commands)->d3d->CopyTextureRegion(
      &target, 0, 0, 0, &source, nullptr);
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
