/** Portable worker-budget selection for the offline DXC prelinker. */
#pragma once

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstring>
#include <optional>

namespace lego_gpu_aot {

inline std::optional<unsigned> PrelinkWorkerCount(size_t jobs, unsigned hardware,
                                                 const char* requested) {
  // Retain the existing unset default, including its unknown-hardware fallback.
  unsigned workers = hardware ? hardware : 4;
  if (requested) {
    unsigned parsed = 0;
    const char* const end = requested + std::strlen(requested);
    const auto result = std::from_chars(requested, end, parsed, 10);
    if (result.ec != std::errc{} || result.ptr != end || parsed == 0)
      return std::nullopt;
    workers = std::min(workers, parsed);
  }
  return static_cast<unsigned>(std::min<size_t>(jobs, workers));
}

}  // namespace lego_gpu_aot
