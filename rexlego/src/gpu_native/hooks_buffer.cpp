#include "gpu_native/renderer_route.h"
#include <rex/ppc/context.h>
#include <rex/types.h>

#include <atomic>
#include <algorithm>
#include <vector>
#include <cstring>
#include <cstdlib>
#include <memory>
#include <utility>
#include <xxhash.h>

#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include "gpu_native/buffers.h"
#include "gpu_native/textures.h"
#include "gpu_native/state.h"
#include "gpu_native/device.h"
#include "gpu_native/shaders.h"
#include "gpu_native/memory_watch.h"
#include "gpu_native/slow_operation.h"
#include "gpu_native/native_worker.h"
#include "gpu_native/pool_copy.h"
#include "gpu_native/native_recording.h"
#include "gpu_native/draw_batch.h"

extern "C" void __imp__sub_83FC49E8(PPCContext& __restrict ctx, u8* base);
extern "C" void __imp__sub_82BD2868(PPCContext& __restrict ctx, u8* base);

namespace legodimensions::gpu_native {
namespace {

thread_local PoolCopyDraws* g_cpu_pool_copy = nullptr;

bool PhysicalRange(u32 address, u32 size, PhysicalCopyRange& result) {
  auto* memory = REX_KERNEL_MEMORY();
  const u32 physical = address < 0x20000000u ? address : memory->GetPhysicalAddress(address);
  if (!size || physical == UINT32_MAX || u64(physical) + size > 0x20000000ull ||
      u64(address) + size > 0x100000000ull) return false;
  // Do not cross a virtual heap/alias boundary while assuming linear mapping.
  const u32 last = address < 0x20000000u ? address + size - 1 :
      memory->GetPhysicalAddress(address + size - 1);
  if (last != physical + size - 1) return false;
  result = {physical, size};
  return true;
}

bool AccessiblePhysicalRange(PhysicalCopyRange range, bool write) {
  auto* heap = REX_KERNEL_MEMORY()->GetPhysicalHeap();
  for (u64 at = range.address; at < u64(range.address) + range.length;) {
    rex::memory::HeapAllocationInfo info{};
    if (!heap->QueryRegionInfo(u32(at), &info) ||
        !(info.state & rex::memory::kMemoryAllocationCommit) ||
        !(info.protect & rex::memory::kMemoryProtectRead) ||
        (write && !(info.protect & (rex::memory::kMemoryProtectWrite |
                                   rex::memory::kMemoryProtectWriteCombine)))) return false;
    const u64 page = heap->heap_base() +
        ((at - heap->heap_base()) / heap->page_size()) * heap->page_size();
    const u64 next = std::min(page + info.region_size,
                             u64(heap->heap_base()) + heap->heap_size());
    if (next <= at) return false;
    at = next;
  }
  return true;
}

struct PoolCopyScope {
  PoolCopyDraws draws;
  PoolCopyDraws* previous;
  explicit PoolCopyScope(u32 bytes) : draws{bytes / 64, 0}, previous(g_cpu_pool_copy) {
    g_cpu_pool_copy = &draws;
  }
  ~PoolCopyScope() { g_cpu_pool_copy = previous; }
};

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

void BeginExportHook(u32 /*device*/, u32 index, u32 resource, u32 format) {
  BeginExportBinding(index, resource, format);
  if (g_cpu_pool_copy) return;
  static std::atomic<u32> warning_count{0};
  if (warning_count.fetch_add(1, std::memory_order_relaxed) < 8) {
    REXLOG_WARN(
        "Native GPU: captured memexport begin index={} resource=0x{:08X} "
        "format={} (GPU writeback pending implementation)",
        index, resource, format);
  }
}

void EndExportHook(u32 /*device*/, u32 index, u32 resource, u32 format) {
  EndExportBinding(index, resource, format);
}

bool OrderedPoolCopyEnabled() {
  static const bool enabled = [] {
    const char* value = std::getenv("LEGO_NATIVE_ORDERED_POOL_COPY");
    return !value || value[0] != '0';
  }();
  return enabled;
}

// Pool copies between two other capture-thread tasks move their bytes in order and share one invalidation at the flush.
struct PoolCopyBatch {
  bool active = false, waited = false;
  u32 completed_copies = 0;
  std::vector<PhysicalCopyRange> textures, buffers, shaders;
};
std::atomic<u32> g_pending_pool_copies{0};
PoolCopyBatch g_pool_batch;  // capture thread only

void BeginPoolCopyBatch() {
  g_pool_batch.active = true;
}

// Checks only the pages the copy touches: committed and readable, and writable for the destination.
bool BatchAccessible(PhysicalCopyRange range, bool write) {
  auto* heap = REX_KERNEL_MEMORY()->GetPhysicalHeap();
  if (!heap->IsRangeCommittedReadable(range.address, range.length)) return false;
  return !write || heap->QueryRangeAccess(range.address, range.address + range.length - 1) ==
      rex::memory::PageAccess::kReadWrite;
}

void PoolCopyRecorded(u32 destination, u32 source, u32 bytes, PhysicalCopyRange src,
                      PhysicalCopyRange dst) {
  auto& batch = g_pool_batch;
  if (!batch.active) BeginPoolCopyBatch();
  if (!BatchAccessible(src, false) || !BatchAccessible(dst, true)) {
    static std::atomic<u32> logs{0};
    if (logs.fetch_add(1, std::memory_order_relaxed) < 32)
      REXLOG_ERROR("Native GPU: ordered pool copy skipped, inaccessible dst={:08X} src={:08X} bytes={}",
                   destination, source, bytes);
    return;
  }
  // Sources need current GPU bytes; partial destinations must retain the GPU
  // bytes outside this copy before CPU authority supersedes the host image.
  const bool destination_texture = PoolCopyMayTouchTextures(dst);
  if ((PoolCopyMayTouchTextures(src) && PoolCopyGpuSourceIndexed(src)) ||
      (destination_texture && PoolCopyGpuDestinationIndexed(dst))) {
    // An earlier copy in this batch may have superseded that GPU texture; settle it and look again.
    FlushPoolCopyBatch();
    BeginPoolCopyBatch();
    if (PoolCopyGpuSourceIndexed(src) || PoolCopyGpuDestinationIndexed(dst)) {
      // A source read must observe earlier GPU writes, just like an on-demand
      // memexport readback. The coordinator cannot synchronize a command tape:
      // finish earlier preparation, then materialize on the actual recorder.
      // The barrier also prevents later draws from uploading stale guest bytes
      // while the recorder publishes this GPU version.
      WaitAllPreparation();
      batch.waited = true;
      auto materialized = std::make_shared<std::atomic<bool>>(false);
      if (OnNativeRecorderThread()) {
        // The temporal lane is already after every earlier draw. Enqueuing a
        // recorder task and waiting here would wait for this thread itself.
        materialized->store(MaterializePoolCopyTextureDependencies(src,dst), std::memory_order_release);
      } else {
        NativeRecorderSubmit([src, dst, materialized] {
          materialized->store(MaterializePoolCopyTextureDependencies(src,dst), std::memory_order_release);
        }, false, true);
        NativeRecorderWaitExecuted(NativeRecorderBarrier());
      }
      if (!materialized->load(std::memory_order_acquire) || PoolCopyGpuSourceIndexed(src) ||
          PoolCopyGpuDestinationIndexed(dst)) {
        REXLOG_ERROR("Native GPU: ordered pool copy unsupported GPU dependency dst={:08X} src={:08X} bytes={}",
                     destination, source, bytes);
        return;
      }
      // Access may have changed while waiting for GPU completion.
      if (!BatchAccessible(src, false) || !BatchAccessible(dst, true)) return;
    }
  }
  const bool textures = destination_texture;
  const bool buffers = PoolCopyMayTouchBuffers(dst);
  const bool shaders = PoolCopyMayTouchShaders(dst);
  // Workers may still upload texture data from this destination for draws captured before the copy.
  if (textures && !batch.waited) {
    WaitAllPreparation();
    batch.waited = true;
  }
  auto* memory = REX_KERNEL_MEMORY();
  InvalidateCpuPhysicalMemory(dst.address, bytes);
  std::memmove(memory->TranslatePhysical(dst.address), memory->TranslatePhysical(src.address), bytes);
  if (textures) batch.textures.push_back(dst);
  if (buffers) batch.buffers.push_back(dst);
  if (shaders) batch.shaders.push_back(dst);
}

// Queues the copy in capture order and swallows the helper's POINTLIST copy draws before they are queued.
bool OrderedPoolCopy(PPCContext& ctx, u8* base) {
  if (!OrderedPoolCopyEnabled() || !NativeWorkerShouldQueue() || !NativeFullDrawPreparationEnabled())
    return false;
  const u32 destination = ctx.r3.u32, source = ctx.r4.u32, bytes = ctx.r5.u32;
  PhysicalCopyRange src, dst;
  if (!bytes || (bytes & 63) || !PhysicalRange(source, bytes, src) || !PhysicalRange(destination, bytes, dst))
    return false;
  g_pending_pool_copies.fetch_add(1, std::memory_order_release);
  NativeWorkerSubmitPoolCopy([=] {
    PoolCopyRecorded(destination, source, bytes, src, dst);
    // The CPU bytes alone are insufficient: keep this copy pending until
    // FlushPoolCopyBatch also invalidates every affected resource cache.
    ++g_pool_batch.completed_copies;
  });
  PoolCopyScope scope(bytes);
  __imp__sub_82BD2868(ctx, base);
  if (scope.draws.consumed != scope.draws.records)
    REXLOG_ERROR("Native GPU: pool copy POINTLIST contract mismatch consumed={} expected={}",
                 scope.draws.consumed, scope.draws.records);
  return true;
}

}  // namespace

namespace {
// Touching or overlapping ranges merge; the union is exactly what the copies wrote.
void MergeRanges(std::vector<PhysicalCopyRange>& ranges) {
  std::sort(ranges.begin(), ranges.end(),
            [](const PhysicalCopyRange& a, const PhysicalCopyRange& b) { return a.address < b.address; });
  size_t merged = 0;
  for (size_t i = 0; i < ranges.size(); ++i) {
    if (merged && u64(ranges[merged - 1].address) + ranges[merged - 1].length >= ranges[i].address) {
      const u64 end = std::max(u64(ranges[merged - 1].address) + ranges[merged - 1].length,
                               u64(ranges[i].address) + ranges[i].length);
      ranges[merged - 1].length = u32(end - ranges[merged - 1].address);
    } else {
      ranges[merged++] = ranges[i];
    }
  }
  ranges.resize(merged);
}
}  // namespace

void FlushPoolCopyBatch() {
  auto& batch = g_pool_batch;
  if (!batch.active) return;
  MergeRanges(batch.textures);
  MergeRanges(batch.buffers);
  MergeRanges(batch.shaders);
  if (!batch.textures.empty() || !batch.buffers.empty() || !batch.shaders.empty()) {
    auto recording = HostDevice::LockRecording();
    for (const auto& range : batch.buffers) InvalidatePoolCopyBuffers(range);
    for (const auto& range : batch.textures) InvalidatePoolCopyTexturesIndexed(range);
    for (const auto& range : batch.shaders) InvalidatePoolCopyShaders(range);
  }
  const u32 completed = std::exchange(batch.completed_copies, 0);
  batch.active = batch.waited = false;
  batch.textures.clear();
  batch.buffers.clear();
  batch.shaders.clear();
  if (completed) g_pending_pool_copies.fetch_sub(completed, std::memory_order_release);
}

void OrderPoolCopiesBeforeResourceRelocation(u64 caller) {
  // XGOffsetResource changes the placement header immediately on the CPU.
  // Earlier queued copies may still need its old header for GPU writeback.
  // The copy helper also relocates two private memexport buffer descriptors;
  // those are inside PoolCopyScope and must retain ordinary copy batching.
  if (caller == 0x83F9B660 && !g_cpu_pool_copy &&
      g_pending_pool_copies.load(std::memory_order_acquire))
    NativeWorkerDrainCapture();
}

namespace {
}  // namespace

bool GuestRangeOverlapsPoolCopy(u32 address, u32 size, PhysicalCopyRange copy) {
  PhysicalCopyRange resource;
  return PhysicalRange(address, size, resource) && CopyRangesOverlap(resource, copy);
}

bool ConsumeCpuPoolCopyDraw(u32 primitive, u32 start, u32 count) {
  return g_cpu_pool_copy && g_cpu_pool_copy->Consume(primitive, start, count);
}

bool PoolCopyPhysicalRange(u32 address, u32 size, PhysicalCopyRange& result) {
  return PhysicalRange(address,size,result);
}
bool PoolCopyRangeWritable(PhysicalCopyRange range) {
  return AccessiblePhysicalRange(range,true);
}
bool PoolCopyRangeReadable(PhysicalCopyRange range) {
  return AccessiblePhysicalRange(range,false);
}

void PoolCopyHook(PPCContext& ctx, u8* base) {
  if (OrderedPoolCopy(ctx, base)) return;
  // Holds the recording lock across guest helper code that may issue draws:
  // those must run inline, ahead of this hook's own CPU copy below.
  NativeInlineScope inline_scope;
  SlowNativeOperation slow("pool-copy");
  auto recording = HostDevice::LockRecording();
  slow.Mark("recording_lock_ms");
  const u32 destination = ctx.r3.u32, source = ctx.r4.u32, bytes = ctx.r5.u32;
  const u32 caller = ctx.lr == 0x82BD2B44 ?
      u32(*REX_KERNEL_MEMORY()->TranslateVirtual<const be_u32*>(ctx.r1.u32 + 152)) : u32(ctx.lr);
  PhysicalCopyRange src, dst;
  // 82BD2AA0 supplies already rounded chunks and traverses them backwards.
  // Keep the original helper's headers, constants, ring advancement, bindings
  // and final stream unbind; replace only its POINTLIST copy work.
  const bool accessible = bytes && !(bytes & 63) &&
      PhysicalRange(source, bytes, src) && PhysicalRange(destination, bytes, dst) &&
      AccessiblePhysicalRange(src, false) && AccessiblePhysicalRange(dst, true);
  // Sources and partial destinations share one readback transaction and GPU
  // wait. A whole destination overwrite needs no preservation readback.
  bool gpu_owned=accessible && !MaterializePoolCopyTextureDependencies(src,dst);
  if(accessible && !gpu_owned) gpu_owned=PoolCopyHasHostTextureSource(src);
  slow.Mark("source_validation_ms");
  if (!accessible || gpu_owned) {
    REXLOG_ERROR("Native GPU: pool copy unsupported dst={:08X} src={:08X} bytes={} "
                 "lr={:08X} accessible={} overlaps_host_resolve={}",
                 destination, source, bytes, ctx.lr, accessible, gpu_owned);
    __imp__sub_82BD2868(ctx, base);
    return;
  }
  auto* memory = REX_KERNEL_MEMORY();
  static const bool verify_copy = [] {
    char* value = nullptr;
    size_t length = 0;
    _dupenv_s(&value, &length, "LEGO_DUMP_MISSING_SHADERS");
    const bool enabled = value && length > 1;
    std::free(value);
    return enabled;
  }();
  const u64 source_hash = verify_copy ?
      XXH3_64bits(memory->TranslatePhysical(src.address), bytes) : 0;
  // Canonical physical pointers make memmove safe even across A/C/E aliases
  // whose separate host mappings would hide an overlapping physical range.
  InvalidateCpuPhysicalMemory(dst.address, bytes);
  std::memmove(memory->TranslatePhysical(dst.address),
               memory->TranslatePhysical(src.address), bytes);
  if (verify_copy && source_hash != XXH3_64bits(memory->TranslatePhysical(dst.address), bytes))
    REXLOG_ERROR("Native GPU: pool copy byte verification failed dst={:08X} src={:08X} bytes={}",
                 destination, source, bytes);
  slow.Mark("cpu_copy_ms");
  u32 buffers, textures, shaders;
  {
    // The outer timing groups all invalidation. Split it only in the opt-in
    // slow-operation trace so a travel stall is attributed to the right cache.
    SlowNativeOperation invalidation("pool-copy-invalidation");
    buffers = InvalidatePoolCopyBuffers(dst);
    invalidation.Mark("buffers_ms");
    textures = InvalidatePoolCopyTextures(dst);
    invalidation.Mark("textures_ms");
    shaders = InvalidatePoolCopyShaders(dst);
    invalidation.Mark("shaders_ms");
  }
  slow.Mark("cache_invalidation_ms");
  static std::atomic<u32> logs{0};
  if (logs.fetch_add(1, std::memory_order_relaxed) < 128)
    REXLOG_INFO("Native GPU: CPU pool copy dst={:08X} src={:08X} bytes={} "
        "physical={:08X}<-{:08X} lr={:08X} caller={:08X} VS={:016X} verify={} invalidated VB/IB={} textures={} shaders={}",
        destination, source, bytes, dst.address, src.address, ctx.lr, caller,
        BoundShaderHash(ShaderStage::kVertex), verify_copy, buffers, textures, shaders);
  PoolCopyScope scope(bytes);
  __imp__sub_82BD2868(ctx, base);
  if (scope.draws.consumed != scope.draws.records)
    REXLOG_ERROR("Native GPU: pool copy POINTLIST contract mismatch consumed={} expected={}",
                 scope.draws.consumed, scope.draws.records);
}
}  // namespace legodimensions::gpu_native

