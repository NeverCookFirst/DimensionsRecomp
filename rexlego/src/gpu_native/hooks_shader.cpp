#include <rex/hook.h>
#include <rex/ppc/context.h>
#include <rex/types.h>

#include "gpu_native/d3d.h"
#include "gpu_native/buffers.h"
#include "gpu_native/shaders.h"

extern "C" void __imp__sub_83FC4CE0(PPCContext& __restrict ctx, u8* base);
extern "C" void __imp__sub_83FC59D8(PPCContext& __restrict ctx, u8* base);

namespace legodimensions::gpu_native {
namespace {

u32 CreatePixelShaderHook(mapped_u32 function) {
  return CreateShaderResource(function, ShaderStage::kPixel);
}

u32 CreateVertexShaderHook(mapped_u32 function) {
  return CreateShaderResource(function, ShaderStage::kVertex);
}

void SetPixelShaderHook(D3DDevice* device, u32 shader) {
  if (device) {
    device->pixel_shader = shader;
  }
  BindShader(ShaderStage::kPixel, shader);
}

void SetVertexShaderHook(D3DDevice* device, u32 shader) {
  if (device) {
    device->vertex_shader = shader;
  }
  BindShader(ShaderStage::kVertex, shader);
}

}  // namespace
}  // namespace legodimensions::gpu_native

REX_HOOK(sub_83FB7528, legodimensions::gpu_native::CreatePixelShaderHook);
REX_HOOK(sub_83FB7750, legodimensions::gpu_native::CreateVertexShaderHook);
REX_HOOK(sub_83FB6828, legodimensions::gpu_native::SetPixelShaderHook);
REX_HOOK(sub_83FB6A30, legodimensions::gpu_native::SetVertexShaderHook);

// Resource AddRef/Release serve all D3D resource types. Only intercept objects
// created by the native shader hooks; every other pointer retains the exact
// Sep'13 implementation.
REX_HOOK_RAW(sub_83FC4CE0) {
  const u32 resource = ctx.r3.u32;
  if (!legodimensions::gpu_native::IsNativeShader(resource)) {
    if (legodimensions::gpu_native::IsNativeBuffer(resource)) {
      ctx.r3.u64 = legodimensions::gpu_native::AddRefNativeBuffer(resource);
      return;
    }
    __imp__sub_83FC4CE0(ctx, base);
    return;
  }
  ctx.r3.u64 = legodimensions::gpu_native::AddRefNativeShader(resource);
}

REX_HOOK_RAW(sub_83FC59D8) {
  const u32 resource = ctx.r3.u32;
  if (!legodimensions::gpu_native::IsNativeShader(resource)) {
    if (legodimensions::gpu_native::IsNativeBuffer(resource)) {
      ctx.r3.u64 = legodimensions::gpu_native::ReleaseNativeBuffer(resource);
      return;
    }
    __imp__sub_83FC59D8(ctx, base);
    return;
  }
  ctx.r3.u64 = legodimensions::gpu_native::ReleaseNativeShader(resource);
}
