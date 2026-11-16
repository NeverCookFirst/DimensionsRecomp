#pragma once

#include "gpu_native/fsr3_settings.h"
#include "gpu_native/visual_effects.h"
#include "settings_revision.h"
#include <optional>
#include <string>

namespace legodimensions::gpu_native {

// Resolve environment/installation overrides on the UI side and copy them
// with the request. The decoder and render consumers do no environment/file IO.
struct NativeSettingsOverrides {
  std::optional<bool> fsr3, vsync;
  std::optional<std::string> fsr3_quality;
  std::optional<int32_t> fps_limit;
  bool native_720p = false;
  int32_t inherited_anisotropy = -1;
  std::string skip_pixel_shaders;
  bool present_dither = true;
};
struct NativeRenderSettingsSnapshot {
  uint64_t revision = 0;
  NativeFsr3Request fsr3{3,150,{true,.2f,0}};
  bool fullscreen = false, vsync = false, depth_of_field = true, fsr1 = false;
  bool show_fps = true;
  bool bloom = true;
  NativeCelShadingMode cel_shading = NativeCelShadingMode::kOriginal;
  int32_t fps_limit = 240, anisotropy = -1, fsr1_passes = 4;
  float fsr1_sharpness_reduction = .2f;
  std::string skip_pixel_shaders;
  bool present_dither = true;
};

std::optional<NativeRenderSettingsSnapshot> DecodeNativeRenderSettings(
    const settings::SettingsRevision& revision, const NativeSettingsOverrides& overrides,
    std::string& error);

}  // namespace legodimensions::gpu_native
