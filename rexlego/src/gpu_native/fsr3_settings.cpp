#include "gpu_native/fsr3_settings.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <atomic>
#include <rex/cvar.h>

REXCVAR_DEFINE_BOOL(native_fsr3, true, "Graphics", "Enable native FSR 3 upscaling (no frame generation)");
REXCVAR_DEFINE_STRING(native_fsr3_quality, "quality", "Graphics", "FSR 3 render quality");
REXCVAR_DEFINE_BOOL(native_fsr3_sharpen, true, "Graphics", "Enable FSR 3 sharpening");
REXCVAR_DEFINE_DOUBLE(native_fsr3_sharpness, 0.2, "Graphics", "FSR 3 sharpening strength (0 to 1)")
    .range(0.0, 1.0);
REXCVAR_DEFINE_DOUBLE(native_fsr3_mip_bias, 0.0, "Graphics",
    "Adjustment to automatic FSR texture detail bias; positive values reduce texture shimmer")
    .range(-1.0, 1.0);
REXCVAR_DEFINE_INT32(native_resolution_scale, 0, "Graphics",
    "Internal resolution in percent of 1280x720 when upscaling is off (20 to 300, steps of 5; 300 is 3840x2160); 0 is 1080p, or 720p with LEGO_NATIVE_720P");

