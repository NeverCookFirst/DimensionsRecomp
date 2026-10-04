#include "window_mode.h"
#include <cassert>
#include <iostream>
struct Window {
  bool fullscreen = false;
  uint32_t width = 1280, height = 720, monitor = 3, changes = 0;
  bool IsFullscreen() const { return fullscreen; }
  uint32_t GetActualLogicalWidth() const { return width; }
  uint32_t GetActualLogicalHeight() const { return height; }
  void SetFullscreen(bool value) {
    fullscreen = value; ++changes;
    if (value) { width = 1920; height = 1080; }
  }
  void SetDesiredLogicalSize(uint32_t w, uint32_t h) { width=w; height=h; }
};
int main() {
  Window w; legodimensions::WindowedSize saved;
  assert(legodimensions::ToggleBorderless(w,saved));
  assert(w.fullscreen && w.monitor==3 && saved.width==1280 && saved.height==720);
  assert(!legodimensions::ToggleBorderless(w,saved));
  assert(!w.fullscreen && w.width==1280 && w.height==720 && w.monitor==3);
  w.width=900; w.height=600;
  legodimensions::ToggleBorderless(w,saved); legodimensions::ToggleBorderless(w,saved);
  assert(w.width==900 && w.height==600);
  w.fullscreen=true; saved={};
  legodimensions::ToggleBorderless(w,saved); assert(!w.fullscreen && w.width==900);
  w.width=0; w.height=0;
  legodimensions::ToggleBorderless(w,saved); assert(!saved.width && !saved.height);
  std::cout << "PASS: borderless toggle, windowed restoration, monitor retention, startup fullscreen, zero-size\n";
}
