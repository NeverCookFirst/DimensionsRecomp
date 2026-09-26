// legodimensions - ReXGlue Recompiled Project
//
// Two entries the Xbox 360 main menu hides: the top-left slot becomes Credits
// and the middle-left one Quit Game. Both already exist in the menu layout
// (gui\gui3\layouts\mainmenu.xml, codes Help and Quit); the title just switches
// them off on this platform. The hooks switch them back on, relabel them, and
// route their selection here instead of to the title's own handlers.

#pragma once

#include <cstdint>
#include <functional>
#include <memory>

namespace rex::ui {
class ImGuiDialog;
class ImGuiDrawer;
}  // namespace rex::ui

namespace legodimensions::main_menu {

struct Host {
  // Withholds the game's input while a prompt is up, so the menu underneath
  // does not react to the same presses.
  std::function<void(bool paused)> set_paused;
  // Buttons held on any controller (X_INPUT_GAMEPAD_* bits), 0 if none.
  std::function<uint16_t()> pad_buttons;
  // Called on "Yes" to quitting; does not return.
  std::function<void()> quit;
};

// Always alive; draws nothing until the menu asks for Credits or Quit.
std::unique_ptr<rex::ui::ImGuiDialog> CreateOverlay(rex::ui::ImGuiDrawer* drawer, Host host);

}  // namespace legodimensions::main_menu
