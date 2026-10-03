#include "gpu_native/renderer_route.h"
#include <rex/ppc/context.h>
#include <rex/types.h>

#include "gpu_native/textures.h"

extern "C" void __imp__sub_83FB5540(PPCContext& __restrict ctx, u8* base);
extern "C" void __imp__sub_83FB5560(PPCContext& __restrict ctx, u8* base);
extern "C" void __imp__sub_83FB57C8(PPCContext& __restrict ctx, u8* base);

namespace legodimensions::gpu_native {
namespace {

u32 CreateTextureHook(u32 width, u32 height, u32 depth, u32 levels, u32 usage,
                      u32 format, u32 pool, u32 type) {
  return CreateTextureResource(width, height, depth, levels, usage, format,
                               pool, type);
}

u32 CreateSurfaceHook(u32 width, u32 height, u32 format, u32 multi_sample,
                      u32 parameters) {
  return CreateSurfaceResource(width, height, format, multi_sample, parameters);
}

}  // namespace
}  // namespace legodimensions::gpu_native

REX_HOOK(sub_83FB5570, legodimensions::gpu_native::CreateTextureHook);
REX_HOOK(sub_83FB5690, legodimensions::gpu_native::CreateSurfaceHook);

REX_HOOK_RAW(sub_83FB5540) {
  if (legodimensions::gpu_native::LockTextureResource(
          ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32)) {
    return;
  }
  // A malformed request for one of our zero-fetch-constant textures must not
  // enter the SDK body: it decodes that absent constant and can divide by zero.
  if (legodimensions::gpu_native::IsNativeTexture(ctx.r3.u32)) {
    return;
  }
  __imp__sub_83FB5540(ctx, base);
}

REX_HOOK_RAW(sub_83FB5560) {
  if (legodimensions::gpu_native::DescribeTextureResource(
          ctx.r3.u32, ctx.r4.u32, ctx.r5.u32)) {
    return;
  }
  if (legodimensions::gpu_native::IsNativeTexture(ctx.r3.u32)) {
    return;
  }
  __imp__sub_83FB5560(ctx, base);
}

REX_HOOK_RAW(sub_83FB57C8) {
  if (legodimensions::gpu_native::DescribeSurfaceResource(ctx.r3.u32,
                                                          ctx.r4.u32)) {
    return;
  }
  if (legodimensions::gpu_native::IsNativeTexture(ctx.r3.u32)) {
    return;
  }
  __imp__sub_83FB57C8(ctx, base);
}
