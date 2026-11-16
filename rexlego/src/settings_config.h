#pragma once

#include <filesystem>
#include <span>
#include <string>

namespace legodimensions::settings {

enum class ConfigType { kBoolean, kInteger, kFloat, kString };
struct ConfigChange {
  std::string key;
  ConfigType type;
  std::string value;
};

// Patch only committed settings. Unknown keys/tables (including paths/mods)
// survive semantically; TOML formatting/comments may be normalized. Call on
// the settings owner thread. A failed write leaves the old destination intact.
// This serializes writers using this function, not unrelated external editors.
bool SaveConfigPatch(const std::filesystem::path& path,
                     std::span<const ConfigChange> changes, std::string& error);

}  // namespace legodimensions::settings
