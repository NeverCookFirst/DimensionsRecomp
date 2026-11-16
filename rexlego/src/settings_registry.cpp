#include "settings_registry.h"
#include <cstdio>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <stdexcept>
#include <thread>
#include <rex/cvar.h>

namespace legodimensions::settings {
std::string NativeSettingsLogValues(const Values& values) {
  std::string text;
  for (const auto& spec : NativeSettingsCatalog()) {
    const auto found = values.find(spec.key);
    if (found == values.end()) continue;
    if (!text.empty()) text += ' ';
    text += std::string(spec.key) + '=' + found->second;
  }
  return text;
}
namespace {
constexpr std::string_view qualities[]{"native", "quality", "balanced", "performance", "ultra_performance"};
constexpr std::string_view effects[]{"bilinear", "fsr"};
constexpr std::string_view cel_modes[]{"original", "everywhere", "off"};
const SettingSpec catalog[]{
    {"fullscreen", "Borderless fullscreen", SettingsGroup::kDisplay, ConfigType::kBoolean, "false", 0, 0, {}, true},
    {"native_vsync", "Vertical sync", SettingsGroup::kDisplay, ConfigType::kBoolean, "false", 0, 0, {}, true},
    {"native_fps_limit", "Frame rate limit (0: uncapped)", SettingsGroup::kPerformance, ConfigType::kInteger, "240", 0, 1000},
    {"native_show_fps", "FPS meter", SettingsGroup::kPerformance, ConfigType::kBoolean, "true"},
    {"native_resolution_scale", "Internal scene resolution (300%: 3840 x 2160)", SettingsGroup::kPerformance, ConfigType::kInteger, "0", 0, 300, {}, true},
    {"native_bloom", "Bloom glow", SettingsGroup::kDisplay, ConfigType::kBoolean, "true"},
    {"native_cel_shading", "Cel shading", SettingsGroup::kDisplay, ConfigType::kString, "original", 0, 0, cel_modes},
    {"native_fsr3", "FSR 3 upscaling", SettingsGroup::kGraphics, ConfigType::kBoolean, "true", 0, 0, {}, true},
    {"native_fsr3_quality", "FSR 3 quality", SettingsGroup::kGraphics, ConfigType::kString, "quality", 0, 0, qualities, true},
    {"native_fsr3_sharpen", "FSR 3 sharpening", SettingsGroup::kGraphics, ConfigType::kBoolean, "true"},
    {"native_fsr3_sharpness", "FSR 3 sharpening strength", SettingsGroup::kGraphics, ConfigType::kFloat, "0.2", 0, 1},
    {"native_fsr3_mip_bias", "FSR 3 texture detail adjustment", SettingsGroup::kGraphics, ConfigType::kFloat, "0", -1, 1},
    {"present_effect", "FSR 1 / bilinear (when FSR 3 is off)", SettingsGroup::kGraphics, ConfigType::kString, "bilinear", 0, 0, effects, true},
    {"present_fsr_sharpness_reduction", "FSR 1 sharpening reduction", SettingsGroup::kGraphics, ConfigType::kFloat, "0.2", 0, 2},
    {"present_fsr_max_upsampling_passes", "FSR 1 maximum upscaling passes", SettingsGroup::kGraphics, ConfigType::kInteger, "4", 1, 4},
    {"native_anisotropic_override", "Texture filtering (-2: installation, -1: game)", SettingsGroup::kGraphics, ConfigType::kInteger, "-2", -2, 5},
    {"depth_of_field", "Depth of field", SettingsGroup::kGraphics, ConfigType::kBoolean, "true"},
};

const SettingSpec* Find(std::string_view key) {
  for (const auto& spec : catalog) if (spec.key == key) return &spec;
  return nullptr;
}

NormalizedValue Canonical(const SettingSpec& spec, std::string_view value, bool validate) {
  const auto invalid = [&] { return NormalizedValue{false, {}, "Invalid value for " + std::string(spec.label)}; };
  std::string normalized;
  switch (spec.type) {
    case ConfigType::kBoolean:
      if (value != "true" && value != "false") return invalid();
      normalized = value;
      break;
    case ConfigType::kInteger: {
      int32_t number{};
      auto result = std::from_chars(value.data(), value.data() + value.size(), number);
      if (result.ec != std::errc{} || result.ptr != value.data() + value.size()) return invalid();
      if (validate && (number < spec.minimum || number > spec.maximum)) return invalid();
      if (validate && spec.key == "native_resolution_scale" && number != 0 &&
          (number < 20 || number % 5 != 0)) return invalid();
      normalized = std::to_string(number);
      break;
    }
    case ConfigType::kFloat: {
      double number{};
      auto result = std::from_chars(value.data(), value.data() + value.size(), number);
      if (result.ec != std::errc{} || result.ptr != value.data() + value.size() || !std::isfinite(number)) return invalid();
      if (validate && (number < spec.minimum || number > spec.maximum)) return invalid();
      // SDK double getters use six decimals. Use the same precision for
      // drafts and reads, so 0.2 and 0.200000 compare as the same value.
      char text[384];
      auto converted = std::to_chars(text, text + sizeof(text), number, std::chars_format::fixed, 6);
      if (converted.ec != std::errc{}) return invalid();
      normalized.assign(text, converted.ptr);
      while (normalized.ends_with('0')) normalized.pop_back();
      if (normalized.ends_with('.')) normalized.pop_back();
      if (normalized == "-0") normalized = "0";
      break;
    }
    case ConfigType::kString:
      if (validate && !spec.choices.empty() &&
          std::find(spec.choices.begin(), spec.choices.end(), value) == spec.choices.end()) return invalid();
      normalized = value;
      break;
  }
  return {true, std::move(normalized), {}};
}

void AssertOwner(std::thread::id owner) {
  if (owner != std::this_thread::get_id())
    throw std::runtime_error("Native settings must be changed on the platform UI thread");
}

Values ReadRegistryValues() {
  Values result;
  for (const auto& spec : catalog) {
    const auto raw = rex::cvar::GetFlagByName(spec.key);
    if (raw.empty()) throw std::runtime_error("Native setting is not registered: " + std::string(spec.key));
    auto normalized = Canonical(spec, raw, false);
    if (!normalized.valid) throw std::runtime_error(normalized.error);
    result.emplace(spec.key, std::move(normalized.value));
  }
  return result;
}
}  // namespace

std::span<const SettingSpec> NativeSettingsCatalog() { return catalog; }
Values NativeSettingsDefaults() {
  Values result;
  for (const auto& spec : catalog) result.emplace(spec.key, spec.default_value);
  return result;
}
std::set<std::string, std::less<>> NativeSettingsConfirmationKeys() {
  std::set<std::string, std::less<>> result;
  for (const auto& spec : catalog) if (spec.confirm) result.emplace(spec.key);
  return result;
}
NormalizedValue NormalizeNativeSetting(std::string_view key, std::string_view value) {
  // Upgrade saved/edited legacy Off without rejecting the rest of a profile.
  if (key == "native_cel_shading" && value == "off") value = "original";
  const auto* spec = Find(key);
  return spec ? Canonical(*spec, value, true) : NormalizedValue{false, {}, "Unknown native setting"};
}

std::string NativeSettingOverrideReason(std::string_view key) {
  if (key == "native_vsync") {
    if (const char* forced = std::getenv("LEGO_NATIVE_PRESENT"); forced && *forced) {
      int sync = -1;
      if (std::sscanf(forced, "%d", &sync) == 1 && sync >= 0)
        return "Setting is overridden by LEGO_NATIVE_PRESENT";
    }
  }
  const auto source = rex::cvar::GetFlagSource(key);
  if (source == rex::cvar::Source::kCommandLine) return "Setting is controlled by an explicit command-line option";
  if (source == rex::cvar::Source::kEnvironment) return "Setting is controlled by an explicit environment option";
  std::string_view variable;
  if (key == "native_fps_limit") variable = "LEGO_NATIVE_FPS_LIMIT";
  else if (key == "native_vsync") variable = "LEGO_NATIVE_VSYNC";
  else if (key == "native_fsr3") variable = "LEGO_NATIVE_FSR3";
  else if (key == "native_fsr3_quality") variable = "LEGO_NATIVE_FSR3_QUALITY";
  if (!variable.empty()) {
    const char* value = std::getenv(variable.data());
    if (value && *value) return "Setting is overridden by " + std::string(variable);
  }
  if (key == "native_fsr3_quality") {
    const char* value = std::getenv("LEGO_NATIVE_720P");
    if (value && *value && value[0] != '0') return "Setting is overridden by LEGO_NATIVE_720P";
  }
  return {};
}

Backend MakeNativeRegistryBackend(std::filesystem::path config_path, SettingAvailability availability,
                                  SettingWriter writer) {
  const auto owner = std::this_thread::get_id();
  return {
      [owner] {
        AssertOwner(owner);
        // Preserve legacy unsupported values until the player edits them.
        return ReadRegistryValues();
      },
      [owner, availability](std::string_view key, std::string_view value) {
        AssertOwner(owner);
        auto reason = NativeSettingOverrideReason(key);
        if (reason.empty() && availability) reason = availability(key);
        if (!reason.empty()) return NormalizedValue{false, {}, std::move(reason)};
        return NormalizeNativeSetting(key, value);
      },
      [owner, writer = std::move(writer)](std::string_view key, std::string_view value) {
        AssertOwner(owner);
        // Rollback must be able to restore an old value outside the menu's
        // current choices; proposal validation already happened before Apply.
        return Find(key) && (writer ? writer(key, value) : rex::cvar::SetFlagByName(key, value));
      },
      [owner, config_path = std::move(config_path)](const Values& changes, std::string& error) {
        AssertOwner(owner);
        if (changes.empty()) return true;
        std::vector<ConfigChange> patch;
        for (const auto& [key, value] : changes) {
          const auto* spec = Find(key);
          if (!spec) { error = "Unknown native setting: " + key; return false; }
          const auto normalized = NormalizeNativeSetting(key, value);
          if (!normalized.valid || normalized.value != value) { error = "Invalid native setting: " + key; return false; }
          auto reason = NativeSettingOverrideReason(key);
          if (!reason.empty()) { error = std::move(reason); return false; }
        }
        const auto current = ReadRegistryValues();
        for (const auto& [key, value] : changes)
          if (current.at(key) != value) { error = "Setting changed before saving: " + key; return false; }
        // Commit the approved native choices, including unchanged choices
        // inherited from this launch's defaults. A later launcher/default
        // change must not silently change them. Temporary explicit overrides
        // and unsupported legacy choices are not new player preferences.
        for (const auto& spec : catalog) {
          if (!NativeSettingOverrideReason(spec.key).empty()) continue;
          auto normalized = Canonical(spec, current.at(std::string(spec.key)), true);
          if (normalized.valid) patch.push_back({std::string(spec.key), spec.type, std::move(normalized.value)});
        }
        return SaveConfigPatch(config_path, patch, error);
      }};
}

}  // namespace legodimensions::settings
