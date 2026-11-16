// Keep the SDK's passive F3 statistics panel, with title dragging and passive gameplay input.
#pragma once
#include <algorithm>
#include <imgui.h>

namespace legodimensions::ui {
struct DebugPositionState {
  ImVec2 position{}, size{}, grab{};
  float title_height = 0;
  int frame = -10;
  bool dragging = false;
};
inline DebugPositionState debug_position;

inline bool DebugTitleContains(const ImVec2& mouse) {
  const auto& s = debug_position;
  return mouse.x >= s.position.x && mouse.x < s.position.x + s.size.x &&
      mouse.y >= s.position.y && mouse.y < s.position.y + s.title_height;
}

// Called by the statistics provider, inside the SDK Debug window's Begin/End.
// No SDK replacement, duplicate key binding or interactive ImGui window is used.
inline void UpdatePassiveDebugPosition() {
  auto& s = debug_position;
  const auto& io = ImGui::GetIO();
  if (s.frame < ImGui::GetFrameCount() - 1 || io.AppFocusLost ||
      !ImGui::IsMouseDown(ImGuiMouseButton_Left)) s.dragging = false;
  s.position = ImGui::GetWindowPos();
  s.size = ImGui::GetWindowSize();
  s.title_height = ImGui::GetFrameHeight();
  s.frame = ImGui::GetFrameCount();
  if (!io.AppFocusLost && !io.WantCaptureMouse && DebugTitleContains(io.MousePos) &&
      ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
    s.dragging = true;
    s.grab = ImVec2(io.MousePos.x - s.position.x, io.MousePos.y - s.position.y);
  }
  if (s.dragging) {
    const ImVec2 position(
        std::clamp(io.MousePos.x - s.grab.x, 0.0f, std::max(0.0f, io.DisplaySize.x - s.size.x)),
        std::clamp(io.MousePos.y - s.grab.y, 0.0f, std::max(0.0f, io.DisplaySize.y - s.title_height)));
    ImGui::SetWindowPos(position);
    s.position = position;
  }
}

inline bool DebugPositionUsesPointer() {
  const auto& io = ImGui::GetIO();
  const auto& s = debug_position;
  return s.frame >= ImGui::GetFrameCount() - 1 && !io.AppFocusLost &&
      (s.dragging || (!io.WantCaptureMouse && DebugTitleContains(io.MousePos) &&
                     ImGui::IsMouseClicked(ImGuiMouseButton_Left)));
}
}  // namespace legodimensions::ui
