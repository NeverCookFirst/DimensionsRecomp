#include "gpu_native/buffers.h"
#include "gpu_native/footprint_index.h"
#include "gpu_native/native_worker.h"
#include "gpu_native/native_recording.h"
#include "gpu_native/draw_batch.h"
#include <unordered_set>
#include "gpu_native/resource_registry.h"
#include "gpu_native/geometry_upload.h"
#include "gpu_native/geometry_content_cache.h"
#include <plume_d3d12.h>
#include "gpu_native/buffer_header.h"
#include "gpu_native/buffer_alloc_diagnostic.h"
#include "gpu_native/long_probe.h"
#include "gpu_native/memory_watch.h"
#include "gpu_native/vertex_upload.h"
#include "gpu_native/upload_buffer_pool.h"

#include <cstring>
#include <cstdlib>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <list>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>
#include <immintrin.h>

#include <plume_render_interface.h>
// Use the game's configured ISA for bulk hashing (AVX2 on x86-64-v3),
// retaining XXH3's exact hash and every-use content validation.
#define XXH_INLINE_ALL
#include <xxhash.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include "gpu_native/d3d.h"
#include "gpu_native/device.h"
#include "gpu_native/textures.h"

namespace legodimensions::gpu_native {
namespace {

struct BufferResource {
  u32 guest_address = 0;
  u32 mirror_address = 0;
  u32 length = 0;
  u32 format = 0;
  BufferKind kind = BufferKind::kVertex;
  bool owns_guest_memory = true;
  // Frame+1 of the last content re-upload; a recent one marks the buffer volatile.
  u32 recreate_frame = 0;
  // Set once another record owns this address; pinned lookups then fall through.
  std::atomic<bool> replaced{false};
  std::mutex upload_mutex;
  std::shared_ptr<plume::RenderBuffer> buffer;
  u64 content_hash = 0;
  u64 byte_order_hash = 0;
  CpuMemoryStamp cpu_stamp;
  IndexSnapshotCache index_snapshots;
  // Immutable conversions of the SAME contents for alternating declarations.
  std::unordered_map<u64, std::shared_ptr<plume::RenderBuffer>> variants;
  std::unordered_map<u64, std::shared_ptr<BufferResource>> windows;
  // Window keys, most recently used first. Each window keeps its position so
  // a hit and the eviction of the least recently used range are both O(1);
  // a shared mesh pool can hold thousands of ranges.
  std::list<u64> window_lru;
  std::list<u64>::iterator window_lru_position;
  u64 window_cache_bytes = 0;
  u32 window_offset = 0;  // Diagnostic origin within the parent buffer.
};

UploadBufferPool<plume::RenderBuffer> g_upload_pool;
UploadBufferPool<plume::RenderBuffer> g_resident_pool;
GeometryContentCache<plume::RenderBuffer> g_geometry_content_cache;
std::mutex g_buffers_mutex;
ResourceRegistry<BufferResource> g_buffers;
BufferUploadTiming g_buffer_timing;
std::mutex g_buffer_timing_mutex;

D3DBuffer* GuestBuffer(u32 guest_address) {
  auto* memory = REX_KERNEL_MEMORY();
  return memory->TranslateVirtual<D3DBuffer*>(guest_address);
}

std::shared_ptr<BufferResource> FindBuffer(u32 guest_address) {
  if (auto pinned = FindPinnedOwner(kPinBuffer, guest_address)) {
    auto record = std::static_pointer_cast<BufferResource>(std::move(pinned));
    if (!record->replaced.load(std::memory_order_acquire)) return record;
  }
  return g_buffers.Lookup(guest_address, g_buffers_mutex);
}

std::shared_ptr<BufferResource> AdoptBuffer(u32 address, BufferKind kind) {
  if (!address || !HostDevice::IsReady()) return nullptr;
  auto existing = FindBuffer(address);
  if (existing && existing->owns_guest_memory) return existing;
  const auto* header = GuestBuffer(address);
  const auto info = DecodePlacementBuffer(header->resource.common,
      header->fetch_lo, header->fetch_hi, kind == BufferKind::kIndex);
  if (!info.address) return nullptr;
  if (existing && existing->kind == kind && existing->mirror_address == info.address &&
      existing->length == info.size && existing->format == info.index_format)
    return existing;
  auto resource = std::make_shared<BufferResource>();
  resource->guest_address = address;
  resource->mirror_address = info.address;
  resource->length = info.size;
  resource->format = info.index_format;
  resource->kind = kind;
  resource->owns_guest_memory = false;
  {
    std::lock_guard lock(g_buffers_mutex);
    auto& entry = g_buffers[address];
    if (existing) existing->replaced.store(true, std::memory_order_release);
    if (entry != existing) return entry;  // Another caller adopted/replaced it.
    entry = resource;
  }
  if (existing) HostDevice::RetireResource(std::move(existing));
  static std::atomic<u32> adopted_logs{0};
  if (adopted_logs.fetch_add(1) < 32)
    REXLOG_INFO("Native GPU: adopted {} buffer 0x{:08X} data=0x{:08X} bytes={} index_format={}",
        kind == BufferKind::kIndex ? "index" : "vertex", address,
        info.address, info.size, info.index_format);
  return resource;
}

void TraceBufferAllocation(const BufferResource& resource, const VertexByteOrder& order,
    u64 old_hash, u64 new_hash, u64 conversion_hash, size_t previous_variants,
    bool evicted) {
  if (!LongProbeEnabled()) return;
  static BufferAllocationDiagnostic diagnostic = [] {
    const char* guest = std::getenv("LEGO_NATIVE_BUFFER_ALLOC_GUEST");
    const char* trigger = std::getenv("LEGO_NATIVE_BUFFER_ALLOC_TRIGGER");
    return BufferAllocationDiagnostic(guest ? BufferAllocationDiagnostic::ParseGuest(guest) :
        std::nullopt, trigger ? trigger : "");
  }();
  if (!diagnostic.Select(g_probe_frame.load(), resource.guest_address)) return;
  LongProbeEvent("buffer_allocation", false, "guest=", resource.guest_address,
      "mirror=", resource.mirror_address, "kind=", u32(resource.kind), "bytes=", resource.length,
      "window_offset=", resource.window_offset, "stride=", order.stride,
      "phase=", order.stream_offset, "reversal_hash=", conversion_hash,
      "old_content=", old_hash, "new_content=", new_hash,
      "reason=", old_hash != new_hash ? "content_changed" : "variant_miss",
      "previous_variants=", previous_variants, "evicted=", evicted);
}

void ByteSwapElements(void* destination, const void* source, size_t size,
                      u32 element_size) {
  auto* dst = static_cast<u8*>(destination);
  const auto* src = static_cast<const u8*>(source);
  size_t offset = 0;
  if (element_size == 2) {
    for (; offset + 2 <= size; offset += 2) {
      dst[offset + 0] = src[offset + 1];
      dst[offset + 1] = src[offset + 0];
    }
  } else {
    for (; offset + 4 <= size; offset += 4) {
      dst[offset + 0] = src[offset + 3];
      dst[offset + 1] = src[offset + 2];
      dst[offset + 2] = src[offset + 1];
      dst[offset + 3] = src[offset + 0];
    }
  }
  if (offset != size) {
    std::memcpy(dst + offset, src + offset, size - offset);
  }
}

}  // namespace

BufferUploadTiming ConsumeBufferUploadTiming() {
  auto recording = HostDevice::LockRecording();
  std::lock_guard lock(g_buffer_timing_mutex);
  return std::exchange(g_buffer_timing, {});
}

bool PoolCopyMayTouchBuffers(PhysicalCopyRange range) {
  static FootprintIndex index;
  // New buffers are marked as they appear; erased ones leave stale bits (extra scans only), cleared by
  // a full rebuild every 4096 additions.
  static bool built = false;
  static u32 since_rebuild = 0;
  const u64 generation = g_buffers.Insertions();
  if (!built || !index.Current(generation, 0)) {
    // Only reads registries: the registry half suffices and never waits on the recorder.
    struct RegistryOnly {
      RegistryOnly() { NativeRecordingGate::EnterPreparation(); }
      ~RegistryOnly() { NativeRecordingGate::LeavePreparation(); }
    } registry_only;
    auto recording = HostDevice::LockRecording();
    std::lock_guard lock(g_buffers_mutex);
    std::vector<u32> added;
    const bool complete = g_buffers.TakeInsertedKeys(added);
    const auto mark = [&](const std::shared_ptr<BufferResource>& resource) {
      if (resource->owns_guest_memory) return;
      PhysicalCopyRange physical;
      if (PoolCopyPhysicalRange(resource->mirror_address, resource->length, physical)) index.pages.Mark(physical);
      if (PoolCopyPhysicalRange(resource->guest_address, sizeof(D3DBuffer), physical)) index.pages.Mark(physical);
    };
    since_rebuild += u32(added.size());
    if (!built || !complete || since_rebuild > 4096) {
      index.pages.Clear();
      for (const auto& [address, resource] : g_buffers) mark(resource);
      built = true;
      since_rebuild = 0;
    } else {
      for (const u32 address : added) {
        const auto found = g_buffers.find(address);
        if (found != g_buffers.end()) mark(found->second);
      }
    }
    index.key[0] = g_buffers.Insertions();
    index.key[1] = 0;
  }
  return index.pages.Any(range);
}

u32 InvalidatePoolCopyBuffers(PhysicalCopyRange destination) {
  auto recording = HostDevice::LockRecording();
  std::vector<std::shared_ptr<BufferResource>> retired;
  {
    std::lock_guard lock(g_buffers_mutex);
    for (auto it = g_buffers.begin(); it != g_buffers.end();) {
      const auto& resource = it->second;
      if (!resource->owns_guest_memory &&
          (GuestRangeOverlapsPoolCopy(resource->mirror_address, resource->length, destination) ||
           GuestRangeOverlapsPoolCopy(resource->guest_address, sizeof(D3DBuffer), destination))) {
        static std::atomic<u32> logs{0};
        if (logs.fetch_add(1, std::memory_order_relaxed) < 32)
          REXLOG_INFO("Native GPU: pool copy invalidates {} buffer {:08X} data={:08X} bytes={}",
              resource->kind == BufferKind::kIndex ? "index" : "vertex",
              resource->guest_address, resource->mirror_address, resource->length);
        retired.push_back(std::move(it->second));
        it = g_buffers.erase(it);
      } else ++it;
    }
  }
  const u32 count = u32(retired.size());
  for (auto& resource : retired) HostDevice::RetireResource(std::move(resource));
  return count;
}

u32 CreateBufferResource(u32 length, u32 /*usage*/, u32 format, u32 /*pool*/,
                         BufferKind kind) {
  if (!length || !HostDevice::IsReady()) {
    return 0;
  }
  auto* memory = REX_KERNEL_MEMORY();
  const u32 guest_address = memory->SystemHeapAlloc(sizeof(D3DBuffer), 0x10);
  if (!guest_address) {
    return 0;
  }
  const u32 mirror_address = memory->SystemHeapAlloc(length, 0x20);
  if (!mirror_address) {
    memory->SystemHeapFree(guest_address);
    return 0;
  }
  memory->Zero(guest_address, sizeof(D3DBuffer));
  memory->Zero(mirror_address, length);

  auto* guest = GuestBuffer(guest_address);
  guest->resource.common = static_cast<u32>(kind);
  guest->resource.reference_count = 1;
  guest->fetch_lo = mirror_address;
  guest->fetch_hi = length;

  auto resource = std::make_shared<BufferResource>();
  resource->guest_address = guest_address;
  resource->mirror_address = mirror_address;
  resource->length = length;
  resource->format = format;
  resource->kind = kind;
  {
    std::lock_guard lock(g_buffers_mutex);
    g_buffers.emplace(guest_address, std::move(resource));
  }
  return guest_address;
}

u32 LockBufferResource(u32 guest_address, u32 offset, u32 size, u32 /*flags*/,
                       BufferKind kind) {
  const auto resource = AdoptBuffer(guest_address, kind);
  if (!resource || resource->kind != kind || offset > resource->length ||
      (size != 0 && size > resource->length - offset)) {
    return 0;
  }
  return resource->mirror_address + offset;
}

std::shared_ptr<plume::RenderBuffer> CreateImmutableUploadBuffer(u32 length, BufferKind kind) {
  auto* device=HostDevice::Device();
  if (!device) return {};
  const auto create=[&](u64 capacity) {
    const auto desc=kind==BufferKind::kIndex
        ? plume::RenderBufferDesc::IndexBuffer(capacity,plume::RenderHeapType::UPLOAD)
        : plume::RenderBufferDesc::VertexBuffer(capacity,plume::RenderHeapType::UPLOAD);
    auto buffer=device->createBuffer(desc);
    static const bool named=std::getenv("LEGO_NATIVE_GPU_DEBUG")!=nullptr;
    if (buffer && named) buffer->setName(kind==BufferKind::kIndex ? "LEGO guest index buffer" : "LEGO guest vertex buffer");
    return buffer;
  };
  static const bool enabled=[] {
    const char* value=std::getenv("LEGO_NATIVE_UPLOAD_POOL");
    return !value || std::strcmp(value,"0")!=0;
  }();
  if (!enabled) return create(length);
  auto buffer=g_upload_pool.Acquire(length,uint8_t(kind),create);
  static std::atomic<u64> calls{0};
  if (++calls%4096==0) {
    const auto stats=g_upload_pool.Stats();
    REXLOG_INFO("Native immutable upload pool: hits={} misses={} idle_bytes={} idle_buffers={}",
        stats.hits,stats.misses,stats.bytes,stats.count);
  }
  return buffer;
}

std::shared_ptr<GeometryPublication> PromoteImmutableGeometry(
    std::shared_ptr<plume::RenderBuffer>& buffer, BufferKind kind, u32 length,
    bool track_publication = false) {
  // Quad expansion consumes the immutable index upload on the CPU.
  if (kind == BufferKind::kIndex) return {};
  static const bool enabled = [] {
    const char* value = std::getenv("LEGO_NATIVE_LOCAL_GEOMETRY");
    const bool selected = (!value || value[0] != '0') &&
        !std::getenv("LEGO_NATIVE_MESH_TRACE") && !std::getenv("LEGO_NATIVE_COLOR_TRACE");
    REXLOG_INFO("Native vertex storage: {}", selected ? "device local" : "upload heap");
    return selected;
  }();
  if (!enabled) return {};
  const auto draw = HostDevice::CurrentDrawDeviceView();
  if (!draw.device || !draw.commands || !length) return {};
  const bool index = kind == BufferKind::kIndex;
  auto resident = g_resident_pool.Acquire(length, uint8_t(kind) | 0x80, [&](u64 capacity) {
    const auto desc = index
        ? plume::RenderBufferDesc::IndexBuffer(capacity, plume::RenderHeapType::DEFAULT)
        : plume::RenderBufferDesc::VertexBuffer(capacity, plume::RenderHeapType::DEFAULT);
    auto created = draw.device->createBuffer(desc);
    static const bool named = std::getenv("LEGO_NATIVE_GPU_DEBUG") != nullptr;
    if (created && named) created->setName(index ? "LEGO resident index buffer" : "LEGO resident vertex buffer");
    return created;
  });
  if (!resident || !static_cast<plume::D3D12Buffer*>(resident.get())->d3d) return {};
  std::shared_ptr<GeometryPublication> publication;
  draw.commands->barriers(plume::RenderBarrierStage::COPY,
      plume::RenderBufferBarrier(resident.get(), plume::RenderBufferAccess::WRITE));
  draw.commands->copyBufferRegion(resident->at(0), buffer->at(0), length);
  draw.commands->barriers(plume::RenderBarrierStage::GRAPHICS,
      plume::RenderBufferBarrier(resident.get(), plume::RenderBufferAccess::READ));
  if(track_publication) {
    publication=std::make_shared<GeometryPublication>();
    RecordNativeCommand(draw.commands,[publication](auto*) {
      publication->recorded.store(true,std::memory_order_release);
    });
    HostDevice::RetireResource(std::make_shared<GeometryFenceCompletion>(publication));
  }
  HostDevice::RetireResource(std::move(buffer));
  buffer = std::move(resident);
  return publication;
}

plume::RenderBuffer* ResolveBufferContents(const std::shared_ptr<BufferResource>& resource,
    u32 guest_address, BufferKind kind, const VertexByteOrder& byte_order, BufferResourceView* prepared = nullptr) {
  if (!resource || resource->kind != kind) {
    static std::atomic<u32> missing_logs{0};
    if (guest_address && missing_logs.fetch_add(1) < 8) {
      const auto* words = REX_KERNEL_MEMORY()->TranslateVirtual<const be_u32*>(guest_address);
      REXLOG_WARN("Native GPU: missing {} buffer 0x{:08X} header={:08X} {:08X} {:08X} {:08X} {:08X} {:08X} {:08X} {:08X}",
          static_cast<u32>(kind), guest_address, u32(words[0]), u32(words[1]), u32(words[2]),
          u32(words[3]), u32(words[4]), u32(words[5]), u32(words[6]), u32(words[7]));
    }
    return nullptr;
  }
  std::lock_guard upload_lock(resource->upload_mutex);
  if (!byte_order.reversed_elements.empty() &&
      (byte_order.stream_offset > resource->length || byte_order.stride < 4)) return nullptr;
  const auto conversion_order = CanonicalVertexByteOrder(byte_order);
  const u64 byte_order_hash = conversion_order.reversed_elements.empty() ? 0 :
      XXH3_64bits_withSeed(conversion_order.reversed_elements.data(),
          conversion_order.reversed_elements.size_bytes(),
          (u64(conversion_order.stream_offset) << 32) | conversion_order.stride);
  const auto* source =
      REX_KERNEL_MEMORY()->TranslateVirtual<const u8*>(resource->mirror_address);
  const bool timing = NativeTextureTimingEnabled();
  std::unique_lock timing_lock(g_buffer_timing_mutex, std::defer_lock);
  if (timing) timing_lock.lock();
  const auto hash_start = timing ? std::chrono::steady_clock::now()
                                : std::chrono::steady_clock::time_point{};
  const bool watch_enabled = CpuBufferMemoryWatchEnabled();
  // A buffer rewritten in the last frames is never clean next use; arming its watch only costs page protection.
  const bool volatile_watch = resource->recreate_frame &&
      g_probe_frame.load(std::memory_order_relaxed) + 1 - resource->recreate_frame <= 8;
  const bool watch_hit = watch_enabled && !volatile_watch && !resource->variants.empty() &&
      CpuMemoryUnchanged(resource->cpu_stamp);
  static const bool audit_watch = std::getenv("LEGO_NATIVE_AUDIT_BUFFER_WATCH") != nullptr;
  CpuMemoryStamp next_stamp;
  u64 content_hash = resource->content_hash;
  if (!watch_hit || audit_watch) {
    // Arm before hashing/conversion. A concurrent write invalidates this
    // stamp, so a mixed snapshot cannot remain cached as permanently clean.
    if (watch_enabled && !volatile_watch) {
      const CpuMemorySpan span{resource->mirror_address, resource->length};
      next_stamp = WatchCpuMemory({&span, 1});
    }
    content_hash = XXH3_64bits(source, resource->length);
    if (timing) g_buffer_timing.hashed_bytes += resource->length;
    if (watch_hit && audit_watch) {
      if (timing) ++g_buffer_timing.watch_audits;
      if (content_hash != resource->content_hash) {
        if (timing) ++g_buffer_timing.watch_mismatches;
        REXLOG_ERROR("Native buffer watch: stale clean stamp guest={:08X} data={:08X} bytes={}",
            guest_address, resource->mirror_address, resource->length);
      }
    }
  } else if (timing) ++g_buffer_timing.watch_hits;
  if (timing) {
    ++g_buffer_timing.calls;
    g_buffer_timing.hash_ms += std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - hash_start).count();
  }
  const u64 previous_content_hash = resource->content_hash;
  const size_t previous_variants = resource->variants.size();
  if (resource->content_hash != content_hash) {
    for (auto& [key, version] : resource->variants) HostDevice::RetireResource(std::move(version));
    resource->variants.clear();
    resource->buffer.reset();
  } else if (const auto it = resource->variants.find(byte_order_hash); it != resource->variants.end()) {
    if (!watch_hit || audit_watch) resource->cpu_stamp = std::move(next_stamp);
    resource->buffer = it->second;
    if (prepared) *prepared = {it->second.get(), resource->length, resource->format,
        resource->mirror_address, content_hash, byte_order_hash};
    return it->second.get();
  }
  const u32 frame = g_probe_frame.load(std::memory_order_relaxed) + 1;
  const bool volatile_content = resource->recreate_frame && frame - resource->recreate_frame <= 8;
  static const bool reuse_enabled=[] {
    const char* value=std::getenv("LEGO_NATIVE_GEOMETRY_REUSE");
    const bool enabled=!value || value[0]!='0';
    REXLOG_INFO("Native immutable geometry content reuse: {}",enabled);
    return enabled;
  }();
  const auto& reuse_stamp=watch_hit && !audit_watch ? resource->cpu_stamp : next_stamp;
  const GeometryContentKey reuse_key{content_hash,byte_order_hash,resource->length,u32(kind),
      resource->format,conversion_order.reversed_elements.empty()?0:conversion_order.stride,
      conversion_order.reversed_elements.empty()?0:conversion_order.stream_offset};
  std::vector<u8> reuse_bytes;
  bool reusable=reuse_enabled && !volatile_content && !LongProbeEnabled() &&
      GeometryContentCache<plume::RenderBuffer>::Eligible(resource->length,conversion_order.reversed_elements.size()) &&
      CpuMemoryUnchanged(reuse_stamp);
  if(reusable) {
    reuse_bytes.assign(source,source+resource->length);
    reusable=CpuMemoryUnchanged(reuse_stamp);
  }
  std::shared_ptr<plume::RenderBuffer> next_buffer;
  if(reusable) {
    next_buffer=g_geometry_content_cache.Find(reuse_key,reuse_bytes,conversion_order.reversed_elements);
    // A write during lookup invalidates this reuse just like a write during
    // hashing. It must not publish a permanently clean, mismatched version.
    if(!CpuMemoryUnchanged(reuse_stamp)) {next_buffer.reset();reusable=false;}
  }
  const bool content_reused=bool(next_buffer);
  if(content_reused) {
    static std::atomic<u64> reuse_logs{0};
    const u64 count=reuse_logs.fetch_add(1,std::memory_order_relaxed)+1;
    if((count&(count-1))==0) {
      const auto stats=g_geometry_content_cache.Stats();
      REXLOG_INFO("Native geometry content cache: hits={} misses={} pending={} cpu_bytes={} entries={}",
          stats.hits,stats.misses,stats.pending,stats.bytes,stats.entries);
    }
  }
  if(!next_buffer) {
    // Never overwrite an UPLOAD buffer referenced by already recorded draws.
    // CPU changes get a new version; the previous one survives its frame fence.
    next_buffer = CreateImmutableUploadBuffer(resource->length,kind);
    if (!next_buffer) {
      REXLOG_ERROR("Native GPU: failed to allocate {} buffer ({} bytes)",
                   kind == BufferKind::kIndex ? "index" : "vertex",
                   resource->length);
      return nullptr;
    }

    void* mapped = next_buffer->map();
    if (!mapped) {
      return nullptr;
    }
    const u32 element_size =
        kind == BufferKind::kIndex && resource->format == 1 ? 2 : 4;
    const u8* conversion_source=reusable?reuse_bytes.data():source;
    // UPLOAD heaps can be write-combined. Read/modify/write there (the packed
    // vertex swizzle below) is extremely slow. Transform in cacheable CPU RAM,
    // then perform a sequential write-only copy into the new GPU version.
    std::vector<u8> converted;
    void* conversion_target = mapped;
    const bool fused_upload = kind == BufferKind::kVertex &&
        WriteAlignedVertexUpload(static_cast<u8*>(mapped), conversion_source, resource->length, conversion_order);
    if (!fused_upload) {
      if (kind == BufferKind::kVertex && !byte_order.reversed_elements.empty()) {
        converted.resize(resource->length);
        conversion_target = converted.data();
      }
      ByteSwapElements(conversion_target, conversion_source, resource->length, element_size);
    }
    if (timing) g_buffer_timing.converted_bytes += resource->length;
    if (!fused_upload && kind == BufferKind::kVertex &&
        !ApplyVertexByteOrder(static_cast<u8*>(conversion_target), resource->length, conversion_order)) {
      next_buffer->unmap();
      REXLOG_WARN("Native GPU: rejected invalid packed vertex layout stride={}",
                  byte_order.stride);
      return nullptr;
    }
    if (!converted.empty()) std::memcpy(mapped, converted.data(), converted.size());
    next_buffer->unmap();
    // UPLOAD memory can be write-combined. Flush stores before another CPU
    // preparation job may consume this immutable allocation through the cache.
    _mm_sfence();
    // A buffer rewritten within the last frames is read once; the device copy would only add work.
    std::shared_ptr<GeometryPublication> publication;
    if (!volatile_content) publication=PromoteImmutableGeometry(next_buffer, kind, resource->length, reusable);
    if(reusable && CpuMemoryUnchanged(reuse_stamp)) {
      if(!publication) {
        publication=std::make_shared<GeometryPublication>();
        publication->recorded.store(true,std::memory_order_relaxed);
        publication->ready.store(true,std::memory_order_release);
      }
      g_geometry_content_cache.Remember(reuse_key,std::move(reuse_bytes),
          conversion_order.reversed_elements,next_buffer,std::move(publication));
    }
  }
  resource->recreate_frame = frame;
  const bool variant_evicted = resource->variants.size() >= 8;
  if (resource->variants.size() >= 8) {
    auto old = resource->variants.begin();
    HostDevice::RetireResource(std::move(old->second));
    resource->variants.erase(old);
  }
  resource->variants.emplace(byte_order_hash, next_buffer);
  resource->buffer = std::move(next_buffer);
  resource->content_hash = content_hash;
  resource->byte_order_hash = byte_order_hash;
  if (!watch_hit || audit_watch) resource->cpu_stamp = std::move(next_stamp);
  if(!content_reused) TraceBufferAllocation(*resource, conversion_order, previous_content_hash,
      content_hash, byte_order_hash, previous_variants, variant_evicted);
  if (prepared) *prepared = {resource->buffer.get(), resource->length, resource->format,
      resource->mirror_address, content_hash, byte_order_hash};
  return resource->buffer.get();
}

