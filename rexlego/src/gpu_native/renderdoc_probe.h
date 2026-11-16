#pragma once

// Opt-in single-frame diagnostic. No RenderDoc dependency is loaded in normal
// runs; the module must be attached before creating the native D3D12 device.
#include <cstdlib>
#include <chrono>
#include <filesystem>
#include <windows.h>
#include <renderdoc_app.h>
#include <rex/logging.h>

namespace legodimensions::gpu_native {
#if defined(LEGODIMENSIONS_DEV_PROBES)
inline RENDERDOC_API_1_0_0* g_logo_capture_api = nullptr;
inline std::filesystem::path g_manual_capture_trigger;

inline void InitializeLogoCapture() {
  const auto* dll = std::getenv("LEGO_NATIVE_RENDERDOC_DLL");
  const auto* path = std::getenv("LEGO_NATIVE_RENDERDOC_CAPTURE");
  if (!dll || !*dll || !path || !*path) return;
  if (!std::filesystem::path(dll).is_absolute() ||
      !std::filesystem::path(path).is_absolute()) {
    REXLOG_ERROR("Native logo capture requires absolute DLL and output paths");
    return;
  }
  const auto* trigger = std::getenv("LEGO_NATIVE_RENDERDOC_TRIGGER");
  if (trigger && *trigger) {
    if (!std::filesystem::path(trigger).is_absolute()) {
      REXLOG_ERROR("Native manual capture requires an absolute trigger path");
      return;
    }
    g_manual_capture_trigger = trigger;
  }
  // Keep the capture module loaded through all D3D12 resource destruction.
  const auto module = LoadLibraryW(std::filesystem::path(dll).c_str());
  const auto get_api = module ? reinterpret_cast<pRENDERDOC_GetAPI>(
      GetProcAddress(module, "RENDERDOC_GetAPI")) : nullptr;
  if (!get_api || !get_api(eRENDERDOC_API_Version_1_0_0,
                          reinterpret_cast<void**>(&g_logo_capture_api))) {
    REXLOG_ERROR("Native logo capture could not initialize RenderDoc");
    g_logo_capture_api = nullptr;
    return;
  }
  g_logo_capture_api->SetLogFilePathTemplate(path);
  g_logo_capture_api->MaskOverlayBits(0, 0);
  REXLOG_INFO("Native logo capture armed: {}", path);
}

inline void RequestLogoCapture() {
  static bool requested = false;
  if (!g_logo_capture_api || requested || !g_manual_capture_trigger.empty()) return;
  requested = true;
  // Automatic capture starts at the next present, recording a complete frame
  // with the logo fully faded in, including its preceding depth-writing draws.
  g_logo_capture_api->TriggerCapture();
  REXLOG_INFO("Native logo capture requested at TT opacity >= 0.95");
}

inline void ReportLogoCapture() {
  if (!g_logo_capture_api) return;
  static auto checked = std::chrono::steady_clock::time_point{};
  static unsigned requested = 0, reported = 0;
  const auto now = std::chrono::steady_clock::now();
  if (!g_manual_capture_trigger.empty() && requested < 4 &&
      now-checked >= std::chrono::milliseconds(500)) {
    checked = now;
    std::error_code error;
    if (std::filesystem::remove(g_manual_capture_trigger,error)) {
      ++requested;
      // Called after Present: record the following complete frame, with no
      // application snapshots or early startup/logo capture required.
      g_logo_capture_api->TriggerCapture();
      REXLOG_INFO("Native manual RenderDoc capture requested ({}/4)",requested);
    }
  }
  const auto captures = g_logo_capture_api->GetNumCaptures();
  if (captures > reported) {
    reported = captures;
    REXLOG_INFO("Native RenderDoc captures saved: {}",reported);
  }
}
#else
inline constexpr RENDERDOC_API_1_0_0* g_logo_capture_api = nullptr;
inline void InitializeLogoCapture() {}
inline void RequestLogoCapture() {}
inline void ReportLogoCapture() {}
#endif
}  // namespace legodimensions::gpu_native
