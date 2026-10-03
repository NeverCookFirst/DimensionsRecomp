#pragma once

#include <array>
#include <cstdint>

namespace legodimensions::gpu_native {

// Host elapsed time, not GPU timestamps. Serialized by LockRecording.
struct DrawTiming {
  uint64_t calls = 0;
  // Bindings, begin commands, pipeline, constants/textures, vertices, issue, tail.
  std::array<double, 7> stages_ms{};
};
DrawTiming ConsumeDrawTiming();

// Must run before shader/device teardown because cached PSOs refer to both.
void ResetDrawResources();

}  // namespace legodimensions::gpu_native