plume::RenderBuffer* ResolveBufferResource(u32 guest_address, BufferKind kind,
                                         const VertexByteOrder& byte_order) {
  return ResolveBufferContents(AdoptBuffer(guest_address, kind), guest_address, kind, byte_order);
}

BufferPreparationSnapshot CaptureBufferPreparation(u32 address, BufferKind kind) {
  auto resource = AdoptBuffer(address, kind);
  if (!resource || resource->kind != kind) return {};
  return {resource, {nullptr, resource->length, resource->format, resource->mirror_address}};
}

std::shared_ptr<const IndexBoundsSnapshot> CaptureIndexBounds(
    const BufferPreparationSnapshot& snapshot, u32 offset, u32 length) {
  const auto resource = std::static_pointer_cast<BufferResource>(snapshot.owner);
  if (!resource || resource->kind != BufferKind::kIndex || !length || length > (4u << 20) ||
      offset > resource->length || length > resource->length - offset) return {};
  static const bool reuse = [] {
    const char* value = std::getenv("LEGO_NATIVE_INDEX_SNAPSHOT_CACHE");
    return !value || value[0] != '0';
  }();
  std::lock_guard lock(resource->upload_mutex);
  return resource->index_snapshots.Get(offset, length, reuse && CpuBufferMemoryWatchEnabled(), [&] {
    auto result = std::make_shared<IndexBoundsSnapshot>();
    const CpuMemorySpan span{resource->mirror_address + offset, length};
    result->stamp = WatchCpuMemory({&span, 1});
    if (result->stamp.pages.empty()) return std::shared_ptr<IndexBoundsSnapshot>{};
    const auto* bytes = REX_KERNEL_MEMORY()->TranslateVirtual<const u8*>(span.address);
    result->bounds = ScanGuestIndexBounds({bytes, length}, resource->format == 1 ? 2 : 4);
    return result;
  });
}

