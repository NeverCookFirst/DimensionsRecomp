#include "gpu_native/device.h"

#include "gpu_native/buffers.h"
#include "gpu_native/shaders.h"
#include "gpu_native/textures.h"
#include "gpu_native/state.h"
#include "gpu_native/vertex_declarations.h"
#include "gpu_native/shaders/copy_color_ps.h"
#include "gpu_native/shaders/copy_vs.h"
#include "native_gpu_build_info.h"

#include <array>
#include <chrono>
#include <mutex>
#include <string>
#include <vector>

#include <plume_render_interface.h>
#include <plume_render_interface_builders.h>
#include <rex/logging.h>
#include <rex/ui/window.h>

namespace plume {
std::unique_ptr<RenderInterface> CreateD3D12Interface();
}  // namespace plume

namespace legodimensions::gpu_native {
namespace {

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
  std::unique_ptr<plume::RenderShader> copy_vertex_shader;
  std::unique_ptr<plume::RenderShader> copy_pixel_shader;
  std::unique_ptr<plume::RenderPipeline> copy_pipeline;
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
  std::array<std::vector<std::shared_ptr<void>>, kFramesInFlight>
      retired_resources;
  u32 frame_slot = 0;
  bool command_list_open = false;
  u32 long_wait_log_count = 0;
  std::vector<bool> texture_slots;
  Backend backend = Backend::kD3D12;
};

constexpr u32 kBindlessTextureCount = 65536;
constexpr u32 kBindlessSamplerCount = 2048;
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
  state.copy_pipeline = state.device->createGraphicsPipeline(desc);
  return state.copy_pipeline != nullptr;
}

bool CreateFrameRing(State& state) {
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
    if (!state.command_lists[i] || !state.frame_fences[i] ||
        !state.acquire_semaphores[i]) {
      return false;
    }
  }
  state.frame_slot = 0;
  return true;
}

bool RebuildSwapChain(State& state) {
  state.swap_chain->wait();
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
  if (state.swap_chain->needsResize() && !RebuildSwapChain(state)) {
    return nullptr;
  }

  const u32 slot = state.frame_slot;
  if (state.frame_submitted[slot]) {
    const auto wait_start = std::chrono::steady_clock::now();
    state.queue->waitForCommandFence(state.frame_fences[slot].get());
    const auto wait_ms = std::chrono::duration<double, std::milli>(
                             std::chrono::steady_clock::now() - wait_start)
                             .count();
    if (wait_ms > 20.0 && state.long_wait_log_count++ < 20) {
      REXLOG_WARN("Native GPU: frame-slot {} GPU wait took {:.2f} ms", slot,
                  wait_ms);
    }
    state.frame_submitted[slot] = false;
  }
  state.retired_resources[slot].clear();
  auto* commands = state.command_lists[slot].get();
  commands->begin();
  state.command_list_open = true;
  return commands;
}

}  // namespace

bool HostDevice::Create(rex::ui::Window* window, Backend backend) {
  std::lock_guard lock(g_mutex);
  if (g_state) {
    return true;
  }
  if (!window || !window->GetNativeWindowHandle()) {
    REXLOG_ERROR("Native GPU: no native window handle");
    return false;
  }

  auto state = std::make_unique<State>();
  state->backend = backend;
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
  if (!CreatePipelineLayout(*state)) {
    REXLOG_ERROR("Native GPU: failed to create bindless pipeline layout");
    return false;
  }
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
  if (!CreatePresentPipeline(*state) || !CreateFrameRing(*state)) {
    REXLOG_ERROR("Native GPU: failed to create present pipeline or frame ring");
    return false;
  }

  const auto& description = state->device->getDescription();
  REXLOG_INFO("Native GPU [{}]: {} ready on '{}' ({}x{}, {} images)",
              kNativeGpuBuildFingerprint, NameOf(backend), description.name,
              state->swap_chain->getWidth(), state->swap_chain->getHeight(),
              state->swap_chain->getTextureCount());
  g_state = std::move(state);
  return true;
}

void HostDevice::Shutdown() {
  std::lock_guard lock(g_mutex);
  if (!g_state) {
    return;
  }
  if (g_state->swap_chain) {
    g_state->swap_chain->wait();
  }
  // All GPU users must be idle before Plume resources are released.
  ResetShaderResources();
  ResetBufferResources();
  ResetTextureResources();
  ResetDrawBindings();
  ResetVertexDeclarations();
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
  g_state->texture_slots[descriptor_index] = false;
}

plume::RenderCommandList* HostDevice::BeginFrameCommands() {
  std::lock_guard lock(g_mutex);
  if (!g_state) {
    return nullptr;
  }
  return BeginFrameCommandsLocked(*g_state);
}

void HostDevice::RetireResource(std::shared_ptr<void> resource) {
  if (!resource) {
    return;
  }
  std::lock_guard lock(g_mutex);
  if (!g_state) {
    return;
  }
  g_state->retired_resources[g_state->frame_slot].emplace_back(
      std::move(resource));
}

bool HostDevice::PresentTexture(plume::RenderTexture* texture,
                                u32 descriptor_index) {
  if (!texture || descriptor_index == ~u32{0}) {
    return false;
  }

  std::lock_guard lock(g_mutex);
  if (!g_state || !g_state->copy_pipeline ||
      descriptor_index >= g_state->texture_slots.size() ||
      !g_state->texture_slots[descriptor_index]) {
    return false;
  }
  State& state = *g_state;
  if (!state.command_list_open && state.swap_chain->needsResize()) {
    if (!RebuildSwapChain(state)) {
      REXLOG_WARN("Native GPU: swap chain resize deferred");
      return false;
    }
  }

  const u32 slot = state.frame_slot;
  plume::RenderCommandList* commands = BeginFrameCommandsLocked(state);
  if (!commands) {
    return false;
  }

  u32 image_index = 0;
  if (!state.swap_chain->acquireTexture(
          state.acquire_semaphores[slot].get(), &image_index) ||
      image_index >= state.framebuffers.size()) {
    // Close and submit any recorded uploads/draws so the allocator remains in
    // a valid state while the window is minimized or the swap chain changes.
    commands->end();
    state.command_list_open = false;
    state.queue->executeCommandLists(commands, state.frame_fences[slot].get());
    state.frame_submitted[slot] = true;
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
  commands->setViewports(
      plume::RenderViewport(0.0f, 0.0f, float(width), float(height)));
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
  state.frame_submitted[slot] = true;
  const bool presented = state.swap_chain->present(image_index, signals, 1);
  state.frame_slot = (slot + 1) % State::kFramesInFlight;
  if (!presented) {
    REXLOG_ERROR("Native GPU: swap-chain present failed");
  }
  return presented;
}

std::string_view HostDevice::BackendName() {
  std::lock_guard lock(g_mutex);
  return g_state ? NameOf(g_state->backend) : "none";
}

}  // namespace legodimensions::gpu_native
