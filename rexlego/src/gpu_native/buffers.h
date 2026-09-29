// Guest-visible vertex/index buffers and their CPU mirrors.
#pragma once

#include <rex/types.h>

namespace plume {
struct RenderBuffer;
}

namespace legodimensions::gpu_native {

enum class BufferKind : u32 {
  kVertex = 6,
  kIndex = 7,
};

u32 CreateBufferResource(u32 length, u32 usage, u32 format, u32 pool,
                         BufferKind kind);
u32 LockBufferResource(u32 guest_address, u32 offset, u32 size, u32 flags,
                       BufferKind kind);

// Uploads the current guest mirror on every call. This deliberately favors
// correctness over dirty-range guesses: CPU writes made without Lock/Unlock
// and later CPU readback remain visible.
plume::RenderBuffer* ResolveBufferResource(u32 guest_address, BufferKind kind);

bool IsNativeBuffer(u32 guest_address);
u32 NativeBufferType(u32 guest_address);
u32 AddRefNativeBuffer(u32 guest_address);
u32 ReleaseNativeBuffer(u32 guest_address);
void ResetBufferResources();

}  // namespace legodimensions::gpu_native
