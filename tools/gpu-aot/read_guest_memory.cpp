// Read-only, bounded live guest-object snapshots for renderer diagnostics.
// No writes, suspension, input, window activation or remote execution.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <array>
#include <string>

int wmain(int argc, wchar_t** argv) {
  if (argc < 6) {
    std::cerr << "usage: reader PID EXPECTED_EXE HOST_GUEST_BASE OUTPUT_DIR GUEST_ADDRESS...\n";
    return 2;
  }
  try {
    const auto pid = std::stoull(argv[1], nullptr, 0);
    const auto base = std::stoull(argv[3], nullptr, 0);
    if (!pid || pid > MAXDWORD || argc > 37) return 2;
    HANDLE process = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_LIMITED_INFORMATION,
                                 FALSE, DWORD(pid));
    if (!process) return 3;
    struct Close { HANDLE handle; ~Close() { CloseHandle(handle); } } close{process};
    std::array<wchar_t, 32768> path{};
    DWORD count = DWORD(path.size());
    if (!QueryFullProcessImageNameW(process, 0, path.data(), &count) ||
        !std::filesystem::equivalent(path.data(), argv[2])) {
      std::cerr << "Process path mismatch\n"; return 4;
    }
    std::filesystem::create_directories(argv[4]);
    bool failed = false;
    for (int i = 5; i < argc; ++i) {
      const auto address = std::stoull(argv[i], nullptr, 0);
      constexpr size_t length = 1024;
      if (!address || address > 0x100000000ull - length ||
          base > UINT64_MAX - address - length) return 2;
      std::array<char, length> bytes{};
      SIZE_T read = 0;
      const bool complete = ReadProcessMemory(process,
          reinterpret_cast<const void*>(base + address), bytes.data(), bytes.size(), &read);
      if (!complete || read != length) {
        std::cerr << "Read failed guest=" << std::hex << address << " bytes=" << read << '\n';
        failed = true; continue;
      }
      const auto file = std::filesystem::path(argv[4]) /
          (L"guest-" + std::to_wstring(address) + L".bin");
      std::ofstream output(file, std::ios::binary);
      output.write(bytes.data(), bytes.size());
      if (!output) { failed = true; continue; }
      std::cout << "Saved guest=" << std::hex << address << " bytes=" << std::dec << read << '\n';
    }
    return failed ? 5 : 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n'; return 2;
  }
}
