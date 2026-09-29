// Backend-neutral host device owner for the native GPU path.
#pragma once

#include <memory>
#include <string_view>

#include <rex/types.h>

namespace plume {
struct RenderBuffer;
struct RenderCommandFence;
struct RenderCommandQueue;
struct RenderCommandList;
struct RenderDescriptorSet;
struct RenderDevice;
struct RenderInterface;
struct RenderPipelineLayout;
struct RenderSwapChain;
struct RenderTexture;
struct RenderTextureView;
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
  static plume::RenderPipelineLayout* PipelineLayout();
  static plume::RenderDescriptorSet* TextureDescriptorSet();
  static plume::RenderDescriptorSet* SamplerDescriptorSet();
  static plume::RenderBuffer* NullVertexBuffer();
  static u32 RegisterTexture(plume::RenderTexture* texture,
                             plume::RenderTextureView* view);
  static void UnregisterTexture(u32 descriptor_index);
  // Returns the current frame's direct command list, opening a new ring slot
  // and waiting only when that slot is being reused.
  static plume::RenderCommandList* BeginFrameCommands();
  // Keeps a released resource alive through the fence of the command list
  // which may still reference it.
  static void RetireResource(std::shared_ptr<void> resource);
  // Copies a single-sample native texture to the host swap chain. This is the
  // first complete submission path and deliberately keeps command allocators
  // in a frame ring so the CPU does not wait for the frame it just submitted.
  static bool PresentTexture(plume::RenderTexture* texture,
                             u32 descriptor_index);
  static std::string_view BackendName();

 private:
  HostDevice() = default;
};

}  // namespace legodimensions::gpu_native
