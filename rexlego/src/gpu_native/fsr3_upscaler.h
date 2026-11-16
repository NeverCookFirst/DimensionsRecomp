#pragma once
#include <cstdint>
#include <memory>

struct ID3D12Device;
struct ID3D12GraphicsCommandList;
struct ID3D12Resource;

namespace legodimensions::gpu_native {

struct Fsr3Size {
  uint32_t width = 0, height = 0;
};

// Input SRVs must be in NON_PIXEL_SHADER_RESOURCE, output in UNORDERED_ACCESS.
// Motion is unjittered previous-minus-current normalized UV. Depth must be a
// separate R32_FLOAT snapshot of the same scene, before its attachment is reused.
// The caller retains this object AND all inputs through the submission fence.
struct Fsr3FrameInputs {
  ID3D12Resource* color = nullptr;
  ID3D12Resource* depth = nullptr;
  ID3D12Resource* motion = nullptr;
  ID3D12Resource* reactive = nullptr;
  ID3D12Resource* output = nullptr;
  uint64_t frame = 0, scene_epoch = 0;
  float jitter_x = 0, jitter_y = 0;
  float frame_time_ms = 0, pre_exposure = 1;
  float camera_near = 0, camera_far = 0, camera_fov = 0;
  float sharpness = 0;
  bool sharpen = false, camera_cut = false;
};

// Fixed-size context. Resize/settings changes create a replacement; retire the
// old context at its last submission fence. No frame-generation provider exists
// in this class. The normal game present path must supply genuine scene inputs.
class Fsr3Upscaler {
 public:
  static std::shared_ptr<Fsr3Upscaler> Create(ID3D12Device* device,
      Fsr3Size render, Fsr3Size output, bool inverted_depth = false);
  ~Fsr3Upscaler();
  Fsr3Upscaler(const Fsr3Upscaler&) = delete;
  Fsr3Upscaler& operator=(const Fsr3Upscaler&) = delete;
  bool Jitter(uint64_t frame, float& x, float& y) const;
  bool Dispatch(ID3D12GraphicsCommandList* commands, const Fsr3FrameInputs& inputs);
  void InvalidateHistory();
  bool LastDispatchResetHistory() const;

 private:
  struct Impl;
  explicit Fsr3Upscaler(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

}  // namespace legodimensions::gpu_native
