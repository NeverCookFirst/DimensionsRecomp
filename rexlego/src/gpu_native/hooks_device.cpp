#include <algorithm>
#include <array>
#include <cstring>
#include <cstdlib>
#include <iterator>
#include <unordered_set>

#include "gpu_native/renderer_route.h"
#include <rex/ppc.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/kernel_state.h>
#include <rex/types.h>

#include "gpu_native/d3d.h"
#include "gpu_native/device.h"
#include "gpu_native/state.h"
#include "gpu_native/buffers.h"
#include "gpu_native/textures.h"
#include "gpu_native/shaders.h"

namespace legodimensions::gpu_native {
namespace {

constexpr u32 kRenderStateTable = 0x847F9B18;
constexpr u32 kSamplerStateTable = 0x847F9FD8;

void CopyStateDispatch(D3DDevice* device, u8* membase) {
  const auto* render = reinterpret_cast<const be_u32*>(membase + kRenderStateTable);
  const auto* sampler = reinterpret_cast<const be_u32*>(membase + kSamplerStateTable);
  for (u32 i = 0; i < kRenderStateCount; ++i) {
    device->get_render_state[i] = render[i * 3 + 0];
    device->set_render_state[i] = render[i * 3 + 1];
  }
  for (u32 i = 0; i < kSamplerStateCount; ++i) {
    device->get_sampler_state[i] = sampler[i * 3 + 0];
    device->set_sampler_state[i] = sampler[i * 3 + 1];
  }
}

bool InitializeRenderDefaults(u32 guest_address, u8* membase) {
  // Only statically audited leaf alpha/blend/write-mask/float setters. Use the actual
  // TU23 descriptor defaults, not guessed reset values or full queue init.
  constexpr u32 indices[] = {15,16,18,19,20,21,22,23,24,25,26,53,54,55,56,59};
  constexpr u32 setters[] = {0x83FB7E38,0x83FB81C8,0x83FB7F58,0x83FB7FE8,
      0x83FB7EC8,0x83FB80E8,0x83FB8158,0x83FB8078,0x83FB7E00,0x83FB8260,0x83FB82C0,
      0x83FB8A68,0x83FB8AA8,0x83FB8AE8,0x83FB8B28,0x83FB92B8};
  const auto* table = reinterpret_cast<const be_u32*>(membase + kRenderStateTable);
  auto* kernel_state = REX_KERNEL_STATE();
  auto* dispatcher = kernel_state ? kernel_state->function_dispatcher() : nullptr;
  if (!dispatcher) return false;
  for (u32 i = 0; i < std::size(indices); ++i) {
    const u32 address = table[indices[i] * 3 + 1];
    if (address != setters[i] || !dispatcher->GetFunction(address)) {
      REXLOG_ERROR("Native GPU: unexpected render default setter {:08X}", address);
      return false;
    }
  }
  for (u32 i = 0; i < std::size(indices); ++i) {
    const u32 address = table[indices[i] * 3 + 1];
    const u32 value = table[indices[i] * 3 + 2];
    rex::ppc::GuestToHostFunction<void>(dispatcher->GetFunction(address), guest_address, value);
    REXLOG_INFO("Native GPU: render default setter={:08X} raw={:08X}", address, value);
  }
  return true;
}

bool InitializeSamplerDefaults(D3DDevice* device, u32 guest_address, u8* membase) {
  // Only these statically verified leaf setters are safe on the detached
  // device. Never enter full 83FC9450, which also initializes Xbox queues.
  constexpr std::array<u32, kSamplerStateCount> allowed{
      0x83FB9DB8, 0x83FB9E08, 0x83FB9E58, 0x83FB97E8, 0x83FB9640,
      0x83FB9988, 0x83FB9A88, 0x83FB9BA8, 0x83FB9C48, 0x83FB9CC8,
      0x83FB9B00, 0x83FB9D48, 0x83FB9EA8, 0x83FB9FB0, 0x83FB9F00,
      0x83FB9F58, 0x83FBA008, 0x83FB98E8, 0x83FB9740, 0x83FB99E0};
  auto* kernel = rex::system::kernel_state();
  auto* dispatcher = kernel ? kernel->function_dispatcher() : nullptr;
  if (!dispatcher) return false;
  const auto* table = reinterpret_cast<const be_u32*>(membase + kSamplerStateTable);
  // Check the entire table before executing any entry, including game data.
  for (u32 k = 0; k < kSamplerStateCount; ++k) {
    const u32 address = table[k * 3 + 1];
    if (std::find(allowed.begin(), allowed.end(), address) == allowed.end() ||
        !dispatcher->GetFunction(address)) {
      REXLOG_ERROR("Native GPU: unsafe sampler initializer k={} setter={:08X}", k, address);
      return false;
    }
    REXLOG_INFO("Native sampler default k={} setter={:08X} value={:08X}",
                k, address, u32(table[k * 3 + 2]));
  }
  // Sampler-major order matches the original. Min/max mip requests are saved
  // even with no bound texture, then applied by SetNativeTexture on a bind.
  for (u32 s = 0; s < kFetchConstantCount; ++s) {
    for (u32 k = 0; k < kSamplerStateCount; ++k) {
      auto* setter = dispatcher->GetFunction(table[k * 3 + 1]);
      rex::ppc::GuestToHostFunction<void>(setter, guest_address, s, u32(table[k * 3 + 2]));
    }
    SetNativeTexture(device, s, 0, u64{1} << (31 - s));
  }
  REXLOG_INFO("Native GPU: initialized {} sampler fetch shadows", kFetchConstantCount);
  return true;
}

bool InitializeStencilDefaults(u32 guest_address, u8* membase) {
  // TU23 descriptors 27..42 are CPU-only leaves. Do not enter queue init.
  constexpr u32 setters[] = {0x83FB8570,0x83FB85B0,0x83FB8618,0x83FB8650,
      0x83FB8688,0x83FB85E8,0x83FB8788,0x83FB87A8,0x83FB87C8,0x83FB86E8,
      0x83FB8720,0x83FB8758,0x83FB86B8,0x83FB87E8,0x83FB8808,0x83FB8828};
  const auto* table = reinterpret_cast<const be_u32*>(membase + kRenderStateTable);
  auto* kernel = REX_KERNEL_STATE();
  auto* dispatcher = kernel ? kernel->function_dispatcher() : nullptr;
  if (!dispatcher) return false;
  for (u32 i = 0; i < 16; ++i) {
    const u32 address = table[(27 + i) * 3 + 1];
    if (address != setters[i] || !dispatcher->GetFunction(address)) return false;
  }
  for (u32 i = 0; i < 16; ++i) {
    const u32 address = table[(27 + i) * 3 + 1];
    rex::ppc::GuestToHostFunction<void>(dispatcher->GetFunction(address), guest_address,
                                     u32(table[(27 + i) * 3 + 2]));
  }
  REXLOG_INFO("Native GPU: initialized isolated stencil candidate defaults");
  return true;
}

u32 Direct3DCreateDeviceHook(u32 /*adapter*/, u32 /*device_type*/, u32 /*focus_window*/,
                             u32 /*behavior_flags*/, u32 /*present_params*/,
                             mapped_u32 out_device) {
  if (!out_device || !HostDevice::IsReady()) {
    return 0x8007000Eu;
  }
  *out_device = 0;

  auto* memory = REX_KERNEL_MEMORY();
  const u32 guest_address = memory->SystemHeapAlloc(kGuestDeviceSize, 0x80);
  if (!guest_address) {
    return 0x8007000Eu;
  }
  memory->Zero(guest_address, kGuestDeviceSize);
  ResetDrawBindings();

  auto* device = reinterpret_cast<D3DDevice*>(memory->virtual_membase() + guest_address);
  CopyStateDispatch(device, memory->virtual_membase());
  if (std::getenv("LEGO_NATIVE_VIEWPORT")) {
    // CPU register defaults from TU23 83FCA320; 83FB8FD0 subsequently toggles
    // the viewport enable bits and CLIP_DISABLE without entering GPU init.
    auto* bytes = reinterpret_cast<u8*>(device);
    *reinterpret_cast<be_u32*>(bytes + 10564) = 1u << 19;
    *reinterpret_cast<be_u32*>(bytes + 10572) = 0x43F;
  }
  if (!InitializeRenderDefaults(guest_address, memory->virtual_membase()) ||
      !InitializeSamplerDefaults(device, guest_address, memory->virtual_membase()) ||
      (std::getenv("LEGO_NATIVE_STENCIL") &&
       !InitializeStencilDefaults(guest_address, memory->virtual_membase()))) {
    memory->SystemHeapFree(guest_address);
    return 0x80004005u;
  }
  device->viewport.width = 1280.0f;
  device->viewport.height = 720.0f;
  device->viewport.max_z = 1.0f;
  device->scissor.right = 1280;
  device->scissor.bottom = 720;
  *out_device = guest_address;
  return 0;
}

// The Xenos implementation dirties the emulated ring/device globals. Native
// buffers are CPU-visible mirrors and D3D12 barriers are recorded explicitly,
// so there is no guest GPU cache to invalidate.
void InvalidateGpuCacheHook(D3DDevice* /*device*/, u32 /*address*/,
                            u32 /*size*/, u32 /*flags*/) {}

// Register allocation is an Xenos microcode scheduling knob. AOT DXIL has
// already gone through the desktop compiler's register allocation.
void SetShaderGprAllocationHook(D3DDevice* /*device*/, u32 /*vertex_gprs*/,
                                u32 /*pixel_gprs*/) {}

// Presentation pacing is owned by the host swap chain. The original helper
// writes to the Xenos ring globals that do not exist in detached-native mode.
void SynchronizeToPresentationIntervalHook(D3DDevice* /*device*/,
                                           u32 /*interval*/) {}

void BlockUntilIdleHook(D3DDevice* /*device*/) {
  HostDevice::Synchronize(SyncReason::kIdle);
  HostDevice::PollCompletionCallbacks();
}

// TU23 83FBF8A0 takes the fence marker in r3, NOT a device pointer.
// Both original wrappers fetch an Xbox-only global device before reaching
// CBlocker. A full host drain is conservative (waits for more work), but keeps
// ordering without reading that nonexistent ring or claiming immediate completion.
void BlockOnFenceHook(u32 /*fence_marker*/) {
  if (!HostDevice::Synchronize(SyncReason::kFence))
    REXLOG_ERROR("Native GPU: BlockOnFence failed; host device unavailable");
  HostDevice::PollCompletionCallbacks();
}

void BlockUntilNotBusyHook(u32 resource) {
  auto recording = HostDevice::LockRecording();
  // Opt-in until the controlled cutscene/title test passes. Native uploads
  // copy CPU data into GPU-owned storage and retain that storage at fences.
  // Updating those guest bytes cannot overwrite an earlier GPU draw.
  static const bool asynchronous_cpu = std::getenv("LEGO_NATIVE_ASYNC_CPU_RESOURCES") != nullptr;
  if (asynchronous_cpu && (IsNativeBuffer(resource) || IsNativeShader(resource) ||
                           IsCpuUploadedTexture(resource))) {
    HostDevice::RecordCpuResourceWaitSkip();
    return;
  }
  // Unknown resources and host-resolved textures keep the full wait.
  if (!HostDevice::Synchronize(SyncReason::kResource))
    REXLOG_ERROR("Native GPU: BlockUntilNotBusy failed; host device unavailable");
  recording.unlock();
  HostDevice::PollCompletionCallbacks();
}

void KickOffSegmentHook(D3DDevice* /*device*/) {
  HostDevice::SubmitRecordedWork();
  HostDevice::PollCompletionCallbacks();
}

u32 KickOffHook(D3DDevice* device) {
  HostDevice::SubmitRecordedWork();
  HostDevice::PollCompletionCallbacks();
  if (!device) {
    return 0;
  }
  // Callers store the returned ring marker before comparing it later. Give
  // them a stable, device-owned scratch marker while Plume owns the real
  // submission fence.
  auto* base = REX_KERNEL_MEMORY()->virtual_membase();
  return static_cast<u32>(reinterpret_cast<u8*>(device) - base) + 0x6000;
}

// Return without waiting, like Xenos. All callbacks (including profiler
// markers) run after their containing submission completes, on a guest hook
// thread. No early timestamp shortcut or worker-thread guest invocation.
void InsertCallbackHook(D3DDevice* /*device*/, u32 flags,
                        u32 callback, u32 callback_data) {
  auto recording = HostDevice::LockRecording();
  if (!callback) {
    return;
  }
  auto* kernel = rex::system::kernel_state();
  auto* dispatcher = kernel ? kernel->function_dispatcher() : nullptr;
  auto* function = dispatcher ? dispatcher->GetFunction(callback) : nullptr;
  if (!function) {
    REXLOG_ERROR("Native GPU: callback 0x{:08X} is not recompiled", callback);
    return;
  }
  static std::unordered_set<u32> logged_callbacks;
  if (logged_callbacks.size() < 32 && logged_callbacks.insert(callback).second)
    REXLOG_INFO("Native GPU: deferred callback={:08X} context={:08X} flags={:08X}",
                callback, callback_data, flags);
  if (!HostDevice::EnqueueCompletionCallback([function, callback_data] {
        rex::ppc::GuestToHostFunction<void>(function, callback_data);
      })) {
    REXLOG_WARN("Native GPU: callback 0x{:08X} skipped; device is unavailable", callback);
    return;
  }
  recording.unlock();
  HostDevice::PollCompletionCallbacks();
}

}  // namespace
}  // namespace legodimensions::gpu_native

REX_HOOK(sub_83FAF9A0, legodimensions::gpu_native::Direct3DCreateDeviceHook);
REX_HOOK(sub_83FC5A58,
         legodimensions::gpu_native::InvalidateGpuCacheHook);
REX_HOOK(sub_83FB6DD0,
         legodimensions::gpu_native::SetShaderGprAllocationHook);
REX_HOOK(sub_83FB2F20,
         legodimensions::gpu_native::SynchronizeToPresentationIntervalHook);
REX_HOOK(sub_83FC0EC0, legodimensions::gpu_native::BlockUntilIdleHook);
REX_HOOK(sub_83FBF8A0, legodimensions::gpu_native::BlockOnFenceHook);
REX_HOOK(sub_83FBF900, legodimensions::gpu_native::BlockUntilNotBusyHook);
REX_HOOK(sub_83FC08D0, legodimensions::gpu_native::KickOffSegmentHook);
REX_HOOK(sub_83FC0C10, legodimensions::gpu_native::KickOffHook);
REX_HOOK(sub_83FC0F38, legodimensions::gpu_native::InsertCallbackHook);
