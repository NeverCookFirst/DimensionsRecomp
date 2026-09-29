#include <cstring>

#include <rex/hook.h>
#include <rex/system/kernel_state.h>
#include <rex/types.h>

#include "gpu_native/d3d.h"
#include "gpu_native/device.h"

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

  auto* device = reinterpret_cast<D3DDevice*>(memory->virtual_membase() + guest_address);
  CopyStateDispatch(device, memory->virtual_membase());
  device->viewport.width = 1280;
  device->viewport.height = 720;
  device->viewport.max_z = 1.0f;
  device->scissor.right = 1280;
  device->scissor.bottom = 720;
  *out_device = guest_address;
  return 0;
}

}  // namespace
}  // namespace legodimensions::gpu_native

REX_HOOK(sub_83FAF9A0, legodimensions::gpu_native::Direct3DCreateDeviceHook);
