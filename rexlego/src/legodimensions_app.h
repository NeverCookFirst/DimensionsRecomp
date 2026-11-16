// legodimensions - ReXGlue Recompiled Project
//
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include <chrono>
#include <cstdlib>
#include <mutex>
#include <thread>

#include <rex/logging.h>
#include <rex/input/input_system.h>
#include <rex/input/mnk/mnk_input_driver.h>
#include <rex/rex_app.h>
#include <rex/runtime.h>
#include <rex/cvar.h>
#include <rex/ui/flags.h>
#include <rex/ui/keybinds.h>

#include "cheat_menu.h"
#include "discord_presence.h"
#ifdef LEGODIMENSIONS_DEV_PROBES
#include "area_watch.h"
#endif
#ifdef LEGODIMENSIONS_DEV_PROBES
#include "hub_portrait.h"
#endif
#include "main_menu_extras.h"
#include "native_process_exit.h"
#include "mod_menu.h"
#include "skip_cutscenes.h"
#include "toypad_app.h"
#include "ui_theme.h"
#include "updates.h"
#include "window_mode.h"
#ifdef LEGODIMENSIONS_NATIVE_GPU
#include "settings_registry.h"
#include "native_debug_position.h"
#include "settings_service.h"
#include "settings_preferences.h"
#include "gpu_native/fsr3_settings.h"
#include "gpu_native/settings_runtime.h"
#include "gpu_native/device.h"
#include "gpu_native/native_overlay.h"
#include "gpu_native/crash_reporter.h"
#include <rex/ui/immediate_drawer.h>
#include <rex/ui/overlay/debug_overlay.h>
#include "gpu_native/renderer_mode.h"
#if defined(_WIN32)
#include <windows.h>
#endif
#endif

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
#ifdef LEGODIMENSIONS_NATIVE_GPU
#if defined(_WIN32)
    if (window() && !window()->IsFullscreen()) {
      const auto handle = static_cast<HWND>(window()->GetNativeWindowHandle());
      if (handle) ShowWindow(handle, SW_MAXIMIZE);
    }
#endif
    // The SDK installs its default counter provider after OnCreateDialogs.
    // Detached native rendering must replace it after that initialization.
    if (!legodimensions::gpu_native::UsePm4Reference())
      SetGuestFrameStats([] {
        legodimensions::ui::UpdatePassiveDebugPosition();
        return legodimensions::gpu_native::NativeOverlayStats();
      });
#endif
#ifdef LEGODIMENSIONS_DEV_PROBES
    // Guest functions are registered by now, which is what the patch needs.
    legodimensions::hub_portrait::Install();
    // Must come before the presence starts: the presence asks it where the
    // player is, and it can only answer once the accessor is wrapped.
    legodimensions::area_watch::Install();
#endif
    legodimensions::cheats::InstallGraphicsToggles();
    legodimensions::skip_cutscenes::Start();
    legodimensions::discord::Start();
    legodimensions::updates::CheckAtStartup([] {
      legodimensions::toypad_app::StopIfStarted();
      rex::FlushLogging();
      std::_Exit(0);
    });
    legodimensions::toypad_app::StartIfEnabled();
  }

