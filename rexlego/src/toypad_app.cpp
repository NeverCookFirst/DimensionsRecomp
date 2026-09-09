// legodimensions - ReXGlue Recompiled Project
//
// See toypad_app.h. The companion app is a separate process on purpose - it
// owns the loopback listener the emulated portal talks to, and it must survive
// the game being restarted, so the game only ever starts it and walks away.

#include "toypad_app.h"

#include <filesystem>
#include <string>
#include <system_error>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>

#ifdef _WIN32
#include <windows.h>

#include <tlhelp32.h>
#endif

// Off by default: starting another process without being asked is not something
// the game should do on its own.
REXCVAR_DEFINE_BOOL(toypad_app_autostart, false, "Toypad",
                    "Start the LEGO Toypad app together with the game");
// Empty means the copy the installer leaves in tools\LegoToypad. A hand-made
// install, or a newer build kept somewhere else, goes in here as a full path.
REXCVAR_DEFINE_STRING(toypad_app_path, "", "Toypad",
                      "LegoToypad.exe - empty means tools\\LegoToypad next to the game");

namespace legodimensions::toypad_app {
namespace {

std::filesystem::path ResolveApp() {
  const std::string configured = REXCVAR_GET(toypad_app_path);
  if (!configured.empty()) {
    return std::filesystem::path(configured);
  }
  const std::filesystem::path dir = rex::filesystem::GetExecutableFolder() / "tools" / "LegoToypad";
  const std::filesystem::path expected = dir / "LegoToypad.exe";
  std::error_code ec;
  if (std::filesystem::exists(expected, ec)) {
    return expected;
  }
  // Released builds have carried names like LegoToypad_1.8.exe, so rather than
  // give up on a folder that plainly holds the app, take the one exe in it.
  for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
    if (entry.is_regular_file(ec) && entry.path().extension() == ".exe") {
      return entry.path();
    }
  }
  return expected;
}

#ifdef _WIN32
// The app binds a loopback port, so a second copy would come up broken. It is
// also perfectly normal for a player to have started it by hand first.
bool IsAlreadyRunning(const std::filesystem::path& exe) {
  const std::wstring wanted = exe.filename().wstring();
  HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snapshot == INVALID_HANDLE_VALUE) {
    return false;
  }
  PROCESSENTRY32W entry{};
  entry.dwSize = sizeof(entry);
  bool found = false;
  if (Process32FirstW(snapshot, &entry)) {
    do {
      if (_wcsicmp(entry.szExeFile, wanted.c_str()) == 0) {
        found = true;
        break;
      }
    } while (Process32NextW(snapshot, &entry));
  }
  CloseHandle(snapshot);
  return found;
}

// Only ever holds an app this game started. A copy the player had running
// before us is none of our business and must survive the game closing.
HANDLE started_process = nullptr;
DWORD started_pid = 0;

// Closing the window makes the game hard-exit with std::_Exit(0) - see
// ReXApp::OnClosing, which does that on purpose to avoid deadlocking on a host
// lock. No destructor, no atexit handler and no OnShutdown runs, so nothing in
// this process can be trusted to clean up. A job object is the one thing that
// still works: the kernel kills everything in it the moment the last handle
// goes away, which happens however this process dies, crash included.
HANDLE kill_on_exit_job = nullptr;

HANDLE EnsureJob() {
  if (kill_on_exit_job) {
    return kill_on_exit_job;
  }
  HANDLE job = CreateJobObjectW(nullptr, nullptr);
  if (!job) {
    return nullptr;
  }
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
  limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
  if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
    CloseHandle(job);
    return nullptr;
  }
  kill_on_exit_job = job;
  return kill_on_exit_job;
}

// CreateProcess rather than std::system, for the same reason the updater uses
// it: no cmd.exe, no console flash, and no waiting on the child.
bool StartDetached(const std::filesystem::path& exe) {
  std::wstring command_line = L"\"" + exe.wstring() + L"\"";
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};
  // The working directory is the app's own folder: it reads LegoToypad.ini
  // from there, and inheriting the game's folder would hide the settings.
  const std::wstring working_directory = exe.parent_path().wstring();
  const BOOL ok = CreateProcessW(nullptr, command_line.data(), nullptr, nullptr, FALSE,
                                 DETACHED_PROCESS, nullptr, working_directory.c_str(), &startup,
                                 &process);
  if (!ok) {
    REXLOG_ERROR("Toypad app: CreateProcess failed ({})", GetLastError());
    return false;
  }
  if (HANDLE job = EnsureJob()) {
    if (!AssignProcessToJobObject(job, process.hProcess)) {
      REXLOG_WARN("Toypad app: could not put it in the job ({}), it may outlive the game",
                  GetLastError());
    }
  }
  CloseHandle(process.hThread);
  // The process handle is kept, not closed: it is what lets the shutdown path
  // close exactly the app we started, even if the player later opens another.
  started_process = process.hProcess;
  started_pid = process.dwProcessId;
  return true;
}

// WM_CLOSE first, because the app writes LegoToypad.ini on the way out and a
// TerminateProcess would lose whatever the player changed this session.
BOOL CALLBACK AskWindowToClose(HWND window, LPARAM pid) {
  DWORD window_pid = 0;
  GetWindowThreadProcessId(window, &window_pid);
  if (window_pid == static_cast<DWORD>(pid)) {
    PostMessageW(window, WM_CLOSE, 0, 0);
  }
  return TRUE;
}
#endif  // _WIN32

}  // namespace

// Shared by the autostart path and the settings button, so both report the same
// way and neither can start a second copy.
void Launch() {
#ifdef _WIN32
  const std::filesystem::path exe = ResolveApp();
  std::error_code ec;
  if (!std::filesystem::exists(exe, ec)) {
    REXLOG_WARN("Toypad app: not found at {}, set toypad_app_path to the exe", exe.string());
    return;
  }
  if (IsAlreadyRunning(exe)) {
    REXLOG_INFO("Toypad app: {} is already running", exe.filename().string());
    return;
  }
  if (StartDetached(exe)) {
    REXLOG_INFO("Toypad app: started {}", exe.string());
  }
#else
  REXLOG_WARN("Toypad app: starting it is only implemented on Windows");
#endif
}

void StartIfEnabled() {
  if (!REXCVAR_GET(toypad_app_autostart)) {
    return;
  }
  Launch();
}

void StopIfStarted() {
#ifdef _WIN32
  if (!started_process) {
    return;
  }
  // Already gone - the player closed it by hand, which is fine.
  if (WaitForSingleObject(started_process, 0) != WAIT_TIMEOUT) {
    CloseHandle(started_process);
    started_process = nullptr;
    return;
  }
  EnumWindows(AskWindowToClose, static_cast<LPARAM>(started_pid));
  // Long enough for a settings file to be written, short enough that a wedged
  // app cannot hold the game's shutdown open.
  constexpr DWORD kCloseTimeoutMs = 3000;
  if (WaitForSingleObject(started_process, kCloseTimeoutMs) == WAIT_TIMEOUT) {
    REXLOG_WARN("Toypad app: did not close in time, terminating it");
    TerminateProcess(started_process, 0);
  } else {
    REXLOG_INFO("Toypad app: closed with the game");
  }
  CloseHandle(started_process);
  started_process = nullptr;
#endif
}

}  // namespace legodimensions::toypad_app

// A button in the settings overlay, so the app can be brought up mid-session
// without turning the autostart on and restarting the game.
REXCVAR_DEFINE_COMMAND(
    toypad_app_launch, [] { legodimensions::toypad_app::Launch(); }, "Toypad",
    "Start the LEGO Toypad app now");