PreparedBufferVersion PrepareCachedBuffer(const BufferPreparationSnapshot& snapshot,
    u32 offset, u32 length, bool window, const VertexByteOrder& byte_order) {
  auto resource = std::static_pointer_cast<BufferResource>(snapshot.owner);
  static const bool audit = std::getenv("LEGO_NATIVE_AUDIT_BUFFER_WATCH") != nullptr;
  if (!resource || !CpuBufferMemoryWatchEnabled() || LongProbeEnabled() ||
      audit) return {};
  if (window) {
    std::lock_guard lock(resource->upload_mutex);
    if (!length || offset > resource->length || length > resource->length - offset) return {};
    const auto found = resource->windows.find((u64(offset) << 32) | length);
    if (found == resource->windows.end()) return {};
    resource = found->second;
  }
  std::lock_guard lock(resource->upload_mutex);
  if (!CpuMemoryUnchanged(resource->cpu_stamp)) return {};
  const auto order = CanonicalVertexByteOrder(byte_order);
  const u64 hash = order.reversed_elements.empty() ? 0 :
      XXH3_64bits_withSeed(order.reversed_elements.data(), order.reversed_elements.size_bytes(),
          (u64(order.stream_offset) << 32) | order.stride);
  const auto found = resource->variants.find(hash);
  if (found == resource->variants.end()) return {};
  return {resource, found->second, {found->second.get(), resource->length, resource->format,
      resource->mirror_address, resource->content_hash, hash}};
}

