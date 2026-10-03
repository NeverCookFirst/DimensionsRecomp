#pragma once

// Opt-in single-frame diagnostic. No RenderDoc dependency is loaded in normal
// runs; the module must be attached before creating the native D3D12 device.
#include <cstdlib>
#include <filesystem>
#include <windows.h>
#include <renderdoc/renderdoc_app.h>
#include <rex/logging.h>

namespace legodimensions::gpu_native {
inline RENDERDOC_API_1_0_0* g_logo_capture_api = nullptr;

inline void InitializeLogoCapture() {
  const auto* dll = std::getenv("LEGO_NATIVE_RENDERDOC_DLL");
  const auto* path = std::getenv("LEGO_NATIVE_RENDERDOC_CAPTURE");
  if (!dll || !*dll || !path || !*path) return;
  if (!std::filesystem::path(dll).is_absolute() ||
      !std::filesystem::path(path).is_absolute()) {
    REXLOG_ERROR("Native logo capture requires absolute DLL and output paths");
    return;
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
  if (!g_logo_capture_api || requested) return;
  requested = true;
  // Automatic capture starts at the next present, recording a complete frame
  // with the logo fully faded in, including its preceding depth-writing draws.
  g_logo_capture_api->TriggerCapture();
  REXLOG_INFO("Native logo capture requested at TT opacity >= 0.95");
}

inline void ReportLogoCapture() {
  static bool reported = false;
  if (!g_logo_capture_api || reported || !g_logo_capture_api->GetNumCaptures()) return;
  reported = true;
  REXLOG_INFO("Native logo capture saved (one frame)");
}
}  // namespace legodimensions::gpu_native
