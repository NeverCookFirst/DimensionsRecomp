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
#include <vector>

int wmain(int argc, wchar_t** argv) {
  if (argc < 6) {
    std::cerr << "usage: reader PID EXPECTED_EXE HOST_GUEST_BASE OUTPUT_DIR [--bytes LENGTH] GUEST_ADDRESS...\n";
    return 2;
  }
  try {
    const auto pid = std::stoull(argv[1], nullptr, 0);
    const auto base = std::stoull(argv[3], nullptr, 0);
    int first_address = 5;
    size_t length = 1024;
    if (std::wstring(argv[5]) == L"--bytes") {
      if (argc < 8) return 2;
      const auto requested = std::stoull(argv[6], nullptr, 0);
      if (!requested || requested > 8ull * 1024 * 1024) return 2;
      length = size_t(requested);
      first_address = 7;
    }
    if (!pid || pid > MAXDWORD || argc - first_address > 32 ||
        uint64_t(length) * (argc - first_address) > 128ull * 1024 * 1024) return 2;
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
    for (int i = first_address; i < argc; ++i) {
      const auto address = std::stoull(argv[i], nullptr, 0);
      // Match Memory::TranslateVirtual for Windows physical heap aliases.
      // MapViewOfFileEx rounds the E/F heap's 4KB backing offset down to64KB.
      const uint64_t host_offset = address >= 0xE0000000ull ? 0x1000ull : 0;
      if (!address || address > 0x100000000ull - length ||
          base > UINT64_MAX - address - host_offset - length) return 2;
      std::vector<char> bytes(length);
      SIZE_T read = 0;
      const bool complete = ReadProcessMemory(process,
          reinterpret_cast<const void*>(base + address + host_offset), bytes.data(), bytes.size(), &read);
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
