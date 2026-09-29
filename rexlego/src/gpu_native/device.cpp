#include "gpu_native/device.h"

#include "gpu_native/buffers.h"
#include "gpu_native/shaders.h"
#include "gpu_native/textures.h"
#include "gpu_native/state.h"
#include "gpu_native/vertex_declarations.h"
#include "native_gpu_build_info.h"

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
  std::unique_ptr<plume::RenderInterface> render_interface;
  std::unique_ptr<plume::RenderDevice> device;
  std::unique_ptr<plume::RenderCommandQueue> queue;
  std::unique_ptr<plume::RenderSwapChain> swap_chain;
  std::unique_ptr<plume::RenderCommandFence> idle_fence;
  std::unique_ptr<plume::RenderPipelineLayout> pipeline_layout;
  std::unique_ptr<plume::RenderDescriptorSet> texture_descriptors;
  std::unique_ptr<plume::RenderDescriptorSet> sampler_descriptors;
  std::unique_ptr<plume::RenderSampler> default_sampler;
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

std::string_view HostDevice::BackendName() {
  std::lock_guard lock(g_mutex);
  return g_state ? NameOf(g_state->backend) : "none";
}

}  // namespace legodimensions::gpu_native
