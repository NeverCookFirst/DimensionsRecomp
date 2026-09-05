// legodimensions - ReXGlue Recompiled Project
//
// See ui_theme.h. The SDK inherits xenia's green ImGui theme, which is what
// tints every overlay border, title bar and button behind F3 and F4. Rather
// than patch the shared default, the whole palette is re-derived here from one
// accent colour.

#include "ui_theme.h"

#include <filesystem>
#include <string>
#include <system_error>

#include <imgui.h>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/ui/style.h>

// Fira Sans stands in for the dashboard's Convection, which is proprietary and
// cannot ship here. Both are humanist sans faces of the Frutiger lineage, and
// Fira is under the SIL Open Font License, so it can live in the repo. Point
// this at ConvectionRegular.ttf instead if a legally obtained copy is at hand.
REXCVAR_DEFINE_STRING(ui_font, "FiraSans-Regular.ttf", "UI",
                      "TrueType font for all overlays. Relative paths resolve against the "
                      "executable folder. Empty, or a missing file, keeps the built-in font.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_DOUBLE(ui_font_size, 18.0, "UI", "Size in pixels for ui_font")
    .range(8.0, 72.0)
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace legodimensions::ui {
namespace {

// LEGO Dimensions blue, #00BDFE.
constexpr float kAccentR = 0.000f;
constexpr float kAccentG = 0.741f;
constexpr float kAccentB = 0.996f;

// The SDK's palette is built almost entirely from pure greens of varying
// intensity, so each one maps onto the accent by reusing its intensity. That
// keeps the original contrast relationships - which entry reads brighter than
// which - instead of hand-picking twenty unrelated blues.
ImVec4 Accent(float intensity, float alpha = 1.0f) {
  return ImVec4(kAccentR * intensity, kAccentG * intensity, kAccentB * intensity, alpha);
}

// A lifted variant for text and other elements that must stay legible against
// the dark background rather than scale toward black.
ImVec4 AccentText(float lift, float alpha = 1.0f) {
  return ImVec4(kAccentR + (1.0f - kAccentR) * lift, kAccentG + (1.0f - kAccentG) * lift,
                kAccentB + (1.0f - kAccentB) * lift, alpha);
}

}  // namespace

void ConfigureStyle(ImGuiStyle& style, rex::ui::Style& ui_style) {
  ImVec4* colors = style.Colors;

  colors[ImGuiCol_WindowBg] = ImVec4(0.00f, 0.03f, 0.05f, 1.00f);
  colors[ImGuiCol_Border] = Accent(0.35f);
  colors[ImGuiCol_TitleBg] = Accent(0.30f);
  colors[ImGuiCol_TitleBgCollapsed] = Accent(0.24f);
  colors[ImGuiCol_TitleBgActive] = Accent(0.52f);
  colors[ImGuiCol_MenuBarBg] = Accent(0.28f);

  colors[ImGuiCol_ScrollbarBg] = Accent(0.26f, 0.59f);
  colors[ImGuiCol_ScrollbarGrab] = Accent(0.55f, 0.68f);
  colors[ImGuiCol_ScrollbarGrabHovered] = Accent(0.80f, 0.62f);
  colors[ImGuiCol_ScrollbarGrabActive] = Accent(0.72f, 0.40f);

  colors[ImGuiCol_CheckMark] = AccentText(0.35f);
  colors[ImGuiCol_SliderGrabActive] = Accent(0.72f);

  colors[ImGuiCol_Button] = Accent(0.42f, 0.60f);
  colors[ImGuiCol_ButtonHovered] = Accent(0.62f);
  colors[ImGuiCol_ButtonActive] = Accent(0.50f);

  colors[ImGuiCol_Header] = Accent(0.32f, 0.71f);
  colors[ImGuiCol_HeaderHovered] = Accent(0.50f, 0.80f);
  colors[ImGuiCol_HeaderActive] = Accent(0.64f, 0.80f);

  colors[ImGuiCol_Separator] = Accent(0.35f);
  colors[ImGuiCol_SeparatorHovered] = AccentText(0.30f);
  colors[ImGuiCol_SeparatorActive] = Accent(0.44f);

  colors[ImGuiCol_TextSelectedBg] = Accent(0.85f, 0.21f);

  // Tabs follow the buttons in the SDK default, so re-copy them here or they
  // keep the green they were assigned before this hook ran.
  colors[ImGuiCol_Tab] = colors[ImGuiCol_Button];
  colors[ImGuiCol_TabHovered] = colors[ImGuiCol_ButtonHovered];
  colors[ImGuiCol_TabActive] = colors[ImGuiCol_ButtonActive];

  // Per-overlay colours the ImGui palette cannot express.
  //
  // AchievementsStyle is deliberately left at the SDK defaults: green there is
  // not chrome, it is the "unlocked" state, and the user wants it to keep
  // reading as such. The accent recolours the interface, not status.
  //
  // The same logic keeps the gamerscore gold, the warning amber and the error
  // reds untouched. lifecycle_live is chrome rather than status - it labels a
  // cvar as hot-reloadable - so it does follow the accent.
  ui_style.settings.lifecycle_live = AccentText(0.30f);
}

void ConfigureFonts(ImFontAtlas* atlas) {
  const std::string configured = REXCVAR_GET(ui_font);
  if (configured.empty()) {
    return;
  }

  std::filesystem::path path(configured);
  if (path.is_relative()) {
    path = rex::filesystem::GetExecutableFolder() / path;
  }

  std::error_code ec;
  if (!std::filesystem::exists(path, ec)) {
    // Convection is proprietary and cannot ship with the project, so its
    // absence is the normal case rather than a fault worth shouting about.
    REXLOG_INFO("UI font '{}' not found, keeping the built-in font", path.string());
    return;
  }

  ImFont* font =
      atlas->AddFontFromFileTTF(path.string().c_str(), float(REXCVAR_GET(ui_font_size)));
  if (!font) {
    REXLOG_WARN("UI font '{}' failed to load, keeping the built-in font", path.string());
    return;
  }

  // The SDK registered its own font first, so this one would otherwise just be
  // an extra entry in the atlas. Making it the default is what carries it into
  // the achievements list and every other overlay.
  ImGui::GetIO().FontDefault = font;
  REXLOG_INFO("UI font loaded: {} at {:.0f}px", path.string(), REXCVAR_GET(ui_font_size));
}

}  // namespace legodimensions::ui
