// Guest-visible vertex/index buffers and their CPU mirrors.
#pragma once

#include <rex/types.h>
#include <memory>
#include "gpu_native/vertex_byte_order.h"
#include "gpu_native/pool_copy.h"
#include "gpu_native/index_snapshot_cache.h"

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
  u64 watch_hits = 0;
  u64 watch_audits = 0;
  u64 watch_mismatches = 0;
  double hash_ms = 0;
};
BufferUploadTiming ConsumeBufferUploadTiming();

u32 CreateBufferResource(u32 length, u32 usage, u32 format, u32 pool,
                         BufferKind kind);
u32 LockBufferResource(u32 guest_address, u32 offset, u32 size, u32 flags,
                       BufferKind kind);

// Checks current guest contents on every call, including placement buffers.
// Opt-in buffer watches use physical page generations; unknown memory hashes.
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
struct BufferPreparationSnapshot {
  std::shared_ptr<void> owner;
  BufferResourceView metadata;
};
struct PreparedBufferVersion {
  std::shared_ptr<void> owner;
  std::shared_ptr<plume::RenderBuffer> storage;
  BufferResourceView view;
};
BufferPreparationSnapshot CaptureBufferPreparation(u32 address, BufferKind kind);
std::shared_ptr<const IndexBoundsSnapshot> CaptureIndexBounds(
    const BufferPreparationSnapshot& snapshot, u32 offset, u32 length);
PreparedBufferVersion PrepareCachedBuffer(const BufferPreparationSnapshot& snapshot,
    u32 offset, u32 length, bool window, const VertexByteOrder& order = {});
BufferResourceView ResolveBufferResourceView(u32 guest_address,
                                             BufferKind kind,
                                             const VertexByteOrder& byte_order = {});
// Bounded immutable uploads for the bytes fetched by a draw. Metadata reads
// revalidate borrowed headers; window contents are still hashed every use.
BufferResourceView InspectBufferResource(u32 guest_address, BufferKind kind);
BufferResourceView ResolveBufferResourceWindow(u32 guest_address, BufferKind kind,
    u32 offset, u32 length, const VertexByteOrder& byte_order = {});
BufferResourceView ResolveBufferIndexWindow(u32 guest_address, u32 offset, u32 length);
BufferResourceView ResolveBufferStreamWindow(u32 stream, u32 guest_address,
    u32 offset, u32 length, const VertexByteOrder& byte_order);

bool IsNativeBuffer(u32 guest_address);
u32 NativeBufferType(u32 guest_address);
u32 AddRefNativeBuffer(u32 guest_address);
u32 ReleaseNativeBuffer(u32 guest_address);
// Registry record for capture-time pinning; null when unknown.
std::shared_ptr<void> FindBufferOwner(u32 guest_address);
// False when the guest bytes changed since the last upload, so the version must be made in order.
bool BufferContentCurrent(const BufferPreparationSnapshot& snapshot);
void ResetBufferResources();
u32 InvalidatePoolCopyBuffers(PhysicalCopyRange destination);
// Capture thread: false only when no registered borrowed buffer shares a page with the range.
bool PoolCopyMayTouchBuffers(PhysicalCopyRange range);

// Moves on every buffer registry insert or erase.
u64 BufferRegistryGeneration();
// True when an armed, unchanged write watch holds the buffer's content version (null counts as true).
bool BufferInputWatched(const std::shared_ptr<void>& owner);
// True when a window copy of this buffer is the storage `bound` and holds an armed, unchanged watch.
bool BufferWindowWatched(const std::shared_ptr<void>& owner, u32 offset, u32 length,
    const plume::RenderBuffer* bound);

}  // namespace legodimensions::gpu_native
