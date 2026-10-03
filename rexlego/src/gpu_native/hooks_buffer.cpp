#include "gpu_native/renderer_route.h"
#include <rex/ppc/context.h>
#include <rex/types.h>

#include <atomic>
#include <algorithm>
#include <cstring>
#include <cstdlib>
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

}  // namespace

bool GuestRangeOverlapsPoolCopy(u32 address, u32 size, PhysicalCopyRange copy) {
  PhysicalCopyRange resource;
  return PhysicalRange(address, size, resource) && CopyRangesOverlap(resource, copy);
}

bool ConsumeCpuPoolCopyDraw(u32 primitive, u32 start, u32 count) {
  return g_cpu_pool_copy && g_cpu_pool_copy->Consume(primitive, start, count);
}

void PoolCopyHook(PPCContext& ctx, u8* base) {
  auto recording = HostDevice::LockRecording();
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
  const bool gpu_owned = accessible &&
      (PoolCopyHasHostTextureSource(src) || PoolCopyHasHostTextureSource(dst));
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
  const u32 buffers = InvalidatePoolCopyBuffers(dst);
  const u32 textures = InvalidatePoolCopyTextures(dst);
  const u32 shaders = InvalidatePoolCopyShaders(dst);
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
