#include "settings_preferences.h"

#include <charconv>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <toml++/toml.hpp>
#include <rex/cvar.h>

REXCVAR_DEFINE_BOOL(native_settings_load, true, "Graphics",
    "Load committed native player settings from user data. Disable explicitly for a comparison using launcher/config defaults.")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace legodimensions::settings {
namespace {
std::string ValueText(const toml::node& node, const SettingSpec& spec) {
  switch (spec.type) {
    case ConfigType::kBoolean:
      if (node.is_boolean()) return *node.value<bool>() ? "true" : "false";
      break;
    case ConfigType::kInteger:
      if (node.is_integer()) return std::to_string(*node.value<int64_t>());
      break;
    case ConfigType::kFloat:
      if (node.is_integer() || node.is_floating_point()) {
        char text[128];
        const auto result = std::to_chars(text, text + sizeof(text), *node.value<double>());
        if (result.ec == std::errc{}) return {text, result.ptr};
      }
      break;
    case ConfigType::kString:
      if (node.is_string()) return *node.value<std::string>();
      break;
  }
  throw std::runtime_error("Invalid saved setting type: " + std::string(spec.label));
}
}  // namespace

std::filesystem::path NativePreferencesPath(const std::filesystem::path& user_data_root) {
  if (user_data_root.empty()) throw std::runtime_error("Native settings require a resolved user-data folder");
  return user_data_root / "native-settings.toml";
}
bool NativePreferencesEnabled() { return REXCVAR_GET(native_settings_load); }

PreferencesResult LoadNativePreferences(const std::filesystem::path& path, Backend& backend) {
  PreferencesResult result;
  Values baseline, proposal;
  std::vector<std::string> attempted;
  try {
    if (!NativePreferencesEnabled()) return result;
    if (!std::filesystem::exists(path)) return result;
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("Could not read saved native settings");
    std::ostringstream bytes;
    bytes << input.rdbuf();
    if (input.bad()) throw std::runtime_error("Could not read all saved native settings");
    const auto table = toml::parse(bytes.str());
    // Validate before considering overrides or touching the registry. A bad
    // recognized value never leaves a partly applied startup configuration.
    for (const auto& spec : NativeSettingsCatalog()) {
      const auto* node = table.get(spec.key);
      if (!node) continue;
      auto normalized = NormalizeNativeSetting(spec.key, ValueText(*node, spec));
      if (!normalized.valid) throw std::runtime_error(normalized.error);
      proposal.emplace(spec.key, std::move(normalized.value));
    }
    baseline = backend.read();  // Also enforces the backend's owning thread.
    for (const auto& [key, value] : proposal) {
      const auto source = rex::cvar::GetFlagSource(key);
      if (source == rex::cvar::Source::kEnvironment || source == rex::cvar::Source::kCommandLine ||
          !NativeSettingOverrideReason(key).empty()) {
        result.overridden.push_back(key);
        continue;
      }
      if (!baseline.contains(key)) throw std::runtime_error("Saved native setting is not available: " + key);
      if (baseline.at(key) == value) continue;
      // Record even a failing/throwing setter: it may have partly mutated.
      attempted.push_back(key);
      if (!backend.write(key, value)) throw std::runtime_error("Could not restore saved setting: " + key);
      result.applied.emplace(key, value);
    }
    const auto current = backend.read();
    for (const auto& [key, value] : result.applied)
      if (!current.contains(key) || current.at(key) != value)
        throw std::runtime_error("Saved setting did not apply: " + key);
    return result;
  } catch (const std::exception& exception) {
    result.ok = false;
    result.error = exception.what();
  }
  for (auto it = attempted.rbegin(); it != attempted.rend(); ++it) {
    try {
      if (!backend.write(*it, baseline.at(*it))) result.recovery_failed = true;
    } catch (...) { result.recovery_failed = true; }
  }
  if (!attempted.empty()) {
    try {
      const auto restored = backend.read();
      for (const auto& key : attempted)
        if (!restored.contains(key) || restored.at(key) != baseline.at(key)) result.recovery_failed = true;
    } catch (...) { result.recovery_failed = true; }
  }
  result.applied.clear();
  if (result.recovery_failed) result.error += "; previous settings could not be restored";
  return result;
}

}  // namespace legodimensions::settings
