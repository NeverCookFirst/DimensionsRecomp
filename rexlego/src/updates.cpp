// legodimensions - ReXGlue Recompiled Project
//
// See updates.h. The updater runs as a separate process on purpose: the game
// must never wait on the network, and a crash in the updater must not be a
// crash in the game.

#include "updates.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <thread>

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

// Updater.cs returns this from "--check --console" when a newer release exists.
constexpr unsigned long kUpdateAvailableExitCode = 10;
// A check that hangs must not keep a thread alive for the whole session.
constexpr unsigned long kUpdateCheckTimeoutMs = 60000;

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
  // No STARTF_USESHOWWINDOW: the updater is a GUI process that stays silent
  // unless there is something to report, and asking for SW_HIDE here can carry
  // into the window it does decide to show. DETACHED_PROCESS alone is what
  // keeps a console from appearing.
  PROCESS_INFORMATION process{};

  const std::wstring working_directory = exe.parent_path().wstring();
  const BOOL ok = CreateProcessW(nullptr, command_line.data(), nullptr, nullptr, FALSE,
                                 DETACHED_PROCESS, nullptr,
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

// The installed version, straight out of the file the installer leaves behind.
// Every bug report starts with "which build is this", and until now the log
// could not answer it.
void LogInstalledVersion() {
  const std::filesystem::path manifest = rex::filesystem::GetExecutableFolder() / "install.json";
  std::ifstream file(manifest);
  if (!file) {
    REXLOG_INFO("Build: no install.json beside the game, so this is a development build");
    return;
  }
  const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
  const std::string key = "\"version\"";
  const size_t at = text.find(key);
  const size_t open = at == std::string::npos ? std::string::npos : text.find('"', at + key.size());
  const size_t close = open == std::string::npos ? std::string::npos : text.find('"', open + 1);
  if (close == std::string::npos) {
    REXLOG_WARN("Build: install.json carries no version");
    return;
  }
  REXLOG_INFO("Build: Dimensions Recompiled {}", text.substr(open + 1, close - open - 1));
}

}  // namespace

void CheckAtStartup() {
  LogInstalledVersion();
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
  std::wstring target = L" --dir \"" + install_dir.wstring() + L"\"";
  if (!repo.empty()) {
    target += L" --repo \"" + std::filesystem::path(repo).wstring() + L"\"";
  }

  // Ask the updater whether there is anything, then say so from the game.
  // Letting the updater raise its own window at startup proved unreliable -
  // players saw nothing and only found the update by running the tool by hand -
  // so the game now tells them itself, and names the file that does the work.
  std::thread([updater, target, repo] {
    std::wstring command_line = L"\"" + updater.wstring() + L"\" --check --console" + target;
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    const std::wstring working_directory = updater.parent_path().wstring();
    if (!CreateProcessW(nullptr, command_line.data(), nullptr, nullptr, FALSE, DETACHED_PROCESS,
                        nullptr, working_directory.c_str(), &startup, &process)) {
      REXLOG_WARN("Could not start the updater at {} (error {})", updater.string(), GetLastError());
      return;
    }
    WaitForSingleObject(process.hProcess, kUpdateCheckTimeoutMs);
    DWORD exit_code = 0;
    GetExitCodeProcess(process.hProcess, &exit_code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);

    if (exit_code != kUpdateAvailableExitCode) {
      REXLOG_INFO("Update check finished, nothing to install (updater exit code {})", exit_code);
      return;
    }
    REXLOG_INFO("An update is available ({})", repo.empty() ? "default feed" : repo);

    const std::wstring text =
        L"A new version of Dimensions Recompiled is available.\n\n"
        L"Install it now? The updater will open:\n" + updater.wstring();
    if (MessageBoxW(nullptr, text.c_str(), L"Dimensions Recompiled - Update available",
                    MB_YESNO | MB_ICONINFORMATION | MB_SETFOREGROUND) != IDYES) {
      REXLOG_INFO("Update declined by the player");
      return;
    }
    std::wstring launch = L"\"" + updater.wstring() + L"\"" + target;
    if (StartDetached(updater, launch)) {
      REXLOG_INFO("Updater launched");
    } else {
      REXLOG_WARN("Could not launch the updater at {} (error {})", updater.string(),
                  GetLastError());
    }
  }).detach();
#else
  REXLOG_INFO("Update checks are Windows-only for now");
#endif
}

}  // namespace legodimensions::updates