BufferResourceView InspectBufferResource(u32 guest_address, BufferKind kind) {
  const auto resource = AdoptBuffer(guest_address, kind);
  if (!resource || resource->kind != kind) return {};
  return {nullptr, resource->length, resource->format, resource->mirror_address};
}

BufferResourceView ResolveBufferResourceWindow(u32 guest_address, BufferKind kind,
    u32 offset, u32 length, const VertexByteOrder& byte_order) {
  const auto parent = AdoptBuffer(guest_address, kind);
  if (!parent || parent->kind != kind || !length || offset > parent->length ||
      length > parent->length - offset) return {};
  // Keep DWORD endian conversion aligned with the original whole buffer.
  const u32 alignment = kind == BufferKind::kIndex && parent->format == 1 ? 2 : 4;
  if (offset % alignment || length % alignment) return {};
  const u64 key = (u64(offset) << 32) | length;
  std::shared_ptr<BufferResource> resource;
  {
    std::lock_guard lock(parent->upload_mutex);
    auto it = parent->windows.find(key);
    if (it != parent->windows.end()) {
      resource = it->second;
      parent->window_lru.splice(parent->window_lru.begin(), parent->window_lru,
                                resource->window_lru_position);
    } else {
      resource = std::make_shared<BufferResource>();
      resource->guest_address = guest_address;
      resource->mirror_address = parent->mirror_address + offset;
      resource->length = length;
      resource->format = parent->format;
      resource->kind = kind;
      resource->owns_guest_memory = false;
      resource->window_offset = offset;
      // A shared mesh pool has far more than 64 draw ranges. Evicting begin()
      // repeatedly discarded newly inserted ranges and allocated GPU buffers
      // for them again on every frame. Bound bytes as well as entry count and
      // retire the least recently used range instead.
      const u64 budget = std::clamp<u64>(parent->length, 256 * 1024, 16 * 1024 * 1024);
      while (!parent->windows.empty() && (parent->windows.size() >= 4096 ||
             parent->window_cache_bytes + length > budget)) {
        auto old = parent->windows.find(parent->window_lru.back());
        parent->window_lru.pop_back();
        parent->window_cache_bytes -= old->second->length;
        HostDevice::RetireResource(std::move(old->second));
        parent->windows.erase(old);
      }
      parent->window_cache_bytes += length;
      parent->window_lru.push_front(key);
      resource->window_lru_position = parent->window_lru.begin();
      parent->windows.emplace(key, resource);
    }
  }
  BufferResourceView prepared;
  ResolveBufferContents(resource, guest_address, kind, byte_order, &prepared);
  return prepared;
}

