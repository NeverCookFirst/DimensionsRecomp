#pragma once
#include <cstdint>

namespace legodimensions {
struct WindowedSize { uint32_t width = 0, height = 0; };

// SDL's SetFullscreen uses the window's current display and borderless mode.
// Remember the actual windowed size, rather than the fullscreen display size.
template<class Window>
bool ToggleBorderless(Window& window, WindowedSize& saved) {
  const bool fullscreen = !window.IsFullscreen();
  if (fullscreen) {
    const auto width = window.GetActualLogicalWidth();
    const auto height = window.GetActualLogicalHeight();
    if (width && height) saved = {width, height};
    window.SetFullscreen(true);
  } else {
    window.SetFullscreen(false);
    if (saved.width && saved.height)
      window.SetDesiredLogicalSize(saved.width, saved.height);
  }
  return fullscreen;
}
}  // namespace legodimensions
