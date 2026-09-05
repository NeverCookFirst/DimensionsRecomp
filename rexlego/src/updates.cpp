// legodimensions - ReXGlue Recompiled Project
//
// See updates.h. The updater runs as a separate process on purpose: the game
// must never wait on the network, and a crash in the updater must not be a
// crash in the game.

#include "updates.h"

#include <filesystem>
#include <string>
#include <system_error>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>

#ifdef _WIN32
#include <windows.h>
#endif

// The one setting this is all about. Off means the game never touches the
// network, and neither does anything it starts.
REXCVAR_DEFINE_BOOL(updates_check, true, "Updates",
                    "Look for a new release when the game starts");
REXCVAR_DEFINE_STRING(updates_repo, "NeverCookFirst/DimensionsRecomp", "Updates",
                      "GitHub owner/name the updater asks for releases");
// Written by the installer. Left empty, the default location next to the game is
// used, so a hand-made install still works.
REXCVAR_DEFINE_STRING(updater_path, "", "Updates",
                      "rexupdate.exe - checks for and applies updates");

namespace legodimensions::updates {
namespace {

std::filesystem::path ResolveUpdater() {
  const std::string configured = REXCVAR_GET(updater_path);
  if (!configured.empty()) {
    return std::filesystem::path(configured);
  }
  return rex::filesystem::GetExecutableFolder() / "tools" / "rexupdate" / "rexupdate.exe";
}

#ifdef _WIN32
// CreateProcess rather than std::system: that one goes through cmd.exe, flashes
// a console window and, worse, blocks until the child is done.
bool StartDetached(const std::filesystem::path& exe, std::wstring command_line) {
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESHOWWINDOW;
  startup.wShowWindow = SW_HIDE;   // the updater shows its own window if it needs one
  PROCESS_INFORMATION process{};

  const std::wstring working_directory = exe.parent_path().wstring();
  const BOOL ok = CreateProcessW(nullptr, command_line.data(), nullptr, nullptr, FALSE,
                                 CREATE_NO_WINDOW | DETACHED_PROCESS, nullptr,
                                 working_directory.c_str(), &startup, &process);
  if (!ok) {
    return false;
  }
  // Nothing here waits for it, so both handles go straight back.
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  return true;
}
#endif

}  // namespace

void CheckAtStartup() {
  if (!REXCVAR_GET(updates_check)) {
    REXLOG_INFO("Update check disabled (updates_check = false)");
    return;
  }

  const std::filesystem::path updater = ResolveUpdater();
  std::error_code ec;
  if (!std::filesystem::exists(updater, ec)) {
    REXLOG_INFO("No updater at {}; skipping the update check", updater.string());
    return;
  }

#ifdef _WIN32
  const std::filesystem::path install_dir = rex::filesystem::GetExecutableFolder();
  const std::string repo = REXCVAR_GET(updates_repo);
  std::wstring command_line = L"\"" + updater.wstring() + L"\" --quiet --dir \"" +
                              install_dir.wstring() + L"\"";
  if (!repo.empty()) {
    command_line += L" --repo \"" + std::filesystem::path(repo).wstring() + L"\"";
  }
  if (StartDetached(updater, command_line)) {
    REXLOG_INFO("Update check started ({})", repo.empty() ? "default feed" : repo);
  } else {
    REXLOG_WARN("Could not start the updater at {} (error {})", updater.string(), GetLastError());
  }
#else
  REXLOG_INFO("Update checks are Windows-only for now");
#endif
}

}  // namespace legodimensions::updates
