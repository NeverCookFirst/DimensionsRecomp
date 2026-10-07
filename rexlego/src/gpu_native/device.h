// Backend-neutral host device owner for the native GPU path.
#pragma once

#include <memory>
#include <functional>
#include <mutex>
#include <string_view>

#include <rex/types.h>
#include "gpu_native/resolve_region.h"
#include "gpu_native/sampler_state.h"

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

// Attribution only: all reasons retain the same completion semantics.
enum class SyncReason : u32 {
  kOther, kIdle, kFence, kResource, kCallback, kQueryBegin, kQueryRelease, kCount
};

enum class ColorResolveDestination { kUnormRGBA16, kFloatRGBA32, kUnormRG16, kDepthFloat32 };

struct DrawUploadSlice {
  plume::RenderBuffer* buffer = nullptr;
  void* mapped = nullptr;
  u64 offset = 0;
  explicit operator bool() const { return buffer && mapped; }
};

// Immutable device objects plus the identity of the currently open recording.
// The caller must hold LockRecording throughout every use of these pointers.
struct DrawDeviceView {
  plume::RenderDevice* device = nullptr;
  plume::RenderPipelineLayout* pipeline_layout = nullptr;
  plume::RenderDescriptorSet* texture_descriptors = nullptr;
  plume::RenderDescriptorSet* sampler_descriptors = nullptr;
  plume::RenderBuffer* null_vertex_buffer = nullptr;
  plume::RenderCommandList* commands = nullptr;
  u64 recording_serial = 0;
};

class HostDevice {
 public:
  // Hold across the whole CPU recording operation, not only list acquisition.
  // Lock order: recording -> resource/cache locks -> device state.
  static std::unique_lock<std::recursive_mutex> LockRecording();
  static bool Create(rex::ui::Window* window, Backend backend = Backend::kD3D12);
  static void Shutdown();
  static bool IsReady();
  // One state-lock acquisition replaces repeated immutable draw getters.
  // Closed recordings return commands=nullptr and recording_serial=0.
  static DrawDeviceView CurrentDrawDeviceView();

  static plume::RenderDevice* Device();
  static plume::RenderCommandQueue* Queue();
  static plume::RenderSwapChain* SwapChain();
  static plume::RenderPipelineLayout* PipelineLayout();
  static plume::RenderDescriptorSet* TextureDescriptorSet();
  static plume::RenderDescriptorSet* SamplerDescriptorSet();
  static u32 RegisterSampler(const TextureFetchWords& fetch);
  static plume::RenderBuffer* NullVertexBuffer();
  static u32 RegisterTexture(plume::RenderTexture* texture,
                             plume::RenderTextureView* view);
  static void UnregisterTexture(u32 descriptor_index);
  // Returns the current frame's direct command list, opening a new ring slot
  // and waiting only when that slot is being reused.
  static plume::RenderCommandList* BeginFrameCommands();
  // Append-only upload storage owned by the current command-list fence slot.
  // It is reset only after that slot's GPU work has completed.
  static DrawUploadSlice AllocateDrawUpload(u32 size);
  // Submit all work recorded so far and wait for every in-flight frame. This
  // is reserved for GPU data dependencies and explicit idle points;
  // the normal present path remains asynchronous through the frame ring.
  static bool Synchronize(SyncReason reason = SyncReason::kOther);
  // Completion is conservative at the end of the containing submission.
  // Enqueue never submits or waits; poll executes guest code outside g_mutex.
  static bool EnqueueCompletionCallback(std::function<void()> callback);
  static void PollCompletionCallbacks();
  static void RecordCpuResourceWaitSkip();
  // Submit the currently recorded list without waiting. Used for the guest's
  // Xenos ring kick-off boundary.
  static bool SubmitRecordedWork();
  // Keeps a released resource alive through the fence of the command list
  // which may still reference it.
  static void RetireResource(std::shared_ptr<void> resource);
  // Copies a single-sample native texture to the host swap chain. This is the
  // first complete submission path and deliberately keeps command allocators
  // in a frame ring so the CPU does not wait for the frame it just submitted.
  static bool PresentTexture(plume::RenderTexture* texture,
                             u32 descriptor_index);
  // Exact texel color scale into UNORM16 storage or a float32 sampling mirror.
  // Width/height select the top-left logical extent of a padded source.
  // Optional validated region supports atlas placement. Depth writes normalized
  // X into an R32_FLOAT mirror; stencil, MSAA and CPU readback are unsupported.
  static bool ResolveHdrColor(plume::RenderTexture* source, u32 descriptor_index,
                              plume::RenderTexture* destination,
                              u32 width, u32 height, float scale,
                              ColorResolveDestination destination_format =
                                  ColorResolveDestination::kUnormRGBA16,
                              bool source_is_target = true,
                              const ResolveRegion* region = nullptr);
  // Single-sample raw D24S8 <-> RGBA8 representations. Pack reads the R32
  // depth sampling mirror; restore writes depth only and retains stencil.
  static bool TransferDepthAlias(plume::RenderTexture* source, u32 descriptor_index,
                                 plume::RenderTexture* destination,
                                 u32 width, u32 height, bool restore);
  // Opt-in intermediate GPU image readback; disabled outside diagnostic runs.
  static void SnapshotTexture(plume::RenderTexture* texture,
                               std::string_view label, bool render_target);
  static void BeginTiledPassSnapshot();
  static bool LongProbeSnapshotActive();
  static std::string_view BackendName();

 private:
  HostDevice() = default;
};

}  // namespace legodimensions::gpu_native