#ifdef LEGODIMENSIONS_NATIVE_GPU
  // Detached renderer mode: the SDK still owns the window/input/audio, while
  // the title's D3D calls are handled by gpu_native instead of rexgpu-xenos.
  void OnPreSetup(rex::RuntimeConfig& config) override {
    REXLOG_INFO("GPU route: direct native D3D-to-RHI; PM4 disabled");
    if (legodimensions::gpu_native::UsePm4Reference()) {
      if (config.gpu_plugin.empty()) config.gpu_plugin = "xenos";
      return;
    }
    config.graphics.reset();
    config.gpu_plugin.clear();
  }

  std::unique_ptr<rex::ui::ImmediateDrawer> OnCreateImmediateDrawer() override {
    return legodimensions::gpu_native::CreateNativeImmediateDrawer();
  }

  void OnPreLaunchModule() override {
    if (legodimensions::gpu_native::UsePm4Reference()) return;
    StopNativeSettings();
    legodimensions::gpu_native::PublishNativeSettingsRuntime({});
    std::error_code preferences_error;
    std::filesystem::create_directories(native_preferences_path_.parent_path(), preferences_error);
    auto settings_backend = legodimensions::settings::MakeNativeRegistryBackend(native_preferences_path_, {},
        [this](std::string_view key, std::string_view value) {
          if (key == "fullscreen") return RequestFullscreen(value == "true");
          return rex::cvar::SetFlagByName(key, value);
        });
    auto preferences = legodimensions::settings::LoadNativePreferences(native_preferences_path_, settings_backend);
    if (preferences_error) {
      preferences.ok = false;
      preferences.error += " Could not prepare the native settings folder: " + preferences_error.message();
    }
    if (preferences.recovery_failed) {
      REXLOG_ERROR("Native preferences recovery failed: {}", preferences.error);
      app_context().QuitFromUIThread();
      return;
    }
    if (!preferences.ok) REXLOG_WARN("Native saved settings: {}", preferences.error);
    else if (!preferences.applied.empty())
      REXLOG_INFO("Restored {} committed native settings from user data", preferences.applied.size());
    // No guest draw can run yet. Refresh any settings snapshot created by an
    // early overlay query before choosing native surfaces and FSR resources.
    legodimensions::gpu_native::ApplyNativeFsr3Settings();
    if (!legodimensions::gpu_native::HostDevice::Create(window())) {
      REXLOG_ERROR("Native GPU initialization failed; stopping before guest launch");
      app_context().QuitFromUIThread();
      return;
    }
    auto* context = &app_context();
    if (rex::cvar::Query<bool>("native_settings_frontend")) {
      native_settings_runtime_ = std::make_shared<legodimensions::gpu_native::NativeSettingsRuntime>(
          std::move(settings_backend), window());
      settings_backend = native_settings_runtime_->MakeBackend();
      legodimensions::gpu_native::PublishNativeSettingsRuntime(native_settings_runtime_);
    }
    native_settings_ = std::make_shared<legodimensions::settings::QueuedSettings>(
        std::move(settings_backend),
        [context](std::function<void()> command) {
          return context->CallInUIThreadDeferred(std::move(command));
        }, legodimensions::settings::NativeSettingsDefaults(),
        legodimensions::settings::NativeSettingsConfirmationKeys());
    if (!preferences.ok) native_settings_->SetStartupErrorFromUI(preferences.error);
    legodimensions::settings::PublishNativeSettingsService(native_settings_);
  }
