#pragma once

#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace legodimensions::gpu_native {

struct CpuMemorySpan { uint32_t address = 0, length = 0; };
struct CpuMemoryStamp {
  std::vector<std::pair<uint32_t, uint64_t>> pages;
};
bool CpuMemoryWatchEnabled();
bool CpuBufferMemoryWatchEnabled();

// Uses the SDK's physical write notifications across A/C/E aliases. Unknown
// virtual storage has no stamp and must still be hashed at each use.
CpuMemoryStamp WatchCpuMemory(std::span<const CpuMemorySpan> spans);
bool CpuMemoryUnchanged(const CpuMemoryStamp& stamp);
// Host writes through TranslatePhysical bypass protected guest aliases.
void InvalidateCpuPhysicalMemory(uint32_t address, uint32_t length);
void ShutdownCpuMemoryWatch();

}  // namespace legodimensions::gpu_native
