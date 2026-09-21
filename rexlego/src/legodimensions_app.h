// legodimensions - ReXGlue Recompiled Project
//
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include <chrono>
#include <cstdlib>
#include <mutex>
#include <thread>

#include <rex/logging.h>
#include <rex/rex_app.h>
#include <rex/ui/keybinds.h>

#include "cheat_menu.h"
#include "discord_presence.h"
#ifdef LEGODIMENSIONS_DEV_PROBES
#include "area_watch.h"
#endif
#ifdef LEGODIMENSIONS_DEV_PROBES
#include "hub_portrait.h"
#endif
#include "mod_menu.h"
#include "toypad_app.h"
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
#ifdef LEGODIMENSIONS_DEV_PROBES
    // Guest functions are registered by now, which is what the patch needs.
    legodimensions::hub_portrait::Install();
    // Must come before the presence starts: the presence asks it where the
    // player is, and it can only answer once the accessor is wrapped.
    legodimensions::area_watch::Install();
#endif
    legodimensions::cheats::InstallGraphicsToggles();
    legodimensions::discord::Start();
    legodimensions::updates::CheckAtStartup();
    legodimensions::toypad_app::StartIfEnabled();
  }

  // The window closing is where the app must be let go: ReXApp::OnClosing then
  // hard-exits with std::_Exit, so OnShutdown never runs. This asks nicely; the
  // job object is the backstop if the game dies without getting here.
  // The watchdog covers the gap before ReXApp::OnClosing's hard-exit. That
  // hard-exit exists precisely because subsystem teardown can deadlock, but the
  // window layer between "close requested" and OnClosing can wedge too, and
  // then the process never dies and the player is left killing it by hand.
  // Give it three seconds and take the same way out.
  bool OnWindowCloseRequested() override {
    legodimensions::toypad_app::StopIfStarted();
    StartShutdownWatchdog();
    return true;
  }

  // Dropping the pipe is what clears the presence; Discord does the rest.
  void OnShutdown() override {
    legodimensions::discord::Stop();
    legodimensions::toypad_app::StopIfStarted();
    // Drops the freeze thread before the guest memory it writes into goes away.
    legodimensions::cheats::Shutdown();
  }

  // The SDK inherits xenia's green ImGui theme; both hooks exist so a game can
  // restyle the overlays without forking the shared UI code.
  void OnConfigureStyle(ImGuiStyle& imgui_style, rex::ui::Style& ui_style) override {
    legodimensions::ui::ConfigureStyle(imgui_style, ui_style);
    legodimensions::ui::ConfigureSettings();
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
    // DEL opens the cheat menu. Only the dialog is thrown away on close - the
    // scanner and any frozen values live on in the engine behind it.
    rex::ui::RegisterBind("bind_cheats", "Delete", "Toggle cheat menu", [this, drawer] {
      if (cheat_menu_) {
        cheat_menu_.reset();
      } else {
        cheat_menu_ = legodimensions::cheats::CreateMenu(drawer);
      }
    });
  }

 private:
  // Last-resort exit if the close path wedges. Detached on purpose: if the
  // process gets where it is going first, _Exit takes this thread with it.
  static void StartShutdownWatchdog() {
    static std::once_flag once;
    std::call_once(once, [] {
      std::thread([] {
        std::this_thread::sleep_for(std::chrono::seconds(3));
        REXLOG_WARN("Shutdown wedged past the close request; hard-exiting.");
        rex::FlushLogging();
        std::_Exit(0);
      }).detach();
    });
  }

  std::unique_ptr<rex::ui::ImGuiDialog> mod_menu_;
  std::unique_ptr<rex::ui::ImGuiDialog> cheat_menu_;

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
