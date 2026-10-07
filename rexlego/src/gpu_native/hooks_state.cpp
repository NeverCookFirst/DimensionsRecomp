#include "gpu_native/renderer_route.h"
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#include <rex/types.h>

#include "gpu_native/d3d.h"
#include "gpu_native/state.h"
#include "gpu_native/textures.h"
#include "gpu_native/vertex_declarations.h"
#include "gpu_native/sampler_state.h"
#include "gpu_native/depth_state.h"
#include "gpu_native/scissor_state.h"
#include "gpu_native/device.h"
#include <rex/logging.h>

extern "C" void __imp__sub_83FBB110(PPCContext& __restrict ctx, u8* base);

namespace legodimensions::gpu_native {
void SetNativeTexture(D3DDevice* device, u32 sampler, u32 texture, u64 dirty) {
  if (!device || sampler >= kNativeTextureSlots) return;
  auto* base = REX_KERNEL_MEMORY()->virtual_membase();
  TextureFetchWords words;
  for (u32 i = 0; i < words.size(); ++i) words[i] = device->fetch_constants[sampler].dword[i];
  if (texture) {
    const auto* header = reinterpret_cast<const D3DTexture*>(base + texture);
    TextureFetchWords texture_words;
    for (u32 i = 0; i < texture_words.size(); ++i) texture_words[i] = header->format.dword[i];
    const auto* bytes = reinterpret_cast<const u8*>(device);
    words = MergeTextureFetch(words, texture_words, bytes[12356 + sampler], bytes[12382 + sampler]);
    RefreshTextureHeader(texture);
    auto* dirty_mask = reinterpret_cast<be_u64*>(reinterpret_cast<u8*>(device) + 24);
    *dirty_mask = u64(*dirty_mask) | dirty;
  } else {
    // Original NULL branch clears only type and binding, keeping sampler bits.
    ClearTextureFetch(words);
  }
  for (u32 i = 0; i < words.size(); ++i) device->fetch_constants[sampler].dword[i] = words[i];
  device->textures[sampler] = texture;
  BindTexture(sampler, texture);
}

namespace {

void SetStreamSourceHook(D3DDevice* device, u32 stream, u32 buffer,
                         u32 offset, u32 stride) {
  if (device && stream < 18) {
    device->vertex_stream_buffers[stream] = buffer;
  }
  BindVertexStream(stream, buffer, offset, stride);
}

void SetIndicesHook(D3DDevice* device, u32 buffer) {
  if (device) {
    device->index_buffer = buffer;
  }
  BindIndexBuffer(buffer);
}

void SetVertexDeclarationHook(D3DDevice* device, u32 declaration) {
  if (device) {
    device->vertex_declaration = declaration;
  }
  BindVertexDeclaration(declaration);
}

u32 CreateVertexDeclarationHook(u32 elements) {
  return CreateVertexDeclarationResource(elements);
}

void SetRenderTargetHook(D3DDevice* device, u32 index, u32 surface) {
  if (device && index < kNativeRenderTargets) {
    device->render_targets[index] = surface;
  }
  BindRenderTarget(index, surface);
}

void SetDepthStencilHook(D3DDevice* device, u32 surface) {
  if (device) {
    device->depth_stencil = surface;
    auto* bytes = reinterpret_cast<u8*>(device);
    auto* control = reinterpret_cast<be_u32*>(bytes + 10548);
    *control = RebindDepthStencilControl(*control,
        *reinterpret_cast<const be_u32*>(bytes + 12308),
        *reinterpret_cast<const be_u32*>(bytes + 12312), surface != 0);
  }
  BindDepthStencil(surface);
}

u32 GetRenderTargetHook(D3DDevice* /*device*/, u32 index) {
  const u32 surface = BoundRenderTarget(index);
  if (surface && IsNativeTexture(surface)) {
    AddRefNativeTexture(surface);
  }
  return surface;
}

u32 GetDepthStencilHook(D3DDevice* /*device*/) {
  const u32 surface = BoundDepthStencil();
  if (surface && IsNativeTexture(surface)) {
    AddRefNativeTexture(surface);
  }
  return surface;
}

bool NativeViewportRange(u32 address, u32 length, bool write) {
  auto* memory = REX_KERNEL_MEMORY();
  if (!memory || !address || !length || u64(address) + length > 0x100000000ull)
    return false;
  auto* heap = memory->LookupHeap(address);
  if (!heap || !heap->IsRangeCommittedReadable(address, length)) return false;
  if (!write) return true;
  const u32 page_size = heap->page_size();
  if (!page_size) return false;
  for (u64 at = address; at < u64(address) + length;) {
    u32 protect = 0;
    if (!heap->QueryProtect(u32(at), &protect) ||
        !(protect & (rex::memory::kMemoryProtectWrite |
                     rex::memory::kMemoryProtectWriteCombine))) return false;
    const u64 next = (at / page_size + 1) * page_size;
    if (next <= at) return false;
    at = next;
  }
  return true;
}

bool NativeViewportDeviceArguments(u32 device_address, u32 stack, u32 scratch = 512) {
  if (!NativeViewportRange(device_address, kGuestDeviceSize, true) ||
      stack < scratch || !NativeViewportRange(stack - scratch, scratch, true)) return false;
  const auto* device = REX_KERNEL_MEMORY()->TranslateVirtual<const D3DDevice*>(device_address);
  const u32 surface = device->render_targets[0] ? u32(device->render_targets[0])
                                              : u32(device->depth_stencil);
  return !surface || NativeViewportRange(surface, sizeof(D3DSurface), false);
}

bool NativeViewportArguments(u32 device_address, u32 viewport_address, u32 stack) {
  return NativeViewportDeviceArguments(device_address, stack) &&
      NativeViewportRange(viewport_address, sizeof(D3DViewport9), false);
}

class ScopedNativeViewportExtent {
 public:
  ScopedNativeViewportExtent(D3DDevice* device, u32 width, u32 height)
      : bytes_(reinterpret_cast<u8*>(device)), flags_(bytes_[11068]),
        width_(*reinterpret_cast<be_u32*>(bytes_ + 13556)),
        height_(*reinterpret_cast<be_u32*>(bytes_ + 13560)) {
    *reinterpret_cast<be_u32*>(bytes_ + 13556) = width;
    *reinterpret_cast<be_u32*>(bytes_ + 13560) = height;
    bytes_[11068] = flags_ | 0x10;
  }
  ~ScopedNativeViewportExtent() {
    bytes_[11068] = flags_;
    *reinterpret_cast<be_u32*>(bytes_ + 13556) = width_;
    *reinterpret_cast<be_u32*>(bytes_ + 13560) = height_;
  }
  ScopedNativeViewportExtent(const ScopedNativeViewportExtent&) = delete;
  ScopedNativeViewportExtent& operator=(const ScopedNativeViewportExtent&) = delete;
 private:
  u8* bytes_;
  u8 flags_;
  u32 width_, height_;
};

void SetScissorRectHook(D3DDevice* device, const D3DRect* rect) {
  if (device && rect) {
    device->scissor = *rect;
  }
}

void SetScissorEnableHook(D3DDevice* device, u32 enabled) {
  if (device) {
    // Keep the requested rectangle intact across disable/re-enable. Native
    // draws derive the effective intersection; no Xbox packet is emitted.
    *reinterpret_cast<be_u32*>(reinterpret_cast<u8*>(device) +
        kNativeScissorEnableOffset) = enabled;
  }
}

// The native backend does not execute Xbox query predicates yet. Entering the
// original XDK routine is unsafe because it dereferences private command-buffer
// arrays that do not exist in our deliberately minimal guest D3DDevice.
void SetPredicationHook(D3DDevice* /*device*/, u32 /*predicate*/) {}

// Emits an Xbox command-buffer format/HiZ packet in the Sep'13 runtime. Native
// render-target transitions own this responsibility on the host backend.
void SetupFormatHook(D3DDevice* /*device*/, u32 /*mode*/) {}

}  // namespace
}  // namespace legodimensions::gpu_native

