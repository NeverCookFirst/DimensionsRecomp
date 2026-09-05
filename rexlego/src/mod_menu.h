// legodimensions - ReXGlue Recompiled Project
//
// In-game mod manager, bound to F8.
//
// Mods are DAT injections: their payload is written into PATCH.DAT, which the
// game opens at startup and reads from for the rest of the session. Patching
// that file while it is in use is not something to attempt, so the menu edits
// the selection and applies it to a modded copy of the update folder, and the
// change takes effect on the next launch. That is also why the enabled set is
// stored in the config rather than kept in memory.

#pragma once

#include <memory>

namespace rex {
struct PathConfig;
namespace ui {
class ImGuiDialog;
class ImGuiDrawer;
}  // namespace ui
}  // namespace rex

namespace legodimensions::mods {

// Creates the F8 dialog. The caller owns it; destroying it closes the menu,
// which is how the SDK's own overlays are toggled.
std::unique_ptr<rex::ui::ImGuiDialog> CreateMenu(rex::ui::ImGuiDrawer* drawer);

// Points the update folder at the modded copy when any mod is enabled. Call
// from ReXApp::OnConfigurePaths, which runs before the runtime is built.
void ResolveUpdateRoot(rex::PathConfig& paths);

}  // namespace legodimensions::mods
