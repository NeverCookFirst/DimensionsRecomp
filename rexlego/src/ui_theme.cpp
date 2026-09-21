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
#include <utility>
#include <vector>

#include <imgui.h>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/ui/overlay/settings_overlay.h>
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

// ---------------------------------------------------------------------------
// The F4 settings window, arranged for a player rather than for whoever wrote
// the cvars. Each page lists the flags it shows, under a name that says what
// the setting does; the tooltip carries the flag's own description unless a
// shorter one is given here. Everything a page does not mention stays
// reachable under Advanced, by its raw name, for bug reports and for us.
void ConfigureSettings() {
  using rex::ui::SettingsItem;
  using rex::ui::SettingsPage;
  using Choices = std::vector<std::pair<std::string, std::string>>;

  rex::ui::SettingsPresentation pres;

  pres.pages.push_back(SettingsPage{
      "Display",
      {
          {"fullscreen", "Fullscreen",
           "Borderless fullscreen on the chosen monitor. Alt+Enter switches at any time."},
          {"monitor", "Monitor",
           "Which monitor the game opens on: 0 = where the window was last, 1 = primary, "
           "2 = second, and so on."},
          {"windowed_width", "Window width", "Size of the window when not fullscreen."},
          {"windowed_height", "Window height", "Size of the window when not fullscreen."},
          {"resolution", "Render resolution",
           "The resolution the game itself renders at. 720p is what the console did; higher "
           "values sharpen everything but cost GPU time.",
           Choices{{"", "Default (720p)"},
                   {"720p", "720p"},
                   {"1080p", "1080p"},
                   {"1440p", "1440p"},
                   {"4k", "4K"}}},
          {"present_letterbox", "Letterbox other aspect ratios",
           "Keep the picture 16:9 with black bars instead of stretching it to the window."},
          {"pause_when_unfocused", "Pause when the window is in the background"},
          {"d3d12_allow_variable_refresh_rate_and_tearing", "Variable refresh rate",
           "Let a G-Sync / FreeSync monitor run at the game's pace. Off forces a fixed "
           "refresh; also turns off tearing."},
      }});

  pres.pages.push_back(SettingsPage{
      "Performance",
      {
          {"resolution_scale", "Resolution scale",
           "Renders the world at a multiple of the game's 720p. The biggest single cost on "
           "the GPU; drop it first when the frame rate suffers.",
           Choices{{"1", "1x (720p, as on console)"},
                   {"2", "2x (1440p)"},
                   {"3", "3x (2160p)"},
                   {"4", "4x (2880p)"}}},
          {"present_max_output_height", "Output resolution cap",
           "Upscale to at most this many lines and let the monitor stretch the rest. A 1080 "
           "cap on a 4K screen costs what a 1080p window costs.",
           Choices{{"0", "Off (whole window)"},
                   {"720", "720 lines"},
                   {"1080", "1080 lines"},
                   {"1440", "1440 lines"},
                   {"2160", "2160 lines"}}},
          {"depth_of_field", "Depth of field blur",
           "The game's background blur. Off skips the pass entirely: the picture stays sharp "
           "at every distance and the GPU does less."},
          {"anisotropic_override", "Anisotropic filtering",
           "Keeps textures sharp at oblique angles. Higher is prettier and slightly costlier.",
           Choices{{"-1", "Game default"},
                   {"0", "Off"},
                   {"1", "1x"},
                   {"2", "2x"},
                   {"3", "4x"},
                   {"4", "8x"},
                   {"5", "16x"}}},
          {"frame_rate", "Frame rate target",
           "How fast the game logic is paced: 30 as on the console, or 60.",
           Choices{{"30", "30 FPS (as on console)"}, {"60", "60 FPS"}}},
          {"framerate_limit", "Frame rate cap",
           "Hard cap in frames per second. 0 = no cap. Useful on a laptop, or when the fans "
           "are too loud in menus."},
          {"async_shader_compilation", "Compile shaders in the background",
           "Avoids the freeze when a new shader appears, at the price of a brief glitch on "
           "the object using it. Leave on unless you see artefacts."},
          {"store_shaders", "Keep a shader cache on disk",
           "Remembers compiled shaders between runs so the second play-through does not "
           "stutter where the first one did."},
      }});

  pres.pages.push_back(SettingsPage{
      "Graphics",
      {
          {"present_effect", "Upscaling filter",
           "How the rendered frame is scaled to the window.",
           Choices{{"bilinear", "Bilinear (fastest)"},
                   {"cas", "AMD CAS (sharpening)"},
                   {"fsr", "AMD FSR 1"},
                   {"fsr2", "AMD FSR 2"},
                   {"fsr3", "AMD FSR 3"}}},
          {"swap_post_effect", "Anti-aliasing",
           "Smooths jagged edges on the final frame.",
           Choices{{"none", "Off"}, {"fxaa", "FXAA"}, {"fxaa_extreme", "FXAA (strong)"}}},
          {"present_dither", "Dithering",
           "Hides colour banding in gradients such as skies."},
          {"gpu_backend", "Graphics API",
           "Which API the renderer uses. Direct3D 12 is the tested default on Windows; "
           "Vulkan is the fallback for GPUs that misbehave on it.",
           Choices{{"any", "Automatic"}, {"d3d12", "Direct3D 12"}, {"vulkan", "Vulkan"}}},
          {"d3d12_adapter", "Graphics card",
           "Index of the graphics adapter to use. -1 picks the first real GPU; on a laptop "
           "with two, 0 and 1 choose between them."},
      }});

  pres.pages.push_back(SettingsPage{
      "Audio",
      {
          {"audio_mute", "Mute"},
      }});

  pres.pages.push_back(SettingsPage{
      "Controls",
      {
          {"input_backend", "Controller driver",
           "SDL handles almost any controller; XInput is the Xbox-only driver Windows "
           "ships with.",
           Choices{{"sdl", "SDL (any controller)"}, {"xinput", "XInput (Xbox controllers)"}}},
          {"player_slot", "Keyboard controls player",
           "Which player the keyboard and mouse stand in for.",
           Choices{{"1", "Player 1"}, {"2", "Player 2"}, {"3", "Player 3"}, {"4", "Player 4"}}},
          {"mnk_mode", "Keyboard and mouse as a controller",
           "Lets the keyboard play as a controller using the bindings on the next page."},
          {"mnk_mouse", "Mouse moves the camera",
           "Use the mouse as the right stick. Off leaves the camera to the keys bound to "
           "it."},
          {"mnk_sensitivity", "Mouse sensitivity"},
          {"guide_button", "Pass the Guide button to the game"},
      }});

  // Every controller key, labelled by the button it stands in for.
  SettingsPage keys{"Keyboard bindings", {}};
  for (const auto& entry : rex::cvar::GetRegistry()) {
    if (entry.name.rfind("keybind_", 0) == 0) {
      keys.items.push_back(
          {entry.name, entry.description.empty() ? entry.name : entry.description});
    }
  }
  pres.pages.push_back(std::move(keys));

  pres.pages.push_back(SettingsPage{
      "Shortcuts",
      {
          {"bind_settings", "Settings menu"},
          {"bind_fullscreen", "Fullscreen on / off"},
          {"bind_debug_overlay", "Frame rate and debug overlay"},
          {"bind_achievements", "Achievements"},
          {"bind_mods", "Mod menu"},
          {"bind_cheats", "Cheat menu"},
          {"bind_console", "Console"},
      }});

  pres.pages.push_back(SettingsPage{
      "Toy Pad",
      {
          {"toypad_emulation", "Emulated Toy Pad",
           "Use the built-in Toy Pad (with the companion app) instead of a real one plugged "
           "in over USB."},
          {"toypad_app_autostart", "Open the Toy Pad app with the game"},
          {"toypad_app_path", "Toy Pad app location",
           "Where LegoToypad.exe is. Empty means tools\\LegoToypad next to the game."},
          {"toypad_passthrough_frame", "Real Toy Pad protocol",
           "How commands are sent to a physical pad. Automatic tells the Xbox 360, Xbox "
           "One and PC / PS3 pads apart on its own.",
           Choices{{"auto", "Automatic"},
                   {"keep", "Xbox 360 framing"},
                   {"strip", "PC / PS3 framing"}}},
      }});

  pres.pages.push_back(SettingsPage{
      "Mods and fixes",
      {
          {"mods", "Enabled mods",
           "Comma-separated folder names under the mods folder. The mod menu (F8) edits "
           "this for you."},
          {"mods_root", "Mods folder"},
          {"fix_portal_trailer", "Fix the Mystery Dimension portal",
           "Makes the portal in Vorton work. Without it the character walks in and is "
           "stuck forever."},
          {"fix_hub_portrait", "3D character portrait everywhere (experimental)",
           "Shows the animated portrait in the hub and open worlds, not only inside "
           "levels."},
      }});

  pres.pages.push_back(SettingsPage{
      "Online",
      {
          {"discord_rpc", "Discord Rich Presence",
           "Show what you are playing in Discord."},
          {"updates_check", "Check for updates at start"},
      }});

  pres.pages.push_back(SettingsPage{
      "Interface",
      {
          {"ui_font_size", "Menu font size"},
      }});

  // Locked to off, and its description says it is not what people think it
  // is; showing it only invites the question.
  pres.hidden = {"vsync"};

  rex::ui::SettingsDialog::SetPresentation(std::move(pres));
}

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
