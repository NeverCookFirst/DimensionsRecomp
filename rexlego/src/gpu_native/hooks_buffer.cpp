#include <rex/hook.h>
#include <rex/ppc/context.h>
#include <rex/types.h>

#include "gpu_native/buffers.h"
#include "gpu_native/textures.h"

extern "C" void __imp__sub_83FC49E8(PPCContext& __restrict ctx, u8* base);

namespace legodimensions::gpu_native {
namespace {

u32 CreateVertexBufferHook(u32 length, u32 usage, u32 fvf, u32 pool) {
  return CreateBufferResource(length, usage, fvf, pool, BufferKind::kVertex);
}

u32 CreateIndexBufferHook(u32 length, u32 usage, u32 format, u32 pool) {
  return CreateBufferResource(length, usage, format, pool, BufferKind::kIndex);
}

u32 LockVertexBufferHook(u32 buffer, u32 offset, u32 size, u32 flags) {
  return LockBufferResource(buffer, offset, size, flags, BufferKind::kVertex);
}

u32 LockIndexBufferHook(u32 buffer, u32 offset, u32 size, u32 flags) {
  return LockBufferResource(buffer, offset, size, flags, BufferKind::kIndex);
}

}  // namespace
}  // namespace legodimensions::gpu_native

REX_HOOK(sub_83FC4D58, legodimensions::gpu_native::CreateVertexBufferHook);
REX_HOOK(sub_83FC4E30, legodimensions::gpu_native::CreateIndexBufferHook);
REX_HOOK(sub_83FC60D8, legodimensions::gpu_native::LockVertexBufferHook);
REX_HOOK(sub_83FC6128, legodimensions::gpu_native::LockIndexBufferHook);

REX_HOOK_RAW(sub_83FC49E8) {
  const u32 resource = ctx.r3.u32;
  const u32 type = legodimensions::gpu_native::NativeBufferType(resource);
  const u32 texture_type =
      legodimensions::gpu_native::NativeTextureType(resource);
  if (type || texture_type) {
    ctx.r3.u64 = type ? type : texture_type;
    return;
  }
  __imp__sub_83FC49E8(ctx, base);
}
