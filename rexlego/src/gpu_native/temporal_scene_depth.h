#pragma once
#include <cstdint>
#include <memory>
namespace plume { struct RenderTexture; }
namespace legodimensions::gpu_native {
// Snapshot owns independent R32_FLOAT storage. Keep this shared owner through
// the FSR submission fence; the original scene attachment may be cleared/reused.
struct TemporalSceneDepth {
  std::shared_ptr<plume::RenderTexture> texture;
  uint32_t width = 0, height = 0;
  uint64_t frame = 0;
};
TemporalSceneDepth CaptureTemporalSceneDepth(uint32_t depth_surface,
    uint32_t width, uint32_t height, uint64_t frame,
    const TemporalSceneDepth& reusable = {});
}  // namespace legodimensions::gpu_native
