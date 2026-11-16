#include "gpu_native/settings_runtime.h"
#include "settings_registry.h"
#include "gpu_native/device.h"
#include "gpu_native/temporal_present.h"
#include "gpu_native/textures.h"
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/window.h>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <sstream>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace legodimensions::gpu_native {
namespace {
std::atomic<std::shared_ptr<NativeSettingsRuntime>> published;
constexpr std::string_view kAux = "@native:";

// Capture on the UI thread, alongside the whole registry proposal. Auxiliary
// values participate in conflict checks but are never editable/persisted rows.
void CaptureInputs(settings::Values& values) {
  const auto put = [&](std::string_view key, std::string value) {
    values[std::string(kAux) + std::string(key)] = std::move(value);
  };
  const auto env = [&](const char* key, std::string_view name) {
    const char* value = std::getenv(name.data());
    put(key, value && *value ? value : "");
  };
  env("fsr3", "LEGO_NATIVE_FSR3");
  env("vsync", "LEGO_NATIVE_VSYNC");
  env("quality", "LEGO_NATIVE_FSR3_QUALITY");
  env("fps", "LEGO_NATIVE_FPS_LIMIT");
  put("720p", Native720PRequested() ? "true" : "false");
  put("af", std::to_string(NativeInheritedAnisotropy()));
  put("dither", rex::cvar::Query<bool>("present_dither") ? "true" : "false");
  // The DoF callback itself edits the legacy skip list. Normalize that own
  // entry away so the same request survives its legitimate setter callback.
  auto raw = rex::cvar::GetFlagByName("skip_pixel_shaders");
  std::replace(raw.begin(), raw.end(), ',', ' ');
  std::istringstream tokens(raw);
  std::string token, skip;
  while (tokens >> token) {
    auto compare = token;
    std::transform(compare.begin(), compare.end(), compare.begin(), [](unsigned char c) { return char(std::toupper(c)); });
    if (compare == "CFEAC7ADB912F8A9" || compare == "0XCFEAC7ADB912F8A9") continue;
    if (!skip.empty()) skip += ',';
    skip += token;
  }
  put("skip", std::move(skip));
  int sync = -1, flags = 0;
  if (const char* value = std::getenv("LEGO_NATIVE_PRESENT")) std::sscanf(value, "%d,%d", &sync, &flags);
  put("present_sync", std::to_string(sync));
}
NativeSettingsOverrides Inputs(const settings::SettingsRevision& revision) {
  const auto get = [&](const char* key) -> const std::string& { return revision.values.at(std::string(kAux) + key); };
  NativeSettingsOverrides result;
  if (!get("fsr3").empty()) result.fsr3 = get("fsr3")[0] != '0';
  if (!get("vsync").empty()) result.vsync = get("vsync")[0] != '0';
  if (!get("quality").empty()) result.fsr3_quality = get("quality");
  if (!get("fps").empty()) result.fps_limit = std::clamp(std::atoi(get("fps").c_str()), 0, 1000);
  const int sync = std::stoi(get("present_sync"));
  if (sync >= 0) result.vsync = sync != 0;
  result.native_720p = get("720p") == "true";
  result.inherited_anisotropy = std::stoi(get("af"));
  result.skip_pixel_shaders = get("skip");
  result.present_dither = get("dither") == "true";
  return result;
}
settings::Backend CaptureBackend(settings::Backend backend) {
  auto read = std::move(backend.read);
  backend.read = [read = std::move(read)] { auto values = read(); CaptureInputs(values); return values; };
  return backend;
}
std::shared_ptr<NativeSettingsFrame> Decode(settings::RevisionTicket ticket) {
  std::string error;
  auto decoded = DecodeNativeRenderSettings(*ticket, Inputs(*ticket), error);
  if (!decoded) throw std::runtime_error(error);
  return std::make_shared<NativeSettingsFrame>(NativeSettingsFrame{std::move(ticket), std::move(*decoded)});
}
settings::RevisionResult CheckWindow(rex::ui::Window* window, const settings::SettingsRevision& revision,
                                     const settings::PresentationObservation& presentation) {
  using settings::RevisionState;
  const auto pending = [&] { return settings::RevisionResult{revision.id, RevisionState::kPending, {}}; };
  if (!window || !window->HasActualState()) return pending();
  const auto handle = static_cast<HWND>(window->GetNativeWindowHandle());
  if (!handle || !IsWindow(handle) || IsIconic(handle)) return pending();
  // HWND/client coordinates are physical in the window's own DPI context.
  struct DpiScope {
    DPI_AWARENESS_CONTEXT previous;
    explicit DpiScope(HWND handle) : previous(SetThreadDpiAwarenessContext(GetWindowDpiAwarenessContext(handle))) {}
    ~DpiScope() { if (previous) SetThreadDpiAwarenessContext(previous); }
  } dpi(handle);
  RECT client{};
  if (!GetClientRect(handle, &client)) return pending();
  POINT origin{client.left, client.top};
  if (!ClientToScreen(handle, &origin)) return pending();
  const auto width = uint32_t(client.right - client.left), height = uint32_t(client.bottom - client.top);
  if (!width || !height || width != presentation.width || height != presentation.height ||
      width != window->GetActualPhysicalWidth() || height != window->GetActualPhysicalHeight()) return pending();
  std::string error;
  const auto desired = DecodeNativeRenderSettings(revision, Inputs(revision), error);
  if (!desired) return {revision.id, RevisionState::kFailed, error};
  if (presentation.vsync != desired->vsync) return pending();
  const auto style = GetWindowLongPtrW(handle, GWL_STYLE);
  // SDL keeps WS_MINIMIZEBOX in fullscreen. It is also part of
  // WS_OVERLAPPEDWINDOW, so testing that entire mask would reject every
  // legitimate SDL fullscreen window. Only visible framing matters here.
  const bool borderless = (style & WS_POPUP) && !(style & (WS_CAPTION | WS_THICKFRAME));
  if (window->IsFullscreen() != desired->fullscreen || borderless != desired->fullscreen) return pending();
  if (desired->fullscreen) {
    MONITORINFO monitor{sizeof(MONITORINFO)};
    if (!GetMonitorInfoW(MonitorFromWindow(handle, MONITOR_DEFAULTTONEAREST), &monitor) ||
        origin.x != monitor.rcMonitor.left || origin.y != monitor.rcMonitor.top ||
        width != uint32_t(monitor.rcMonitor.right-monitor.rcMonitor.left) ||
        height != uint32_t(monitor.rcMonitor.bottom-monitor.rcMonitor.top)) return pending();
  }
  return {revision.id, RevisionState::kComplete, {}};
}
}

NativeSettingsRuntime::NativeSettingsRuntime(settings::Backend registry, rex::ui::Window* window)
    : coordinator_(std::make_unique<settings::SettingsRevisionCoordinator>(CaptureBackend(std::move(registry)),
          [window](const auto& request, const auto& presentation) { return CheckWindow(window, request, presentation); })) {
  active_.store(Decode(coordinator_->ActiveForRenderer()));
  const auto effects = Active()->values;
  PublishNativeVisualEffects({effects.bloom, effects.cel_shading});
}
settings::Backend NativeSettingsRuntime::MakeBackend() const { return coordinator_->MakeBackend(); }
bool NativeSettingsRuntime::AdvanceFromUI() { return coordinator_->AdvanceFromUI(); }
void NativeSettingsRuntime::StopFromUI() { coordinator_->StopFromUI(); }
std::shared_ptr<const NativeSettingsFrame> NativeSettingsRuntime::Active() const { return active_.load(); }
NativeFsr3ApplyResult NativeSettingsRuntime::CommitFrameBoundary() {
  const auto ticket = coordinator_->PendingForRenderer();
  if (!ticket) return NativeFsr3ApplyResult::kUnchanged;
  auto result = NativeFsr3ApplyResult::kUnchanged;
  coordinator_->TryCommit(ticket, [&](const settings::SettingsRevision& revision) {
    const auto next = Decode(ticket);
    const auto previous = Active();
    const auto request = next->values.fsr3;
    const bool reset = request.mode != previous->values.fsr3.mode;
    std::shared_ptr<PreparedTemporalSettings> temporal;
    if (request.mode & 1u) {
      const auto size = NativeFsr3RequestRenderSize(request);
      temporal = PrepareTemporalSettings(size.width, size.height, TemporalDepthInverted());
      if (!CanCommitTemporalSettings(temporal))
        return settings::RevisionResult{revision.id, settings::RevisionState::kFailed, "Could not prepare FSR 3 resources; previous settings retained"};
    }
    result = TryApplyNativeFsr3Settings(request, ReconfigureNativeSceneSurfaces);
    if (result == NativeFsr3ApplyResult::kDeferred)
      return settings::RevisionResult{revision.id, settings::RevisionState::kFailed, "Could not prepare scene surfaces; previous settings retained"};
    // This recording boundary owns the temporal generation. Scene preparation
    // cannot invalidate the checked plan; no draw/present runs between commits.
    if (temporal && !CommitTemporalSettings(temporal, reset)) std::terminate();
    SetNativeActiveAnisotropy(next->values.anisotropy);
    active_.store(next);
    PublishNativeVisualEffects({next->values.bloom, next->values.cel_shading});
    REXLOG_INFO("Native settings: renderer committed revision {} values={}",
        revision.id, settings::NativeSettingsLogValues(revision.values));
    return settings::RevisionResult{revision.id, settings::RevisionState::kComplete, {}};
  });
  return result;
}
void NativeSettingsRuntime::Presented(const std::shared_ptr<const NativeSettingsFrame>& frame,
    settings::PresentationObservation observation, bool effects_ready) {
  // UI independently verifies HWND geometry and the same presentation before
  // starting confirmation or allowing persistence.
  if (frame && effects_ready) coordinator_->AcknowledgePresentation(frame->ticket, observation);
}
void PublishNativeSettingsRuntime(std::shared_ptr<NativeSettingsRuntime> runtime) {
  if (!runtime) ClearNativeVisualEffects();
  published.store(std::move(runtime));
}
std::shared_ptr<NativeSettingsRuntime> AcquireNativeSettingsRuntime() { return published.load(); }
}  // namespace legodimensions::gpu_native