namespace legodimensions::gpu_native {
bool Native720PRequested() {
  static const bool requested = [] {
    const char* value = std::getenv("LEGO_NATIVE_720P");
    return value && *value && value[0] != '0';
  }();
  return requested;
}
NativeFsr3DetailSettings NativeFsr3RequestedDetails() {
  const float sharpness=float(rex::cvar::Query<double>("native_fsr3_sharpness"));
  const float adjustment=float(rex::cvar::Query<double>("native_fsr3_mip_bias"));
  return {rex::cvar::Query<bool>("native_fsr3_sharpen"),
      std::isfinite(sharpness)?std::clamp(sharpness,0.0f,1.0f):0.2f,
      std::isfinite(adjustment)?std::clamp(adjustment,-1.0f,1.0f):0.0f};
}
namespace {
constexpr Fsr3Size kSceneSizes[]{{1920,1080},{1280,720},{1129,635},{960,540},{640,360}};
uint32_t RequestedMode() {
  const bool enabled = [] {
    if (const char* value = std::getenv("LEGO_NATIVE_FSR3"); value && *value)
      return value[0] != '0';
    return rex::cvar::Query<bool>("native_fsr3");
  }();
  const uint32_t mode = []() -> uint32_t {
    if (Native720PRequested()) return 1;
    // The settings UI runs on another thread. Copy strings while holding the
    // registry lock; a direct Cvar reference can race a preset reallocation.
    auto quality = rex::cvar::GetFlagByName("native_fsr3_quality");
    if (const char* value = std::getenv("LEGO_NATIVE_FSR3_QUALITY"); value && *value)
      quality = value;
    if (quality == "native") return 0;
    if (quality == "balanced") return 2;
    if (quality == "performance") return 3;
    if (quality == "ultra_performance") return 4;
    return 1;
  }();
  return (mode << 1) | uint32_t(enabled);
}
uint32_t RequestedScale() {
  const int32_t percent = rex::cvar::Query<int32_t>("native_resolution_scale");
  return percent > 0 ? NativeResolutionScaleStep(percent) : Native720PRequested() ? 100u : 150u;
}
float MipBiasForMode(uint32_t mode,float adjustment) {
  if (!(mode&1u)) return 0;
  return std::clamp(std::log2(float(kSceneSizes[mode>>1].width)/1920.0f)-1.0f+adjustment,-3.0f,0.0f);
}
struct FrameSettings {
  std::atomic<uint32_t> mode{RequestedMode()};
  std::atomic<uint32_t> scale{RequestedScale()};
  std::atomic<bool> sharpen{true};
  std::atomic<float> sharpness{0.2f},mip_bias{0.0f};
  FrameSettings() { SetDetails(mode.load(),NativeFsr3RequestedDetails()); }
  void SetDetails(uint32_t selected,NativeFsr3DetailSettings details) {
    sharpen.store(details.sharpen);sharpness.store(details.sharpness);
    mip_bias.store(MipBiasForMode(selected,details.mip_bias_adjustment));
  }
};
FrameSettings& ActiveSettings() {
  static FrameSettings settings;
  return settings;
}
}
NativeFsr3Request CaptureNativeFsr3SettingsRequest() {
  return {RequestedMode(),RequestedScale(),NativeFsr3RequestedDetails()};
}
Fsr3Size NativeFsr3RequestRenderSize(NativeFsr3Request request) {
  return request.mode < 10 && (request.mode & 1u) ? kSceneSizes[request.mode >> 1] : NativeScaledSceneSize(request.scale);
}
NativeFsr3ApplyResult TryApplyNativeFsr3Settings(NativeFsr3Request request,
                                               bool (*prepare_scene)(NativeRenderExtent)) {
  // SwapHook holds the recording lock. No draw can observe part of this
  // publication; UI readers use the requested registry values instead.
  if (request.mode >= 10 || request.scale < 20 || request.scale > 300 || request.scale % 5 ||
      !std::isfinite(request.details.sharpness) || request.details.sharpness < 0 || request.details.sharpness > 1 ||
      !std::isfinite(request.details.mip_bias_adjustment) || request.details.mip_bias_adjustment < -1 ||
      request.details.mip_bias_adjustment > 1) return NativeFsr3ApplyResult::kDeferred;
  auto& active=ActiveSettings();
  const auto requested = request.mode;
  const auto scale = request.scale;
  const auto details = request.details;
  const bool mode_changed = active.mode.load() != requested;
  const bool scale_changed = active.scale.load() != scale;
  // The scale only sizes the scene while upscaling is off.
  const bool reconfigure = mode_changed || (scale_changed && !(requested & 1u));
  if (reconfigure && prepare_scene) {
    const auto size = (requested & 1u) ? kSceneSizes[requested >> 1] : NativeScaledSceneSize(scale);
    if (!prepare_scene({1280,720,size.width,size.height})) return NativeFsr3ApplyResult::kDeferred;
  }
  active.SetDetails(requested,details);
  active.mode.store(requested);
  active.scale.store(scale);
  return reconfigure ? NativeFsr3ApplyResult::kApplied : NativeFsr3ApplyResult::kUnchanged;
}
NativeFsr3ApplyResult TryApplyNativeFsr3Settings(bool (*prepare_scene)(NativeRenderExtent)) {
  return TryApplyNativeFsr3Settings(CaptureNativeFsr3SettingsRequest(),prepare_scene);
}
bool ApplyNativeFsr3Settings() {
  // Startup only: surfaces do not exist yet and HostDevice creation consumes
  // the resulting extent. The live SwapHook must use the preparing overload.
  return TryApplyNativeFsr3Settings(nullptr) == NativeFsr3ApplyResult::kApplied;
}
uint32_t NativeResolutionScaleStep(int32_t percent) {
  return uint32_t((std::clamp(percent, 20, 300) + 2) / 5 * 5);
}
uint32_t NativeResolutionScale() { return ActiveSettings().scale.load(); }
uint32_t NativeRequestedResolutionScale() { return RequestedScale(); }
Fsr3Size NativeScaledSceneSize(uint32_t percent) {
  return {1280u * percent / 100u, 720u * percent / 100u};
}
bool NativeFsr3Enabled() { return (ActiveSettings().mode.load() & 1u) != 0; }
Fsr3Size NativeFsr3RenderSize() {
  return kSceneSizes[ActiveSettings().mode.load() >> 1];
}
float NativeFsr3Sharpness() {
  return ActiveSettings().sharpness.load();
}
bool NativeFsr3Sharpening() { return ActiveSettings().sharpen.load(); }
float NativeFsr3MipBias() {
  return ActiveSettings().mip_bias.load();
}
NativeRenderExtent NativeConfiguredSceneExtent(uint32_t width, uint32_t height) {
  // Only the 1280x720 scene surfaces scale; CPU assets, portraits and shadow atlases keep their size.
  if (width != 1280 || height != 720) return {width,height,width,height};
  const auto size = NativeSceneRenderSize();
  return {width,height,size.width,size.height};
}
Fsr3Size NativeSceneRenderSize() {
  return NativeFsr3Enabled() ? NativeFsr3RenderSize() : NativeScaledSceneSize(NativeResolutionScale());
}
}  // namespace legodimensions::gpu_native