#endif

  // The window closing is where the app must be let go: ReXApp::OnClosing then
  // hard-exits with std::_Exit, so OnShutdown never runs. This asks nicely; the
  // job object is the backstop if the game dies without getting here.
  // The watchdog covers the gap before ReXApp::OnClosing's hard-exit. That
  // hard-exit exists precisely because subsystem teardown can deadlock, but the
  // window layer between "close requested" and OnClosing can wedge too, and
  // then the process never dies and the player is left killing it by hand.
  // Native Windows close requests have a short whole-process backstop. The
  // normal path still gets the first chance to stop settings and companions.
  bool OnWindowCloseRequested() override {
    // Arm the backstop before any cleanup that may encounter a busy subsystem.
    StartShutdownWatchdog();
#ifdef LEGODIMENSIONS_NATIVE_GPU
    StopNativeSettings(true);
    legodimensions::gpu_native::DetachNativeOverlays();
#endif
    legodimensions::toypad_app::StopIfStarted();
    return true;
  }

  // Dropping the pipe is what clears the presence; Discord does the rest.
  void OnShutdown() override {
#ifdef LEGODIMENSIONS_NATIVE_GPU
    StopNativeSettings();
    legodimensions::gpu_native::DetachNativeOverlays();
#endif
#ifdef LEGODIMENSIONS_NATIVE_GPU
    if (!legodimensions::gpu_native::UsePm4Reference())
      legodimensions::gpu_native::HostDevice::Shutdown();
    legodimensions::gpu_native::PublishNativeSettingsRuntime({});
#endif
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
    legodimensions::updates::RelocateMovedInstall(paths);
    legodimensions::mods::ResolveUpdateRoot(paths);
#ifdef LEGODIMENSIONS_NATIVE_GPU
    native_preferences_path_ = legodimensions::settings::NativePreferencesPath(paths.user_data_root);
#endif
  }

  void OnWindowFocusChanged(bool focused) override {
#ifdef LEGODIMENSIONS_NATIVE_GPU
    if (!focused && native_settings_) native_settings_->FocusLostFromUI();
#else
    (void)focused;
#endif
  }

  // F8 mirrors how the SDK toggles its own overlays: the bind owns the dialog
  // and destroying it closes the menu.
  void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {
#ifdef LEGODIMENSIONS_NATIVE_GPU
    if (!legodimensions::gpu_native::UsePm4Reference()) {
      legodimensions::gpu_native::AttachNativeOverlays(app_context(), drawer);
      SetGuestFrameStats([] {
        legodimensions::ui::UpdatePassiveDebugPosition();
        return legodimensions::gpu_native::NativeOverlayStats();
      });
      if (!rex::cvar::Query<bool>("native_settings_frontend"))
        rex::ui::RegisterBind("bind_native_graphics", "F5", "Toggle native graphics menu", [this,drawer] {
          if (native_graphics_menu_) native_graphics_menu_.reset();
          else native_graphics_menu_ = legodimensions::gpu_native::CreateNativeGraphicsMenu(drawer);
        });
    }
#endif
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
    // Credits and Quit Game in the main menu. Always up; it draws only when
    // one of the two entries is picked.
    legodimensions::main_menu::Host host;
    // The game keeps running under the prompt; only its input is withheld.
    // Suspending its threads crashed 0.1.21 for players: a thread frozen
    // inside the input system's own device refresh, and this overlay polling
    // the same unlocked state from the UI thread.
    host.set_paused = [](bool paused) { rex::input::InputSystem::SetGuestInputBlocked(paused); };
    host.pad_buttons = []() -> uint16_t { return rex::input::InputSystem::HeldButtons(); };
    host.native_quit = [] { legodimensions::ExitNativeGameNow(); };
    host.quit = [this] {
      // Use the same listener/TerminateTitle path as the window's close button.
      // Exiting the UI loop directly reaches OnDestroy's guest-thread join,
      // which the owner V20 dump shows deadlocking on a guest critical section.
      if (OnWindowCloseRequested()) {
        if (auto* win = window()) win->RequestClose();
      }
    };
    // The cursor is only for the overlays: hidden over the game in a window
    // and in fullscreen alike, shown while any menu is open.
    host.on_frame = [this, drawer](bool prompt_open) {
#ifdef LEGODIMENSIONS_NATIVE_GPU
      if (native_settings_runtime_) native_settings_runtime_->AdvanceFromUI();
      if (native_settings_) native_settings_->AdvanceFromUI();
#endif
      auto* win = window();
      if (!win) {
        return;
      }
      // F3 is a passive NoInputs window. Overlay existence alone must not
      // suppress the keyboard/controller; interactive SDK windows advertise
      // capture through ImGui, while our own modal dialogs have explicit owners.
      const auto& input = drawer->GetIO();
      const bool sdk_overlay = AnyOverlayOpen();
      const bool sdk_input = sdk_overlay && (input.WantCaptureMouse || input.WantCaptureKeyboard);
      const bool menu_open = prompt_open || sdk_input || mod_menu_ || cheat_menu_
#ifdef LEGODIMENSIONS_NATIVE_GPU
          || native_graphics_menu_
          || legodimensions::ui::DebugPositionUsesPointer()
#endif
          ;
#ifdef LEGODIMENSIONS_NATIVE_GPU
      if (!legodimensions::gpu_native::UsePm4Reference())
        rex::input::InputSystem::SetGuestInputBlocked(menu_open);
      static int previous_input_gate = -1;
      if (previous_input_gate != int(menu_open)) {
        previous_input_gate = int(menu_open);
        REXLOG_INFO("Native input gate: blocked={} sdk_capture={} prompt={} mods={} cheats={}",
            menu_open, sdk_input, prompt_open, bool(mod_menu_), bool(cheat_menu_));
      }
#endif
      // Visibility follows overlay lifetime, not hover/capture. F3 remains
      // passive for keyboard/controller while the pointer can reach its title.
      const bool cursor_visible = menu_open || sdk_overlay;
      if (cursor_visible && !overlay_pointer_active_) {
        mouse_look_before_overlay_ = rex::input::mnk::IsMouseLookActive();
        rex::input::mnk::SetMouseLookActive(false);
        overlay_pointer_active_ = true;
      } else if (!cursor_visible && overlay_pointer_active_) {
        rex::input::mnk::SetMouseLookActive(mouse_look_before_overlay_);
        overlay_pointer_active_ = false;
      }
      win->SetCursorVisibility(cursor_visible ? rex::ui::Window::CursorVisibility::kVisible
                                         : rex::ui::Window::CursorVisibility::kHidden);
    };
    main_menu_ = legodimensions::main_menu::CreateOverlay(drawer, std::move(host));
  }

  // Handle the convention in the title even with an older deployed SDK bind.
  // Consume repeats so holding the chord cannot resize every keyboard repeat.
  void OnKeyDown(rex::ui::KeyEvent& e) override {
    static const bool input_audit = std::getenv("LEGO_NATIVE_INPUT_AUDIT") != nullptr
#ifdef LEGODIMENSIONS_NATIVE_GPU
        || rex::cvar::Query<bool>("native_settings_frontend")
#endif
        ;
    static unsigned input_logs = 0;
    if (input_audit && input_logs++ < 32)
      REXLOG_INFO("Native input audit: key={} window_focus={} previous={} alt={}",
          static_cast<unsigned>(e.virtual_key()), window() && window()->HasFocus(),
          e.prev_state(), e.is_alt_pressed());
    if (e.virtual_key() == rex::ui::VirtualKey::kReturn && e.is_alt_pressed() &&
        !e.is_ctrl_pressed() && !e.is_shift_pressed() && !e.is_super_pressed()) {
      if (!e.prev_state() && window()) {
        if (!RequestFullscreen(!window()->IsFullscreen()))
          REXLOG_WARN("Could not request the fullscreen setting");
      }
      e.set_handled(true);
      return;
    }
    rex::ui::ProcessKeyEvent(e);
  }

 private:
  bool overlay_pointer_active_ = false;
  bool mouse_look_before_overlay_ = false;

  bool RequestFullscreen(bool fullscreen) {
    auto* win = window();
    if (!win) return false;
    return legodimensions::RequestBorderless(*win, windowed_size_, fullscreen, [](bool value) {
      return rex::cvar::SetFlagByName("fullscreen", value ? "true" : "false");
    });
  }
