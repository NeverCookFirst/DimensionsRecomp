#include "gpu_native/memory_watch.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <rex/cvar.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

REXCVAR_DEFINE_BOOL(gpu_native_texture_watch, true, "GPU",
    "Reuse unchanged native pixel textures with physical page write invalidation")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(gpu_native_buffer_watch, true, "GPU",
    "Reuse unchanged native vertex/index uploads with physical page write invalidation")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace legodimensions::gpu_native {
namespace {
constexpr uint32_t kPageSize = 4096, kPhysicalSize = 0x20000000, kRegionSize = 1u << 20;
// Protected by the SDK global critical region. Fault callbacks never take
// renderer/resource locks and never call the driver.
std::array<std::atomic<uint64_t>, kPhysicalSize / kPageSize> g_page_versions{};
std::array<std::atomic<uint64_t>, kPhysicalSize / kRegionSize> g_region_versions{};
std::atomic<uint64_t> g_write_epoch{1};
rex::thread::global_critical_region g_watch_region;
rex::memory::Memory* g_memory = nullptr;
void* g_callback = nullptr;

void AdvanceWriteEpoch() {
  // Zero permanently disables the shortcut if this counter ever wraps.
  if (g_write_epoch) ++g_write_epoch;
}

bool SelectedMemoryWatch() {
  // Vertex animation textures retain every-use content checks.
  static const bool enabled = [] {
    const char* value = std::getenv("LEGO_NATIVE_STATIC_TEXTURE_WATCH");
    const bool selected = value ? std::strcmp(value, "0") != 0 :
        REXCVAR_GET(gpu_native_texture_watch);
    return selected && !std::getenv("LEGO_NATIVE_NO_MEMORY_WATCH");
  }();
  return enabled;
}

bool SelectedBufferWatch() {
  static const bool enabled = [] {
    const char* value = std::getenv("LEGO_NATIVE_BUFFER_WATCH");
    const bool selected = value ? std::strcmp(value, "0") != 0 :
        REXCVAR_GET(gpu_native_buffer_watch);
    return selected && !std::getenv("LEGO_NATIVE_NO_MEMORY_WATCH");
  }();
  return enabled;
}

void Invalidate(uint32_t address, uint32_t length) {
  if (!length || address >= kPhysicalSize) return;
  const uint64_t end = std::min(uint64_t(address) + length, uint64_t(kPhysicalSize));
  for (uint32_t page = address / kPageSize; uint64_t(page) * kPageSize < end; ++page)
    g_page_versions[page].fetch_add(1, std::memory_order_relaxed);
  for (uint32_t region = address / kRegionSize; uint64_t(region) * kRegionSize < end; ++region)
    g_region_versions[region].fetch_add(1, std::memory_order_relaxed);
  // The epoch publishes the version bumps: a reader that sees it sees them.
  AdvanceWriteEpoch();
}

std::pair<uint32_t, uint32_t> MemoryWritten(void*, uint32_t address,
                                          uint32_t length, bool) {
  Invalidate(address, length);
  // Unprotect only the notified pages. Returning a wider range would leave
  // other stamps falsely clean when the SDK removes their write watches.
  return {address, length};
}

bool PhysicalSpan(rex::memory::Memory* memory, CpuMemorySpan input, CpuMemorySpan& output) {
  if (!input.length || uint64_t(input.address) + input.length > 0x100000000ull) return false;
  const auto physical = [&](uint32_t at) {
    return at < kPhysicalSize ? at : memory->GetPhysicalAddress(at);
  };
  const uint32_t first = physical(input.address);
  if (first == UINT32_MAX || uint64_t(first) + input.length > kPhysicalSize ||
      physical(input.address + input.length - 1) != first + input.length - 1) return false;
  auto* heap = memory->GetPhysicalHeap();
  // WatchCpuMemory holds the SDK global critical region before this heap lock.
  // Validate only the requested pages; QueryRegionInfo scans to the allocation
  // end even when a draw uses a small window within a large shared pool.
  if (!heap->IsRangeCommittedReadable(first, input.length)) return false;
  output = {first, input.length};
  return true;
}
}  // namespace

bool CpuMemoryWatchEnabled() { return SelectedMemoryWatch(); }
bool CpuBufferMemoryWatchEnabled() { return SelectedBufferWatch(); }
uint64_t CpuMemoryWriteEpoch() { return g_write_epoch.load(std::memory_order_acquire); }

CpuMemoryStamp WatchCpuMemory(std::span<const CpuMemorySpan> spans) {
  CpuMemoryStamp stamp;
  if (!SelectedMemoryWatch() && !SelectedBufferWatch()) return stamp;
  auto lock = g_watch_region.Acquire();
  auto* memory = REX_KERNEL_MEMORY();
  if (!memory) return stamp;
  std::vector<CpuMemorySpan> physical;
  for (const auto span : spans) {
    if (!span.length) continue;
    CpuMemorySpan decoded;
    if (!PhysicalSpan(memory, span, decoded)) return {};
    physical.push_back(decoded);
  }
  if (physical.empty()) return {};
  if (!g_callback) {
    g_memory = memory;
    g_callback = memory->RegisterPhysicalMemoryInvalidationCallback(MemoryWritten, nullptr);
  }
  for (const auto span : physical) {
    // Arm BEFORE hashing or copying, so an intervening write makes this stamp
    // unusable rather than caching a mixture as permanently clean.
    memory->EnablePhysicalMemoryAccessCallbacks(span.address, span.length, true, false);
    const uint32_t last = (span.address + span.length - 1) / kPageSize;
    for (uint32_t page = span.address / kPageSize; page <= last; ++page)
      stamp.pages.emplace_back(page, g_page_versions[page].load(std::memory_order_relaxed));
    const uint32_t last_region = (span.address + span.length - 1) / kRegionSize;
    for (uint32_t region = span.address / kRegionSize; region <= last_region; ++region)
      stamp.regions.emplace_back(region, g_region_versions[region].load(std::memory_order_relaxed));
  }
  return stamp;
}

bool CpuMemoryUnchanged(const CpuMemoryStamp& stamp) {
  if (stamp.pages.empty()) return false;
  // Lock-free: the epoch is read before the scan, so a bump during the scan forces a rescan next time.
  const uint64_t epoch = g_write_epoch.load(std::memory_order_acquire);
  std::atomic_ref<uint64_t> checked(stamp.checked_write_epoch);
  if (epoch && checked.load(std::memory_order_relaxed) == epoch) return true;
  static const bool use_regions = [] {
    const char* value = std::getenv("LEGO_NATIVE_WATCH_REGIONS");
    return !value || std::strcmp(value, "0") != 0;
  }();
  bool regions_clean = use_regions && !stamp.regions.empty();
  for (const auto [region, version] : stamp.regions)
    if (g_region_versions[region].load(std::memory_order_relaxed) != version) { regions_clean = false; break; }
  if (!regions_clean)
    for (const auto [page, version] : stamp.pages)
      if (g_page_versions[page].load(std::memory_order_relaxed) != version) return false;
  if (epoch) checked.store(epoch, std::memory_order_relaxed);
  return true;
}

void InvalidateCpuPhysicalMemory(uint32_t address, uint32_t length) {
  auto lock = g_watch_region.Acquire();
  Invalidate(address, length);
}

void ShutdownCpuMemoryWatch() {
  auto lock = g_watch_region.Acquire();
  if (g_callback) g_memory->UnregisterPhysicalMemoryInvalidationCallback(g_callback);
  g_callback = nullptr;
  g_memory = nullptr;
  // Invalidate stamps even if a new device is created with the same Memory.
  for (auto& version : g_page_versions) version.fetch_add(1, std::memory_order_relaxed);
  for (auto& version : g_region_versions) version.fetch_add(1, std::memory_order_relaxed);
  AdvanceWriteEpoch();
}
}  // namespace legodimensions::gpu_native
