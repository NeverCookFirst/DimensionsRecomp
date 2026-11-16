#pragma once

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace legodimensions::gpu_native {
enum RendererOptimization : unsigned {
  kDeviceSnapshotPool = 1,
  kRetainedBlendState = 2,
  kSkipEmptyCommandTapes = 4,
};
inline std::atomic<unsigned>& RendererOptimizationMask() {
  static std::atomic<unsigned> mask{[] {
    const auto enabled = [](const char* name) {
      const char* value = std::getenv(name);
      return value && value[0] != '0';
    };
    return (enabled("LEGO_NATIVE_DEVICE_SNAPSHOT_POOL") ? kDeviceSnapshotPool : 0u) |
        (enabled("LEGO_NATIVE_RETAIN_BLEND_STATE") ? kRetainedBlendState : 0u) |
        (enabled("LEGO_NATIVE_SKIP_EMPTY_COMMAND_TAPES") ? kSkipEmptyCommandTapes : 0u);
  }()};
  return mask;
}
inline bool RendererOptimizationEnabled(RendererOptimization option) {
  return (RendererOptimizationMask().load(std::memory_order_relaxed) & option) != 0;
}
inline bool PollRendererOptimizations() {
  static const std::string path = [] {
    const char* value = std::getenv("LEGO_NATIVE_RENDER_OPTIMIZATIONS_FILE");
    return value ? std::string(value) : std::string{};
  }();
  static unsigned presents = 0;
  if (path.empty() || presents++ % 60) return false;
  FILE* input = nullptr;
  if (fopen_s(&input, path.c_str(), "rb") || !input) return false;
  unsigned value = 0;
  char trailing = 0;
  const int fields = std::fscanf(input, "%u %c", &value, &trailing);
  std::fclose(input);
  if (fields != 1 || value > 7) return false;
  return RendererOptimizationMask().exchange(value, std::memory_order_relaxed) != value;
}
}