BufferResourceView ResolveBufferWindowCached(u32 slot, BufferKind kind, u32 guest_address,
    u32 offset, u32 length, const VertexByteOrder& byte_order) {
  static const bool enabled = [] {
    const char* value = std::getenv("LEGO_NATIVE_BUFFER_STREAM_CACHE");
    return value && value[0] == '1' && !std::getenv("LEGO_NATIVE_AUDIT_BUFFER_WATCH");
  }();
  struct Entry {
    u32 address = 0, offset = 0, length = 0, stride = 0, phase = 0;
    u64 generation = 0, epoch = 0, used = 0;
    std::array<u32, 3> header{};
    std::vector<u32> reversal;
    std::weak_ptr<BufferResource> owner;
    std::weak_ptr<plume::RenderBuffer> buffer;
    BufferResourceView view;
  };
  static thread_local std::array<std::array<std::array<Entry, 4>, 1024>, kNativeVertexStreams + 1> entries;
  static thread_local u64 clock = 0;
  if (!enabled || slot >= entries.size() || !guest_address || !CpuBufferMemoryWatchEnabled())
    return ResolveBufferResourceWindow(guest_address, kind, offset, length, byte_order);
  const auto* header = GuestBuffer(guest_address);
  const std::array<u32, 3> words{header->resource.common, header->fetch_lo, header->fetch_hi};
  const auto order = CanonicalVertexByteOrder(byte_order);
  const auto generation = g_buffers.BucketVersion(guest_address);
  const auto epoch = CpuMemoryWriteEpoch();
  const u32 hash = ((guest_address >> 4) ^ (offset * 0x9E3779B9u) ^ (length * 0x85EBCA6Bu));
  auto& bucket = entries[slot][(hash ^ (hash >> 16) ^ (hash >> 10)) & 1023];
  const u64 used = ++clock;
  for (auto& entry : bucket) {
    if (entry.address == guest_address && entry.offset == offset && entry.length == length &&
      entry.stride == order.stride && entry.phase == order.stream_offset &&
      entry.generation == generation && entry.header == words &&
      std::equal(entry.reversal.begin(), entry.reversal.end(), order.reversed_elements.begin(), order.reversed_elements.end())) {
      const auto owner = entry.owner.lock();
      const auto buffer = entry.buffer.lock();
      bool current = false;
      if (owner && buffer) {
        if (epoch && entry.epoch == epoch) {
          // No guest write since the last validation: only the content hash can tell versions apart.
          current = std::atomic_ref<u64>(owner->content_hash).load(std::memory_order_relaxed) == entry.view.content_hash;
        } else {
          std::lock_guard version_lock(owner->upload_mutex);
          current = owner->content_hash == entry.view.content_hash && CpuMemoryUnchanged(owner->cpu_stamp);
        }
      }
      if (current) {
        entry.epoch = epoch; entry.used = used;
        return entry.view;
      }
    }
  }
  auto& entry = *std::min_element(bucket.begin(), bucket.end(), [](const Entry& a, const Entry& b) {
    return a.used < b.used;
  });
  entry.address = 0; entry.generation = 0;
  entry.owner.reset(); entry.buffer.reset();
  const auto view = ResolveBufferResourceWindow(guest_address, kind, offset, length, byte_order);
  if (!view.buffer) return view;
  const auto parent = FindBuffer(guest_address);
  if (!parent || parent->owns_guest_memory) return view;
  const std::array<u32, 3> after{header->resource.common, header->fetch_lo, header->fetch_hi};
  if (after != words || g_buffers.BucketVersion(guest_address) != generation) return view;
  std::lock_guard lock(parent->upload_mutex);
  const auto found = parent->windows.find((u64(offset) << 32) | length);
  if (found == parent->windows.end()) return view;
  const auto& owner = found->second;
  std::lock_guard version_lock(owner->upload_mutex);
  if (!CpuMemoryUnchanged(owner->cpu_stamp)) return view;
  if (owner->buffer.get() != view.buffer) return view;
  entry.address = guest_address; entry.offset = offset; entry.length = length;
  entry.stride = order.stride; entry.phase = order.stream_offset;
  entry.generation = generation; entry.epoch = epoch; entry.used = used; entry.header = words;
  entry.reversal.assign(order.reversed_elements.begin(), order.reversed_elements.end());
  entry.owner = owner; entry.buffer = owner->buffer; entry.view = view;
  return view;
}

