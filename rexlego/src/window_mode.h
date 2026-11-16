#pragma once
#include <cstdint>
#include <utility>

namespace legodimensions {
struct WindowedSize { uint32_t width = 0, height = 0; };

// Call on the platform UI thread. Capture before the SDK's registry callback
// changes the window, so menu Apply/rollback and Alt+Enter share one policy.
// Request returns registry acceptance, not physical window completion. A
// partially failing/throwing callback may have changed the desired state;
// retain its rollback size and restore geometry when it did leave fullscreen.
template<class Window, class Request>
bool RequestBorderless(Window& window, WindowedSize& saved, bool fullscreen,
                       Request&& request) {
  const bool was_fullscreen = window.IsFullscreen();
  if (!was_fullscreen && fullscreen) {
    const auto width = window.GetActualLogicalWidth();
    const auto height = window.GetActualLogicalHeight();
    if (width && height) saved = {width, height};
  }
  const auto restore_size = [&] {
    if (was_fullscreen && !fullscreen && !window.IsFullscreen() && saved.width && saved.height)
      window.SetDesiredLogicalSize(saved.width, saved.height);
  };
  bool accepted;
  try {
    accepted = std::forward<Request>(request)(fullscreen);
  } catch (...) {
    restore_size();
    throw;
  }
  restore_size();
  return accepted;
}

// Direct window-only helper for callers without a registry callback.
template<class Window>
bool ToggleBorderless(Window& window, WindowedSize& saved) {
  const bool fullscreen = !window.IsFullscreen();
  RequestBorderless(window, saved, fullscreen, [&](bool value) {
    window.SetFullscreen(value);
    return true;
  });
  return fullscreen;
}
}  // namespace legodimensions
