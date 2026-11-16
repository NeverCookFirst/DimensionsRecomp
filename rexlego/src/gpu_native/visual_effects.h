#pragma once
#include <atomic>
#include <cstdint>
#include <optional>
#include <string_view>

namespace legodimensions::gpu_native {
enum class NativeCelShadingMode : uint8_t { kOriginal, kEverywhere, kOff };
constexpr std::optional<NativeCelShadingMode> ParseNativeCelShadingMode(std::string_view value) {
  if (value == "original") return NativeCelShadingMode::kOriginal;
  if (value == "everywhere") return NativeCelShadingMode::kEverywhere;
  if (value == "off") return NativeCelShadingMode::kOff;
  return {};
}
struct NativeVisualEffects {
  bool bloom = true;
  NativeCelShadingMode cel_shading = NativeCelShadingMode::kOriginal;
};
// Publish the two controls together at the same recording boundary as the
// immutable render snapshot. Material selection is a guest CPU hot path, so
// it must not acquire shared_ptrs or query string CVars on every material.
inline std::atomic<uint32_t> g_native_visual_effects{0};
inline void PublishNativeVisualEffects(NativeVisualEffects effects) {
  g_native_visual_effects.store(0x80000000u | uint32_t(effects.bloom) |
      (uint32_t(effects.cel_shading) << 1), std::memory_order_release);
}
inline void ClearNativeVisualEffects() {
  g_native_visual_effects.store(0, std::memory_order_release);
}
NativeVisualEffects CurrentNativeVisualEffects();
bool NativeEffectsDiagnosticsEnabled();
void TraceNativeBloomBinding(uint64_t shader, uint32_t mask, bool bloom,
    uint32_t guest, uint32_t descriptor, uint64_t serial, bool repair);

constexpr uint64_t NativeCelShadingPredicate(NativeCelShadingMode mode, uint64_t original) {
  if (mode == NativeCelShadingMode::kEverywhere) return 1;
  // Legacy Off forced a different material technique, not just outlines.
  // Midway character/prop lighting disappears on that path. Retain the
  // world's original technique until a separate outline-only path is verified.
  if (mode == NativeCelShadingMode::kOff) return original;
  return original;
}
// These original composition shaders add max(blur.rgb,0)^2 before exposure.
// Only their blur input changes. DoF, scene lights, alpha and the LUT remain
// original shader operations; other materials' slot 3 is unaffected.
constexpr bool SuppressNativeBloomTexture(uint64_t pixel_shader, uint32_t slot, bool bloom) {
  return !bloom && slot == 3 &&
      (pixel_shader == UINT64_C(0x3A47E5DDE66B42C6) ||
       pixel_shader == UINT64_C(0x967A1882A7502FB3) ||
       pixel_shader == UINT64_C(0x76B999E82C424E40));
}
}  // namespace legodimensions::gpu_native
