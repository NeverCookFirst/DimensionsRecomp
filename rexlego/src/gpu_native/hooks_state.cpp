#include <rex/hook.h>
#include <rex/types.h>

#include "gpu_native/d3d.h"
#include "gpu_native/state.h"
#include "gpu_native/textures.h"
#include "gpu_native/vertex_declarations.h"

namespace legodimensions::gpu_native {
namespace {

void SetTextureHook(D3DDevice* device, u32 sampler, u32 texture) {
  if (device && sampler < kNativeTextureSlots) {
    device->textures[sampler] = texture;
  }
  BindTexture(sampler, texture);
}

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

void SetViewportHook(D3DDevice* device, const D3DViewport9* viewport) {
  if (device && viewport) {
    device->viewport = *viewport;
  }
}

void SetScissorRectHook(D3DDevice* device, const D3DRect* rect) {
  if (device && rect) {
    device->scissor = *rect;
  }
}

}  // namespace
}  // namespace legodimensions::gpu_native

REX_HOOK(sub_83FB58A8, legodimensions::gpu_native::SetTextureHook);
REX_HOOK(sub_83FBA160, legodimensions::gpu_native::SetStreamSourceHook);
REX_HOOK(sub_83FBA308, legodimensions::gpu_native::SetIndicesHook);
REX_HOOK(sub_83FB6D50,
         legodimensions::gpu_native::CreateVertexDeclarationHook);
REX_HOOK(sub_83FB7840,
         legodimensions::gpu_native::SetVertexDeclarationHook);
REX_HOOK(sub_83FBAAA8, legodimensions::gpu_native::SetRenderTargetHook);
REX_HOOK(sub_83FBAE38, legodimensions::gpu_native::SetDepthStencilHook);
REX_HOOK(sub_83FBA398, legodimensions::gpu_native::GetRenderTargetHook);
REX_HOOK(sub_83FBA3E0, legodimensions::gpu_native::GetDepthStencilHook);
REX_HOOK(sub_83FBA978, legodimensions::gpu_native::SetViewportHook);
REX_HOOK(sub_83FBA068, legodimensions::gpu_native::SetScissorRectHook);
