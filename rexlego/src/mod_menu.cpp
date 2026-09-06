// legodimensions - ReXGlue Recompiled Project
//
// See mod_menu.h for why applying a mod set takes effect on the next launch.

#include "mod_menu.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#include <imgui.h>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/rex_app.h>
#include <rex/ui/imgui_dialog.h>

// Comma-separated mod folder names. This is the persisted selection, and it is
// what ResolveUpdateRoot keys off at startup.
REXCVAR_DEFINE_STRING(mods, "", "Mods", "Enabled mod folders, comma separated")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
// The defaults below are relative to the working directory and match the layout
// the installer lays down. A development tree points them somewhere else through
// legodimensions.toml; the installer writes absolute paths there on install.
REXCVAR_DEFINE_STRING(mods_root, "mods", "Mods",
                      "Folder containing one subfolder per mod");
// A copy of the update folder with the mods injected. Everything in it except
// PATCH.DAT/PATCH.HDR is a hard link back to the vanilla folder, so it costs a
// fraction of the size and the original is never modified.
REXCVAR_DEFINE_STRING(mods_update_root, "update-mods", "Mods",
                      "Modded copy of the update folder, used when any mod is enabled");
REXCVAR_DEFINE_STRING(modcli_path, "tools/modcli/modcli.exe", "Mods",
                      "Tool that performs the DAT injection");
// The mods folder is shared with the RPCS3 build, so it also holds mods that
// target PS3 data this build does not have. Only mods declaring this platform,
// or "any", are listed and applied.
REXCVAR_DEFINE_STRING(mods_platform, "x360", "Mods", "Platform tag mods must match");

namespace legodimensions::mods {
namespace {

struct ModEntry {
  std::string folder;
  std::string name;
  bool enabled = false;
};

std::vector<std::string> SplitList(const std::string& csv) {
  std::vector<std::string> out;
  size_t start = 0;
  while (start <= csv.size()) {
    size_t comma = csv.find(',', start);
    std::string item =
        csv.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
    // Trim - the value is hand-editable in the .toml and in the F4 list.
    size_t b = item.find_first_not_of(" \t");
    size_t e = item.find_last_not_of(" \t");
    if (b != std::string::npos) {
      out.push_back(item.substr(b, e - b + 1));
    }
    if (comma == std::string::npos) {
      break;
    }
    start = comma + 1;
  }
  return out;
}

// mod.json carries five short string fields and nothing nested, so the display
// name is pulled out directly rather than linking a JSON parser for it. A mod
// whose name cannot be read still works; it just shows its folder name.
std::string ReadJsonString(const std::string& text, const std::string& field) {
  const std::string key = "\"" + field + "\"";
  size_t k = text.find(key);
  if (k == std::string::npos) {
    return {};
  }
  size_t colon = text.find(':', k + key.size());
  if (colon == std::string::npos) {
    return {};
  }
  size_t open = text.find('"', colon);
  if (open == std::string::npos) {
    return {};
  }
  size_t close = text.find('"', open + 1);
  if (close == std::string::npos) {
    return {};
  }
  return text.substr(open + 1, close - open - 1);
}

bool ReadManifest(const std::filesystem::path& mod_json, std::string& name_out,
                  std::string& platform_out) {
  std::ifstream file(mod_json, std::ios::binary);
  if (!file) {
    return false;
  }
  std::ostringstream buffer;
  buffer << file.rdbuf();
  const std::string text = buffer.str();

  name_out = ReadJsonString(text, "name");
  platform_out = ReadJsonString(text, "platform");
  return true;
}

std::vector<ModEntry> Discover() {
  std::vector<ModEntry> discovered;
  const std::string root = REXCVAR_GET(mods_root);
  if (root.empty()) {
    return discovered;
  }

  std::error_code ec;
  std::filesystem::directory_iterator it(root, ec);
  if (ec) {
    REXLOG_WARN("Mods folder not readable: {} ({})", root, ec.message());
    return discovered;
  }

  const std::vector<std::string> enabled = SplitList(REXCVAR_GET(mods));
  for (const auto& dir : it) {
    if (!dir.is_directory()) {
      continue;
    }
    std::filesystem::path manifest = dir.path() / "mod.json";
    if (!std::filesystem::exists(manifest)) {
      continue;
    }
    std::string name;
    std::string platform;
    if (!ReadManifest(manifest, name, platform)) {
      continue;
    }
    // "any" is the manifest's own wildcard; an unreadable platform field is
    // treated as a mismatch rather than silently offering a mod built for
    // other data.
    const std::string wanted = REXCVAR_GET(mods_platform);
    if (platform != "any" && platform != wanted) {
      continue;
    }

    ModEntry entry;
    entry.folder = dir.path().filename().string();
    entry.name = name.empty() ? entry.folder : name;
    entry.enabled = std::find(enabled.begin(), enabled.end(), entry.folder) != enabled.end();
    discovered.push_back(std::move(entry));
  }

  std::sort(discovered.begin(), discovered.end(),
            [](const ModEntry& a, const ModEntry& b) { return a.name < b.name; });
  return discovered;
}

std::string Quote(const std::string& s) { return "\"" + s + "\""; }

// cmd.exe strips the outer pair of quotes from the whole command line, so a
// command whose program path is quoted needs a second pair around everything.
int RunQuoted(const std::string& command) {
  return std::system(("\"" + command + "\"").c_str());
}

class ModMenuDialog final : public rex::ui::ImGuiDialog {
 public:
  explicit ModMenuDialog(rex::ui::ImGuiDrawer* drawer)
      : ImGuiDialog(drawer), mods_(Discover()) {}

