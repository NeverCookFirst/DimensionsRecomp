#include "gpu_native/device.h"

#include "gpu_native/shaders.h"
#include "native_gpu_build_info.h"

#include <mutex>
#include <string>

#include <plume_render_interface.h>
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
  Backend backend = Backend::kD3D12;
};

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
  ResetShaderResources();
  if (g_state->swap_chain) {
    g_state->swap_chain->wait();
  }
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

std::string_view HostDevice::BackendName() {
  std::lock_guard lock(g_mutex);
  return g_state ? NameOf(g_state->backend) : "none";
}

}  // namespace legodimensions::gpu_native
