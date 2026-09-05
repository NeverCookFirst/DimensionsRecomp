// legodimensions - ReXGlue Recompiled Project
//
// Project-specific look for every ImGui overlay: the LEGO Dimensions blue in
// place of the SDK's inherited green theme, and an optional TrueType font.
//
// Both live here rather than in the SDK because ReXApp exposes them as hooks
// (OnConfigureStyle, OnConfigureFonts) precisely so a game can restyle the UI
// without forking the shared code.

#pragma once

struct ImFontAtlas;
struct ImGuiStyle;

namespace rex::ui {
struct Style;
}  // namespace rex::ui

namespace legodimensions::ui {

// Repaints the ImGui palette and the SDK's per-overlay colours. Called from
// ReXApp::OnConfigureStyle after the SDK defaults have been applied.
void ConfigureStyle(ImGuiStyle& imgui_style, rex::ui::Style& ui_style);

// Loads the font named by the ui_font cvar and makes it the UI default, so the
// achievement toast, the achievements list and every overlay share it. Called
// from ReXApp::OnConfigureFonts, after the SDK registered its own default font
// and before the atlas is built. A missing file is not an error: the built-in
// font is kept and a line is logged.
void ConfigureFonts(ImFontAtlas* atlas);

}  // namespace legodimensions::ui