  void OnDraw(ImGuiIO& io) override {
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, 80.0f), ImGuiCond_FirstUseEver,
                            ImVec2(0.5f, 0.0f));
    ImGui::SetNextWindowSize(ImVec2(520.0f, 360.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowBgAlpha(0.92f);

    if (!ImGui::Begin("Mods##rex", nullptr, ImGuiWindowFlags_NoCollapse)) {
      ImGui::End();
      return;
    }

    if (mods_.empty()) {
      ImGui::TextWrapped("No mods found in %s", REXCVAR_GET(mods_root).c_str());
      ImGui::End();
      return;
    }

    ImGui::TextUnformatted("Changes apply on the next launch.");
    ImGui::Separator();

    ImGui::BeginChild("##modlist", ImVec2(0.0f, -64.0f), false);
    for (ModEntry& mod : mods_) {
      ImGui::PushID(mod.folder.c_str());
      ImGui::Checkbox(mod.name.c_str(), &mod.enabled);
      ImGui::SameLine();
      ImGui::TextDisabled("(%s)", mod.folder.c_str());
      ImGui::PopID();
    }
    ImGui::EndChild();

    ImGui::Separator();
    if (ImGui::Button("Apply", ImVec2(120.0f, 0.0f))) {
      Apply();
    }
    if (!status_.empty()) {
      ImGui::SameLine();
      ImGui::TextWrapped("%s", status_.c_str());
    }

    ImGui::End();
  }

 private:
  void Apply() {
    status_.clear();

    const std::string cli = REXCVAR_GET(modcli_path);
    const std::string target = REXCVAR_GET(mods_update_root);
    const std::string root = REXCVAR_GET(mods_root);

    if (!std::filesystem::exists(cli)) {
      status_ = "modcli not found";
      REXLOG_ERROR("modcli not found at {}", cli);
      return;
    }

    // Always restore first: the tool patches in place, so switching a mod off
    // means putting the vanilla bytes back before re-applying the rest.
    RunQuoted(Quote(cli) + " restore " + Quote(target));

    std::string selection;
    std::string args;
    for (const ModEntry& mod : mods_) {
      if (!mod.enabled) {
        continue;
      }
      if (!selection.empty()) {
        selection += ",";
      }
      selection += mod.folder;
      args += " " + Quote(mod.folder);
    }

    if (!selection.empty()) {
      int rc = RunQuoted(Quote(cli) + " apply " + Quote(target) + " " + Quote(root) + " " +
                         REXCVAR_GET(mods_platform) + args);
      if (rc != 0) {
        status_ = "modcli failed, see the log";
        REXLOG_ERROR("modcli apply returned {}", rc);
        return;
      }
    }

    REXCVAR_SET(mods, selection);
    rex::cvar::SaveConfig(rex::filesystem::GetExecutableFolder() / "legodimensions.toml");

    status_ = selection.empty() ? "All off. Restart for vanilla."
                                : "Applied. Restart to load.";
    REXLOG_INFO("Mod selection applied: [{}]", selection);
  }

  std::vector<ModEntry> mods_;
  std::string status_;
};

}  // namespace

std::unique_ptr<rex::ui::ImGuiDialog> CreateMenu(rex::ui::ImGuiDrawer* drawer) {
  return std::make_unique<ModMenuDialog>(drawer);
}

void ResolveUpdateRoot(rex::PathConfig& paths) {
  const std::string selection = REXCVAR_GET(mods);
  const std::string modded = REXCVAR_GET(mods_update_root);
  if (selection.empty() || modded.empty()) {
    return;
  }
  if (!std::filesystem::exists(std::filesystem::path(modded) / "PATCH.DAT")) {
    REXLOG_WARN("Mods are enabled but {} has no PATCH.DAT; using the vanilla update folder",
                modded);
    return;
  }
  paths.update_data_root = modded;
  REXLOG_INFO("Mods enabled [{}], update folder: {}", selection, modded);
}

}  // namespace legodimensions::mods