REX_HOOK(sub_83FB58A8, legodimensions::gpu_native::SetNativeTexture);
REX_HOOK(sub_83FBA160, legodimensions::gpu_native::SetStreamSourceHook);
REX_HOOK(sub_83FBA308, legodimensions::gpu_native::SetIndicesHook);
REX_HOOK(sub_83FB6D50,
         legodimensions::gpu_native::CreateVertexDeclarationHook);
REX_HOOK(sub_83FB6C48,
         legodimensions::gpu_native::SetVertexDeclarationHook);
REX_HOOK(sub_83FBAAA8, legodimensions::gpu_native::SetRenderTargetHook);
REX_HOOK(sub_83FBAE38, legodimensions::gpu_native::SetDepthStencilHook);
// SetRenderTargets is a separate bulk setter, not a caller of the individual
// setters above. Preserve its XDK shadow/viewport updates, then publish all
// five attachments to the native renderer. Save r3 before the guest call.
REX_HOOK_RAW(sub_83FBB110) {
  auto recording = legodimensions::gpu_native::HostDevice::LockRecording();
  auto* device = reinterpret_cast<legodimensions::gpu_native::D3DDevice*>(base + ctx.r3.u32);
  __imp__sub_83FBB110(ctx, base);
  for (u32 i = 0; i < legodimensions::gpu_native::kNativeRenderTargets; ++i)
    legodimensions::gpu_native::BindRenderTarget(i, device->render_targets[i]);
  legodimensions::gpu_native::BindDepthStencil(device->depth_stencil);
  static u32 logs = 0;
  if (logs++ < 12)
    REXLOG_INFO("Native GPU: bulk attachments rt={:08X},{:08X},{:08X},{:08X} ds={:08X}",
        u32(device->render_targets[0]), u32(device->render_targets[1]),
        u32(device->render_targets[2]), u32(device->render_targets[3]), u32(device->depth_stencil));
}
REX_HOOK(sub_83FBA398, legodimensions::gpu_native::GetRenderTargetHook);
REX_HOOK(sub_83FBA3E0, legodimensions::gpu_native::GetDepthStencilHook);
REX_HOOK_RAW(sub_83FBA978) {
  auto recording = legodimensions::gpu_native::HostDevice::LockRecording();
  if (!legodimensions::gpu_native::NativeViewportArguments(
          ctx.r3.u32, ctx.r4.u32, ctx.r1.u32)) return;
  // Actual CPU-only TU23 conversion, surface/tile clipping and RB_VPORT
  // shadows. Its scissor call takes the native hook, without Xbox packets.
  __imp__sub_83FBA978(ctx, base);
}
REX_HOOK_RAW(sub_83FBA710) {
  auto recording = legodimensions::gpu_native::HostDevice::LockRecording();
  // Shared TU23 writer reserves 160 bytes. Its save-FPR slots are within
  // that range; API caller has already reserved its separate 112-byte frame.
  if (!legodimensions::gpu_native::NativeViewportDeviceArguments(
          ctx.r3.u32, ctx.r1.u32, 160)) return;
  auto* device = reinterpret_cast<legodimensions::gpu_native::D3DDevice*>(base + ctx.r3.u32);
  const u32 surface = device->render_targets[0] ? u32(device->render_targets[0])
                                              : u32(device->depth_stencil);
  u32 width = 0, height = 0;
  if (legodimensions::gpu_native::NativePromotedSurfaceExtent(surface, width, height)) {
    // Native draws cover the whole promoted target, rather than replaying a
    // physical Xbox tile. Let the original CPU writer clip against that extent.
    // Only this call sees the extent override; persistent tiling state and
    // guest surface dimensions remain unchanged.
    const double requested_width = ctx.f3.f64, requested_height = ctx.f4.f64;
    {
      legodimensions::gpu_native::ScopedNativeViewportExtent extent(device, width, height);
      __imp__sub_83FBA710(ctx, base);
    }
    static u32 logs = 0;
    if (logs++ < 16)
      REXLOG_INFO("Native GPU: promoted viewport surface={:08X} requested={}x{} host={}x{} effective={}x{}",
          surface, requested_width, requested_height, width, height,
          float(device->viewport.width), float(device->viewport.height));
  } else {
    __imp__sub_83FBA710(ctx, base);
  }
}
REX_HOOK(sub_83FBA068, legodimensions::gpu_native::SetScissorRectHook);
REX_HOOK(sub_83FBA968, legodimensions::gpu_native::SetScissorEnableHook);
REX_HOOK(sub_83FBCBB8, legodimensions::gpu_native::SetPredicationHook);
REX_HOOK(sub_83FAF5D8, legodimensions::gpu_native::SetupFormatHook);
