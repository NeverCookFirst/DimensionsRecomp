// The "your game crashed" box.
//
// When the game crashes, the SDK's last-chance filter writes the log and the
// minidump, then starts this executable again with --crash-report (see
// LaunchCrashPopup in exception_handler_win.cpp). That second process lands
// here, before the app or the runtime exist: it shows one Yes/No box and exits.
// Yes opens a pre-filled "new issue" page on GitHub and shows game.log in
// Explorer, so the file is right there to drag into the issue.

#if defined(_WIN32)

#include <windows.h>
#include <shellapi.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#pragma comment(lib, "shell32.lib")

namespace {

constexpr wchar_t kIssueUrl[] = L"https://github.com/NeverCookFirst/DimensionsRecomp/issues/new";

std::wstring ArgAfter(int argc, wchar_t** argv, const wchar_t* name) {
  for (int i = 1; i + 1 < argc; ++i) {
    if (_wcsicmp(argv[i], name) == 0) {
      return argv[i + 1];
    }
  }
  return {};
}

// The version the installer recorded, same file LogInstalledVersion reads.
std::string InstalledVersion(const std::filesystem::path& exe_dir) {
  std::ifstream file(exe_dir / "install.json");
  if (!file) {
    return "unknown (development build)";
  }
  const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
  const size_t at = text.find("\"version\"");
  const size_t open = at == std::string::npos ? std::string::npos : text.find('"', at + 9);
  const size_t close = open == std::string::npos ? std::string::npos : text.find('"', open + 1);
  return close == std::string::npos ? "unknown" : text.substr(open + 1, close - open - 1);
}

std::wstring UrlEncode(const std::string& utf8) {
  static const char kHex[] = "0123456789ABCDEF";
  std::wstring out;
  for (unsigned char c : utf8) {
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
        c == '_' || c == '.' || c == '~') {
      out += static_cast<wchar_t>(c);
    } else {
      out += L'%';
      out += static_cast<wchar_t>(kHex[c >> 4]);
      out += static_cast<wchar_t>(kHex[c & 15]);
    }
  }
  return out;
}

std::string Narrow(const std::wstring& wide) {
  const int n = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, nullptr, 0, nullptr, nullptr);
  std::string out(n > 0 ? n : 1, '\0');
  WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, out.data(), n, nullptr, nullptr);
  out.resize(n > 0 ? n - 1 : 0);
  return out;
}

void ShowCrashPopup(int argc, wchar_t** argv) {
  wchar_t exe[MAX_PATH] = {};
  GetModuleFileNameW(nullptr, exe, MAX_PATH);
  const std::filesystem::path exe_dir = std::filesystem::path(exe).parent_path();

  std::wstring log = ArgAfter(argc, argv, L"--crash-log");
  if (log.empty()) {
    log = (exe_dir / L"game.log").wstring();
  }
  const std::wstring dump = ArgAfter(argc, argv, L"--crash-dump");
  const std::wstring dump_name = dump.empty() ? L"" : std::filesystem::path(dump).filename().wstring();

  std::wstring text =
      L":( Your game crashed.\n\n"
      L"Please report it as an issue on GitHub, attach your game.log and describe what "
      L"you were doing when it happened.\n\n";
  if (!dump_name.empty()) {
    text += L"A crash dump was also saved (" + dump_name + L"); attaching it helps too.\n\n";
  }
  text += L"Open the GitHub \"New issue\" page now?\n(game.log will be shown in Explorer.)";

  const int answer = MessageBoxW(nullptr, text.c_str(), L"Dimensions Recompiled - Crash",
                                 MB_YESNO | MB_ICONERROR | MB_SETFOREGROUND | MB_TOPMOST);
  if (answer != IDYES) {
    return;
  }

  const std::string version = InstalledVersion(exe_dir);
  std::string body =
      "**What happened** (what you were doing, which level/world, which button you pressed):\n\n\n"
      "**Version:** " + version + "\n"
      "**Graphics API** (F4 -> Graphics, Direct3D 12 or Vulkan):\n"
      "**GPU:**\n\n"
      "**Attach** your `game.log` (drag it into this box)";
  if (!dump_name.empty()) {
    body += " and `" + Narrow(dump_name) + "`";
  }
  body += ".\n";
  const std::wstring url = std::wstring(kIssueUrl) + L"?labels=bug&title=" +
                           UrlEncode("Crash: ") + L"&body=" + UrlEncode(body);
  ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);

  // Point Explorer at the log (or the dump, if the log is not where we think).
  std::error_code ec;
  const std::wstring select = std::filesystem::exists(log, ec) ? log : dump;
  if (!select.empty()) {
    const std::wstring params = L"/select,\"" + select + L"\"";
    ShellExecuteW(nullptr, L"open", L"explorer.exe", params.c_str(), nullptr, SW_SHOWNORMAL);
  }
}

// Runs during static initialisation of the exe, before WinMain and before the
// app touches the window, the GPU or the log - a crash report must never start
// a second game.
const bool kCrashPopupChecked = [] {
  int argc = 0;
  wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  if (!argv) {
    return false;
  }
  bool is_report = false;
  for (int i = 1; i < argc; ++i) {
    if (_wcsicmp(argv[i], L"--crash-report") == 0) {
      is_report = true;
      break;
    }
  }
  if (is_report) {
    ShowCrashPopup(argc, argv);
    LocalFree(argv);
    ExitProcess(0);
  }
  LocalFree(argv);
  return false;
}();

}  // namespace

#endif  // _WIN32