#ifdef LEGODIMENSIONS_NATIVE_GPU
  std::unique_ptr<rex::ui::ImGuiDialog> native_graphics_menu_;
  std::shared_ptr<legodimensions::settings::QueuedSettings> native_settings_;
  std::shared_ptr<legodimensions::gpu_native::NativeSettingsRuntime> native_settings_runtime_;
  std::filesystem::path native_preferences_path_;
  void StopNativeSettings(bool closing = false) {
    legodimensions::settings::PublishNativeSettingsService({});
    if (native_settings_) {
      const auto stopped = native_settings_->ShutdownFromUI();
      if (!stopped.ok) REXLOG_WARN("Native settings shutdown: {}", stopped.error);
      native_settings_.reset();
    }
    if (native_settings_runtime_) {
      if (closing) {
        // A close request may run reentrantly during native presentation while
        // a worker waits on this UI thread. Do not drain rendering here. Pending
        // previews are discarded with the process; committed disk preferences
        // are untouched. Stop leaves the last immutable frame alive for callers.
        native_settings_runtime_->StopFromUI();
      } else {
        // Module reinitialization has a real drain boundary, unlike close.
        auto recording = legodimensions::gpu_native::HostDevice::LockRecording();
        native_settings_runtime_->AdvanceFromUI();
        native_settings_runtime_->StopFromUI();
      }
      native_settings_runtime_.reset();
      // Published copied state remains valid for any final renderer work. It
      // is replaced on next initialization or cleared after device shutdown.
    }
  }
