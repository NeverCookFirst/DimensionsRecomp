#pragma once

#include "settings_registry.h"

namespace legodimensions::settings {

struct PreferencesResult {
  bool ok = true;
  bool recovery_failed = false;
  Values applied;
  std::vector<std::string> overridden;
  std::string error;
};

// A stable, title-specific user-data file, separate from launcher-generated
// executable/run configs. Old rollback executables do not load this file.
std::filesystem::path NativePreferencesPath(const std::filesystem::path& user_data_root);
bool NativePreferencesEnabled();

// Call on the backend's owner/UI thread before native device creation. Only
// catalog keys are read; paths, mods and unrelated keys never reach setters.
// Validate the whole file before writing anything. Startup never saves it.
// Explicit SDK environment/command-line and native experiment overrides win.
PreferencesResult LoadNativePreferences(const std::filesystem::path& path, Backend& backend);

}  // namespace legodimensions::settings
