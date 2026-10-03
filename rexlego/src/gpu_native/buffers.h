// Guest-visible vertex/index buffers and their CPU mirrors.
#pragma once

#include <rex/types.h>
#include "gpu_native/vertex_byte_order.h"
#include "gpu_native/pool_copy.h"

namespace plume {
struct RenderBuffer;
}

namespace legodimensions::gpu_native {

enum class BufferKind : u32 {
  kVertex = 6,
  kIndex = 7,
};

struct BufferUploadTiming {
  u64 calls = 0;
  u64 hashed_bytes = 0;
  u64 converted_bytes = 0;
  double hash_ms = 0;
};
BufferUploadTiming ConsumeBufferUploadTiming();

u32 CreateBufferResource(u32 length, u32 usage, u32 format, u32 pool,
                         BufferKind kind);
u32 LockBufferResource(u32 guest_address, u32 offset, u32 size, u32 flags,
                       BufferKind kind);

// Checks current guest contents on every call, including placement buffers.
// Changed contents get a new upload buffer retained through the frame fence,
// so later CPU writes cannot overwrite geometry in earlier recorded draws.
plume::RenderBuffer* ResolveBufferResource(u32 guest_address, BufferKind kind,
                                         const VertexByteOrder& byte_order = {});

struct BufferResourceView {
  plume::RenderBuffer* buffer = nullptr;
  u32 length = 0;
  u32 guest_format = 0;
  u32 mirror_address = 0;
  u64 content_hash = 0;
  u64 byte_order_hash = 0;
};
BufferResourceView ResolveBufferResourceView(u32 guest_address,
                                             BufferKind kind,
                                             const VertexByteOrder& byte_order = {});
// Bounded immutable uploads for the bytes fetched by a draw. Metadata reads
// revalidate borrowed headers; window contents are still hashed every use.
BufferResourceView InspectBufferResource(u32 guest_address, BufferKind kind);
BufferResourceView ResolveBufferResourceWindow(u32 guest_address, BufferKind kind,
    u32 offset, u32 length, const VertexByteOrder& byte_order = {});

bool IsNativeBuffer(u32 guest_address);
u32 NativeBufferType(u32 guest_address);
u32 AddRefNativeBuffer(u32 guest_address);
u32 ReleaseNativeBuffer(u32 guest_address);
void ResetBufferResources();
u32 InvalidatePoolCopyBuffers(PhysicalCopyRange destination);

}  // namespace legodimensions::gpu_native