#endif
  legodimensions::WindowedSize windowed_size_;
  // Last-resort exit if the close path wedges. Detached on purpose: if the
  // process gets where it is going first, _Exit takes this thread with it.
  static void StartShutdownWatchdog() {
    static std::once_flag once;
    std::call_once(once, [] {
      std::thread([] {
#if defined(LEGODIMENSIONS_NATIVE_GPU) && defined(_WIN32)
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        // Cleanup and even logging can be waiting on the UI/renderer. Never
        // acquire those locks in the backstop. TerminateProcess also avoids
        // DLL detach handlers that may wait for another game thread.
        TerminateProcess(GetCurrentProcess(), 0);
        std::_Exit(0);
#else
        std::this_thread::sleep_for(std::chrono::seconds(3));
        REXLOG_WARN("Shutdown wedged past the close request; hard-exiting.");
        rex::FlushLogging();
        std::_Exit(0);
#endif
      }).detach();
    });
  }

  std::unique_ptr<rex::ui::ImGuiDialog> mod_menu_;
  std::unique_ptr<rex::ui::ImGuiDialog> cheat_menu_;
  std::unique_ptr<rex::ui::ImGuiDialog> main_menu_;

 public:
  // Override virtual hooks for customization:
  // FSR 2/3 run on synthesized depth and motion and smear busy scenes (the
  // Endless Sea of Possibilities report); people picked them and filed bugs.
  // Hidden from F4, and an install that already has one gets FSR 1 instead.
  // Here because the config is loaded but the presenter is not built yet.
  void OnPostInitLogging() override {
#ifdef LEGODIMENSIONS_NATIVE_GPU
    // The runtime DLL installed its default callback during static init.
    // Extend fatal logs through its public API while retaining the accepted DLL.
    legodimensions::gpu_native::InstallNativeCrashReporter();
#endif
    const std::string effect = REXCVAR_GET(present_effect);
    if (effect == "fsr2" || effect == "fsr3") {
      rex::cvar::SetFlagByName("present_effect", "fsr");
      REXLOG_INFO("present_effect={} is no longer offered; using fsr", effect);
    }
    // Batched memexport readback put stale GPU data back over what the game
    // wrote since (exploded geometry on Endless Sea of Possibilities). Locked
    // off, whatever an older install wrote into the config.
    rex::cvar::SetFlagByName("readback_memexport_batched", "false");
    // On-demand readback (0.1.26) tears the Simpsons world geometry on AMD
    // RX 9060 XT; off = the synchronous path. Same lock as above.
    rex::cvar::SetFlagByName("readback_memexport_on_demand", "false");
  }
  // void OnPreSetup(rex::RuntimeConfig& config) override {}
  // void OnLoadXexImage(std::string& xex_image) override {}
  // void OnPostLoadXexImage() override {}
  // void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {}
  // std::unique_ptr<rex::ui::ImGuiDialog> CreateAchievementsOverlay() override;
  // std::unique_ptr<rex::ui::AchievementNotificationDialog>
  // CreateAchievementNotificationDialog() override;
  // void OnConfigurePaths(rex::PathConfig& paths) override {}
};
