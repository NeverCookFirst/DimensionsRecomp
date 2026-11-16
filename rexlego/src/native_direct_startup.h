#pragma once

#include <cstdlib>
#include <filesystem>
#include <string>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <windows.h>

inline void StartNativeCrashMonitor(const std::filesystem::path& root,
                                    const std::filesystem::path& run) {
  const auto monitor = root / L"tools" / L"CrashCapture.exe";
  const auto executable = root / L"legodimensions.exe";
  FILETIME created{}, exited{}, kernel{}, user{};
  if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user))
    return;
  const auto stamp = (static_cast<unsigned long long>(created.dwHighDateTime) << 32) |
                     created.dwLowDateTime;
  auto arguments = L"\"" + monitor.wstring() + L"\" --observe \"" + executable.wstring() +
                   L"\" \"" + run.wstring() + L"\" " + std::to_wstring(GetCurrentProcessId()) +
                   L" " + std::to_wstring(stamp);
  STARTUPINFOW startup{sizeof(startup)};
  PROCESS_INFORMATION process{};
  if (CreateProcessW(monitor.c_str(), arguments.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                     nullptr, root.c_str(), &startup, &process)) {
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
  }
}

inline void ConfigureNativeDirectStartup() {
  const auto root = rex::filesystem::GetExecutableFolder();
  const auto settings = root / L"native-environment.ini";
  std::error_code error;
  if (!std::filesystem::is_regular_file(settings, error))
    return;

  wchar_t entries[8192]{};
  const auto length = GetPrivateProfileSectionW(L"renderer", entries, _countof(entries),
                                               settings.c_str());
  if (length < _countof(entries) - 2) {
    for (const wchar_t* entry = entries; *entry;) {
      std::wstring setting(entry);
      entry += setting.size() + 1;
      const auto separator = setting.find(L'=');
      if (separator != std::wstring::npos) {
        const auto name = setting.substr(0, separator);
        if (name.starts_with(L"LEGO_NATIVE_") || name.starts_with(L"PLUME_")) {
          const auto value = setting.substr(separator + 1);
          _putenv_s(std::string(name.begin(), name.end()).c_str(),
                    std::string(value.begin(), value.end()).c_str());
        }
      }
    }
  }

  SYSTEMTIME now{};
  GetSystemTime(&now);
  wchar_t runName[80]{};
  swprintf_s(runName, L"%04u%02u%02u-%02u%02u%02u-%03u-%lu", now.wYear, now.wMonth,
             now.wDay, now.wHour, now.wMinute, now.wSecond, now.wMilliseconds,
             GetCurrentProcessId());
  const auto run = root / L"logs" / runName;
  if (!std::filesystem::create_directories(run, error) && error)
    return;
  for (const auto* name : {L"install.json", L"legodimensions.toml", L"native-environment.ini"})
    std::filesystem::copy_file(root / name, run / name, error);
  _putenv_s("LEGO_NATIVE_FRAME_METRICS", (run / L"frames.csv").string().c_str());
  if (rex::cvar::GetFlagSource("log_file") != rex::cvar::Source::kCommandLine)
    rex::cvar::SetFlagFromCommandLine("log_file", (run / L"game.log").string());
  if (rex::cvar::GetFlagSource("log_flush_interval") != rex::cvar::Source::kCommandLine)
    rex::cvar::SetFlagFromCommandLine("log_flush_interval", "1");
  StartNativeCrashMonitor(root, run);
}
