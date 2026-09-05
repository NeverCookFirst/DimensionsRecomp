// legodimensions - ReXGlue Recompiled Project
//
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include <rex/rex_app.h>
#include <rex/ui/keybinds.h>

#include "discord_presence.h"
#include "mod_menu.h"
#include "ui_theme.h"
#include "updates.h"

class LegodimensionsApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<LegodimensionsApp>(new LegodimensionsApp(ctx, "legodimensions",
        PPCImageConfig));
  }

  // Publish Discord Rich Presence for as long as the game is up. Started here
  // rather than earlier so a Discord that is not running yet costs nothing:
  // the connection lives on its own thread and simply keeps retrying.
  // The update check goes out here too: by this point the window is up, so a
  // slow network cannot be mistaken for a slow launch. It costs nothing when
  // updates_check is off, which is the point of the setting.
  void OnPostSetup() override {
    legodimensions::discord::Start();
    legodimensions::updates::CheckAtStartup();
  }

  // Dropping the pipe is what clears the presence; Discord does the rest.
  void OnShutdown() override { legodimensions::discord::Stop(); }

  // The SDK inherits xenia's green ImGui theme; both hooks exist so a game can
  // restyle the overlays without forking the shared UI code.
  void OnConfigureStyle(ImGuiStyle& imgui_style, rex::ui::Style& ui_style) override {
    legodimensions::ui::ConfigureStyle(imgui_style, ui_style);
  }

  void OnConfigureFonts(ImFontAtlas* atlas) override {
    legodimensions::ui::ConfigureFonts(atlas);
  }

  // Runs before the runtime is built, which is the only point where the update
  // folder can still be swapped for the modded copy.
  void OnConfigurePaths(rex::PathConfig& paths) override {
    legodimensions::mods::ResolveUpdateRoot(paths);
  }

  // F8 mirrors how the SDK toggles its own overlays: the bind owns the dialog
  // and destroying it closes the menu.
  void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {
    rex::ui::RegisterBind("bind_mods", "F8", "Toggle mod menu", [this, drawer] {
      if (mod_menu_) {
        mod_menu_.reset();
      } else {
        mod_menu_ = legodimensions::mods::CreateMenu(drawer);
      }
    });
  }

 private:
  std::unique_ptr<rex::ui::ImGuiDialog> mod_menu_;

 public:
  // Override virtual hooks for customization:
  // void OnPostInitLogging() override {}
  // void OnPreSetup(rex::RuntimeConfig& config) override {}
  // void OnLoadXexImage(std::string& xex_image) override {}
  // void OnPostLoadXexImage() override {}
  // void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {}
  // std::unique_ptr<rex::ui::ImGuiDialog> CreateAchievementsOverlay() override;
  // std::unique_ptr<rex::ui::AchievementNotificationDialog>
  // CreateAchievementNotificationDialog() override;
  // void OnConfigurePaths(rex::PathConfig& paths) override {}
};
