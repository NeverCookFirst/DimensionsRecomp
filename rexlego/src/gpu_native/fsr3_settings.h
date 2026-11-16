#pragma once
#include "gpu_native/fsr3_upscaler.h"
#include "gpu_native/render_extent.h"
namespace legodimensions::gpu_native {
bool NativeFsr3Enabled();
// Legacy startup override; empty and "0" do not request 720p rendering.
bool Native720PRequested();
struct NativeFsr3DetailSettings {
  bool sharpen;
  float sharpness;
  float mip_bias_adjustment;
};
struct NativeFsr3Request {
  uint32_t mode;  // Quality index in bits 1..3; enabled in bit 0.
  uint32_t scale;
  NativeFsr3DetailSettings details;
};
NativeFsr3Request CaptureNativeFsr3SettingsRequest();
Fsr3Size NativeFsr3RequestRenderSize(NativeFsr3Request request);
// UI controls read their requested values through the synchronized registry.
NativeFsr3DetailSettings NativeFsr3RequestedDetails();
// Commit requests only between frames; render getters stay fixed until then.
// Returns true only for mode/extent changes requiring surface reconfiguration.
bool ApplyNativeFsr3Settings();
enum class NativeFsr3ApplyResult { kUnchanged, kApplied, kDeferred };
// Runtime frame boundary: prepare/publish scene storage before changing active
// mode/details. A failed preparation keeps every active setting unchanged for
// this frame; the requested registry values remain available for a later retry.
// Caller holds the recording lock; callback must preserve old storage on false.
NativeFsr3ApplyResult TryApplyNativeFsr3Settings(bool (*prepare_scene)(NativeRenderExtent));
// Copied revision payload. After normal startup initializes active state, this
// overload performs no registry/environment reads; all fields stay together
// while resource preparation is in flight.
NativeFsr3ApplyResult TryApplyNativeFsr3Settings(NativeFsr3Request request,
                                               bool (*prepare_scene)(NativeRenderExtent));
Fsr3Size NativeFsr3RenderSize();
// Internal resolution percent of 1280x720 for the native (non-FSR) path, committed at the frame boundary.
uint32_t NativeResolutionScale();
// The percent the settings registry asks for; the UI reads this before it is committed.
uint32_t NativeRequestedResolutionScale();
// Clamps to 20..300 (3840x2160) and rounds to a multiple of 5 so the size stays an exact 16:9 integer.
uint32_t NativeResolutionScaleStep(int32_t percent);
Fsr3Size NativeScaledSceneSize(uint32_t percent);
// The host size the 1280x720 scene renders at: the FSR input size, or the scaled native size.
Fsr3Size NativeSceneRenderSize();
float NativeFsr3Sharpness();
bool NativeFsr3Sharpening();
float NativeFsr3MipBias();
NativeRenderExtent NativeConfiguredSceneExtent(uint32_t width, uint32_t height);
}  // namespace legodimensions::gpu_native