BufferResourceView ResolveBufferStreamWindow(u32 stream, u32 guest_address,
    u32 offset, u32 length, const VertexByteOrder& byte_order) {
  return ResolveBufferWindowCached(stream, BufferKind::kVertex, guest_address, offset, length, byte_order);
}

BufferResourceView ResolveBufferIndexWindow(u32 guest_address, u32 offset, u32 length) {
  return ResolveBufferWindowCached(kNativeVertexStreams, BufferKind::kIndex, guest_address, offset, length, {});
}

BufferResourceView ResolveBufferResourceView(u32 guest_address,
                                             BufferKind kind,
                                             const VertexByteOrder& byte_order) {
  // Keep the adopted owner through upload and view construction. A placement
  // publisher may replace the registry entry while this upload is mapping;
  // a second lookup would combine the old buffer with the new length/format.
  // Recorded draws keep the immutable old generation; the next call adopts
  // the replacement header. DispatchDraw serializes content uploads.
  const auto resource = AdoptBuffer(guest_address, kind);
  BufferResourceView prepared;
  ResolveBufferContents(resource, guest_address, kind, byte_order, &prepared);
  return prepared;
}

bool IsNativeBuffer(u32 guest_address) {
  std::lock_guard lock(g_buffers_mutex);
  return g_buffers.contains(guest_address);
}

