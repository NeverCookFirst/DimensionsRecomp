#include "gpu_native/settings_snapshot.h"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <stdexcept>

namespace legodimensions::gpu_native {
namespace {
std::string_view Value(const settings::Values& values, std::string_view key) {
  const auto it = values.find(key);
  if (it == values.end()) throw std::runtime_error("Native settings snapshot is missing " + std::string(key));
  return it->second;
}
bool Boolean(const settings::Values& values, std::string_view key) {
  const auto text = Value(values, key);
  if (text == "true") return true;
  if (text == "false") return false;
  throw std::runtime_error("Invalid boolean in native settings snapshot: " + std::string(key));
}
template<class T> T Number(const settings::Values& values, std::string_view key) {
  const auto text = Value(values, key);
  T value{};
  const auto parsed = std::from_chars(text.data(), text.data()+text.size(), value);
  if (parsed.ec != std::errc{} || parsed.ptr != text.data()+text.size() || !std::isfinite(double(value)))
    throw std::runtime_error("Invalid number in native settings snapshot: " + std::string(key));
  return value;
}
uint32_t Quality(std::string_view value) {
  if (value == "native") return 0;
  if (value == "balanced") return 2;
  if (value == "performance") return 3;
  if (value == "ultra_performance") return 4;
  return 1;  // Same legacy fallback as the requested-settings path.
}
}

std::optional<NativeRenderSettingsSnapshot> DecodeNativeRenderSettings(
    const settings::SettingsRevision& revision, const NativeSettingsOverrides& overrides,
    std::string& error) {
  error.clear();
  try {
    const auto& values = revision.values;
    NativeRenderSettingsSnapshot snapshot;
    snapshot.revision = revision.id;
    snapshot.fullscreen = Boolean(values,"fullscreen");
    snapshot.vsync = overrides.vsync.value_or(Boolean(values,"native_vsync"));
    snapshot.depth_of_field = Boolean(values,"depth_of_field");
    snapshot.show_fps = Boolean(values,"native_show_fps");
    snapshot.bloom = Boolean(values,"native_bloom");
    const auto cel = ParseNativeCelShadingMode(Value(values,"native_cel_shading"));
    if (!cel) throw std::runtime_error("Invalid cel shading mode in native settings snapshot");
    snapshot.cel_shading = *cel;
    const auto enabled = overrides.fsr3.value_or(Boolean(values,"native_fsr3"));
    const auto requested_quality = Value(values,"native_fsr3_quality");
    const auto quality = overrides.native_720p ? 1u : Quality(overrides.fsr3_quality ?
        std::string_view(*overrides.fsr3_quality) : requested_quality);
    const auto scale = Number<int32_t>(values,"native_resolution_scale");
    snapshot.fsr3 = {(quality<<1)|uint32_t(enabled), scale>0 ? NativeResolutionScaleStep(scale) :
        overrides.native_720p ? 100u : 150u,
        {Boolean(values,"native_fsr3_sharpen"), std::clamp(Number<float>(values,"native_fsr3_sharpness"),0.f,1.f),
         std::clamp(Number<float>(values,"native_fsr3_mip_bias"),-1.f,1.f)}};
    // Runtime legacy negative caps already mean uncapped. Keep positive legacy
    // values intact; the menu validates new edits against its supported range.
    const auto fps = Number<int32_t>(values,"native_fps_limit");
    snapshot.fps_limit = overrides.fps_limit ? std::clamp(*overrides.fps_limit,0,1000) : std::max(0,fps);
    const auto af = Number<int32_t>(values,"native_anisotropic_override");
    const auto inherited = overrides.inherited_anisotropy;
    snapshot.anisotropy = af>=-1 && af<=5 ? af : inherited>=0 && inherited<=5 ? inherited : -1;
    snapshot.fsr1 = Value(values,"present_effect") == "fsr";
    snapshot.fsr1_sharpness_reduction = std::clamp(Number<float>(values,"present_fsr_sharpness_reduction"),0.f,2.f);
    snapshot.fsr1_passes = std::clamp(Number<int32_t>(values,"present_fsr_max_upsampling_passes"),1,4);
    snapshot.skip_pixel_shaders = overrides.skip_pixel_shaders;
    snapshot.present_dither = overrides.present_dither;
    return snapshot;
  } catch (const std::exception& e) {
    error = e.what();
    return {};
  }
}

}  // namespace legodimensions::gpu_native
