// Backend-neutral host device owner for the native GPU path.
#pragma once

#include <memory>
#include <string_view>

namespace plume {
struct RenderCommandFence;
struct RenderCommandQueue;
struct RenderDevice;
struct RenderInterface;
struct RenderSwapChain;
}  // namespace plume

namespace rex::ui {
class Window;
}  // namespace rex::ui

namespace legodimensions::gpu_native {

enum class Backend {
  kD3D12,
  kVulkan,
};

class HostDevice {
 public:
  static bool Create(rex::ui::Window* window, Backend backend = Backend::kD3D12);
  static void Shutdown();
  static bool IsReady();

  static plume::RenderDevice* Device();
  static plume::RenderCommandQueue* Queue();
  static plume::RenderSwapChain* SwapChain();
  static std::string_view BackendName();

 private:
  HostDevice() = default;
};

}  // namespace legodimensions::gpu_native