REX_HOOK_RAW(sub_82BD2868) {
  legodimensions::gpu_native::PoolCopyHook(ctx, base);
}

REX_HOOK(sub_83FC4D58, legodimensions::gpu_native::CreateVertexBufferHook);
REX_HOOK(sub_83FC4E30, legodimensions::gpu_native::CreateIndexBufferHook);
REX_HOOK(sub_83FC60D8, legodimensions::gpu_native::LockVertexBufferHook);
REX_HOOK(sub_83FC6128, legodimensions::gpu_native::LockIndexBufferHook);
REX_HOOK(sub_83FC53D0, legodimensions::gpu_native::BeginExportHook);
REX_HOOK(sub_83FC54C8, legodimensions::gpu_native::EndExportHook);

REX_HOOK_RAW(sub_83FC49E8) {
  legodimensions::gpu_native::OrderPoolCopiesBeforeResourceRelocation(ctx.lr);
  const u32 resource = ctx.r3.u32;
  const u32 type = legodimensions::gpu_native::NativeBufferType(resource);
  const u32 texture_type =
      legodimensions::gpu_native::NativeTextureType(resource);
  if (type || texture_type) {
    ctx.r3.u64 = type ? type : texture_type;
    return;
  }
  __imp__sub_83FC49E8(ctx, base);
  // This caller controls which header field XGOffsetResource relocates.
  if (ctx.lr == 0x83F9B660 && legodimensions::gpu_native::IsNativeBuffer(resource))
    REXLOG_INFO("Native GPU: placement buffer relocation type resource={:08X} type={} lr={:08X}",
                resource, ctx.r3.u32, ctx.lr);
}
