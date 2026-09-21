// legodimensions - ReXGlue Recompiled Project
//
// The DEL overlay: memory scanner, value freezer and saved cheat list.
// The engine behind it is in cheat_engine.h.

#pragma once

#include <memory>

namespace rex::ui {
class ImGuiDialog;
class ImGuiDrawer;
}  // namespace rex::ui

namespace legodimensions::cheats {

// Creates the DEL dialog. The caller owns it; destroying it closes the menu,
// the same way the mod menu and the SDK overlays are toggled. The scanner
// state lives past that in a process-wide engine, so closing and reopening
// does not throw away a scan in progress.
std::unique_ptr<rex::ui::ImGuiDialog> CreateMenu(rex::ui::ImGuiDrawer* drawer);

// Drops the engine and its freeze thread. Called on shutdown so the freeze
// thread is not still writing into guest memory as it is torn down.
void Shutdown();

// Registers the depth_of_field setting's effect and applies the saved value.
// Call once the GPU cvars exist, i.e. after setup.
void InstallGraphicsToggles();

}  // namespace legodimensions::cheats