u32 NativeBufferType(u32 guest_address) {
  const auto resource = FindBuffer(guest_address);
  // Placement headers keep the TU23 types 1/2. In particular XGOffsetResource
  // uses type 1 to update +24, whereas type 6 would update +32 instead.
  // Only our simplified, owned headers need a registry type override.
  return resource && resource->owns_guest_memory ? static_cast<u32>(resource->kind) : 0;
}

u32 AddRefNativeBuffer(u32 guest_address) {
  std::lock_guard lock(g_buffers_mutex);
  if (!g_buffers.contains(guest_address)) {
    return 0;
  }
  auto* header = &GuestBuffer(guest_address)->resource;
  const u32 count = static_cast<u32>(header->reference_count) + 1;
  header->reference_count = count;
  return count;
}

// Resources whose final release is queued but not yet run; guarded by g_buffers_mutex.
std::unordered_set<const void*> g_destroying_buffers;

void DestroyNativeBuffer(u32 guest_address, const std::shared_ptr<BufferResource>& released) {
  auto recording = HostDevice::LockRecording();
  {
    std::lock_guard lock(g_buffers_mutex);
    g_destroying_buffers.erase(released.get());
    const auto it = g_buffers.find(guest_address);
    if (it != g_buffers.end() && it->second == released) g_buffers.erase(it);
  }
  auto* memory = REX_KERNEL_MEMORY();
  HostDevice::RetireResource(released);
  if (released->owns_guest_memory) {
    memory->SystemHeapFree(released->mirror_address);
    memory->SystemHeapFree(guest_address);
  }
}

