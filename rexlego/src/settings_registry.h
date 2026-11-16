#pragma once

#include "settings_config.h"
#include "settings_transaction.h"

#include <filesystem>
#include <span>

namespace legodimensions::settings {

enum class SettingsGroup { kDisplay, kPerformance, kGraphics };
struct SettingSpec {
  std::string_view key, label;
  SettingsGroup group;
  ConfigType type;
  std::string_view default_value;
  double minimum{}, maximum{};
  std::span<const std::string_view> choices;
  bool confirm = false;
};

// Controls with verified native renderer consumers. Missing controls remain
// required integration work; the SDK's entire settings list is not equivalent
// to native renderer support. The native frontend uses this same model.
std::span<const SettingSpec> NativeSettingsCatalog();
Values NativeSettingsDefaults();
std::string NativeSettingsLogValues(const Values& values);
std::set<std::string, std::less<>> NativeSettingsConfirmationKeys();
NormalizedValue NormalizeNativeSetting(std::string_view key, std::string_view value);

// Additional device/capability denial supplied by the host. Empty means the
// setting is available; a reason is suitable for an in-game failure dialog.
using SettingAvailability = std::function<std::string(std::string_view)>;
// Optional host write path, including the SDK registry write itself. This lets
// the app capture window geometry before the SDK fullscreen callback runs.
// Invoked only for known keys and only on the backend's owning UI thread.
using SettingWriter = std::function<bool(std::string_view, std::string_view)>;
std::string NativeSettingOverrideReason(std::string_view key);

// Construct on the platform UI thread. Every callback enforces that thread,
// including persistence and rollback. Never call the SDK's window callbacks
// directly from a guest PPC hook. Renderer acknowledgement remains separate.
// A commit patches the approved native snapshot into the preference file;
// temporary explicit overrides and invalid unchanged legacy values are omitted.
Backend MakeNativeRegistryBackend(std::filesystem::path config_path,
                                  SettingAvailability availability = {},
                                  SettingWriter writer = {});

}  // namespace legodimensions::settings
