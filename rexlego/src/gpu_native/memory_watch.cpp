#include "gpu_native/memory_watch.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <rex/cvar.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

REXCVAR_DEFINE_BOOL(gpu_native_texture_watch, false, "GPU",
    "Reuse unchanged native pixel textures with physical page write invalidation")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(gpu_native_buffer_watch, false, "GPU",
    "Reuse unchanged native vertex/index uploads with physical page write invalidation")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace legodimensions::gpu_native {
namespace {
constexpr uint32_t kPageSize = 4096, kPhysicalSize = 0x20000000;
// Protected by the SDK global critical region. Fault callbacks never take
// renderer/resource locks and never call the driver.
std::array<uint64_t, kPhysicalSize / kPageSize> g_page_versions{};
uint64_t g_write_epoch = 1;
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
  AdvanceWriteEpoch();
  const uint64_t end = std::min(uint64_t(address) + length, uint64_t(kPhysicalSize));
  for (uint32_t page = address / kPageSize; uint64_t(page) * kPageSize < end; ++page)
    ++g_page_versions[page];
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
  for (uint64_t at = first, end = uint64_t(first) + input.length; at < end;) {
    rex::memory::HeapAllocationInfo info{};
    if (!heap->QueryRegionInfo(uint32_t(at), &info) ||
        !(info.state & rex::memory::kMemoryAllocationCommit) ||
        !(info.protect & rex::memory::kMemoryProtectRead)) return false;
    const uint64_t page = heap->heap_base() + ((at - heap->heap_base()) / heap->page_size()) * heap->page_size();
    const uint64_t next = page + info.region_size;
    if (next <= at) return false;
    at = std::min(next, end);
  }
  output = {first, input.length};
  return true;
}
}  // namespace

bool CpuMemoryWatchEnabled() { return SelectedMemoryWatch(); }
bool CpuBufferMemoryWatchEnabled() { return SelectedBufferWatch(); }

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
      stamp.pages.emplace_back(page, g_page_versions[page]);
  }
  return stamp;
}

bool CpuMemoryUnchanged(const CpuMemoryStamp& stamp) {
  if (stamp.pages.empty()) return false;
  auto lock = g_watch_region.Acquire();
  if (g_write_epoch && stamp.checked_write_epoch == g_write_epoch) return true;
  for (const auto [page, version] : stamp.pages)
    if (g_page_versions[page] != version) return false;
  stamp.checked_write_epoch = g_write_epoch;
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
  AdvanceWriteEpoch();
  for (auto& version : g_page_versions) ++version;
}
}  // namespace legodimensions::gpu_native