bool BufferContentCurrent(const BufferPreparationSnapshot& snapshot) {
  const auto resource = std::static_pointer_cast<BufferResource>(snapshot.owner);
  if (!resource) return true;
  std::lock_guard lock(resource->upload_mutex);
  return resource->buffer && CpuMemoryUnchanged(resource->cpu_stamp);
}

std::shared_ptr<void> FindBufferOwner(u32 guest_address) {
  return guest_address ? g_buffers.Lookup(guest_address, g_buffers_mutex) : nullptr;
}

u32 ReleaseNativeBuffer(u32 guest_address) {
  std::shared_ptr<BufferResource> released;
  {
    std::lock_guard lock(g_buffers_mutex);
    const auto it = g_buffers.find(guest_address);
    if (it == g_buffers.end()) {
      return ~u32{0};
    }
    auto* header = &GuestBuffer(guest_address)->resource;
    const u32 old_count = static_cast<u32>(header->reference_count);
    const u32 count = old_count > 0 ? old_count - 1 : 0;
    header->reference_count = count;
    if (count != 0) {
      return count;
    }
    released = it->second;
    if (!g_destroying_buffers.insert(released.get()).second) return 0;
  }
  // Queued draws may still read it, so destruction runs behind them.
  if (NativeWorkerShouldQueue() && NativeDeferredReleaseEnabled()) {
    if (NativeFullDrawPreparationEnabled()) {
      NativeWorkerSubmit([guest_address, released] {
        {
          std::lock_guard lock(g_buffers_mutex);
          const auto it = g_buffers.find(guest_address);
          if (it != g_buffers.end() && it->second == released) g_buffers.erase(it);
        }
        QueueNativeReplayTask([guest_address, released] { DestroyNativeBuffer(guest_address, released); });
      }, false, true, true);
      return 0;
    }
    NativeWorkerSubmit([guest_address, released] { DestroyNativeBuffer(guest_address, released); }, false, false, false, NativeTaskStage::kRecorder);
    return 0;
  }
  DestroyNativeBuffer(guest_address, released);
  return 0;
}

void ResetBufferResources() {
  g_geometry_content_cache.Reset();
  g_upload_pool.Reset();
  g_resident_pool.Reset();
  std::vector<std::pair<u32, u32>> allocations;
  {
    std::lock_guard lock(g_buffers_mutex);
    allocations.reserve(g_buffers.size());
    for (const auto& [guest_address, resource] : g_buffers) {
      if (resource->owns_guest_memory)
        allocations.emplace_back(guest_address, resource->mirror_address);
    }
    g_buffers.clear();
  }
  auto* memory = REX_KERNEL_MEMORY();
  for (const auto [guest_address, mirror_address] : allocations) {
    memory->SystemHeapFree(mirror_address);
    memory->SystemHeapFree(guest_address);
  }
}

u64 BufferRegistryGeneration() { return g_buffers.Generation(); }

bool BufferWindowWatched(const std::shared_ptr<void>& owner, u32 offset, u32 length,
    const plume::RenderBuffer* bound) {
  const auto parent = std::static_pointer_cast<BufferResource>(owner);
  if (!parent || !bound || !CpuBufferMemoryWatchEnabled()) return false;
  std::shared_ptr<BufferResource> window;
  {
    std::lock_guard lock(parent->upload_mutex);
    const auto found = parent->windows.find((u64(offset) << 32) | length);
    if (found == parent->windows.end()) return false;
    window = found->second;
  }
  std::lock_guard lock(window->upload_mutex);
  if (window->cpu_stamp.pages.empty() || !CpuMemoryUnchanged(window->cpu_stamp)) return false;
  for (const auto& [hash, storage] : window->variants)
    if (storage.get() == bound) return true;
  return window->buffer.get() == bound;
}

// Volatile buffers are re-hashed per use and never qualify.
bool BufferInputWatched(const std::shared_ptr<void>& owner) {
  const auto resource = std::static_pointer_cast<BufferResource>(owner);
  if (!resource) return true;
  if (!CpuBufferMemoryWatchEnabled()) return false;
  std::lock_guard lock(resource->upload_mutex);
  const bool volatile_watch = resource->recreate_frame &&
      g_probe_frame.load(std::memory_order_relaxed) + 1 - resource->recreate_frame <= 8;
  return !volatile_watch && !resource->variants.empty() && CpuMemoryUnchanged(resource->cpu_stamp);
}

}  // namespace legodimensions::gpu_native
