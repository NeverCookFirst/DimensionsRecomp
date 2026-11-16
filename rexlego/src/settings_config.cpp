#include "settings_config.h"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <toml++/toml.hpp>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace legodimensions::settings {
namespace {
std::mutex save_mutex;
std::atomic<uint64_t> temp_sequence{};

std::string ReadConfig(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) throw std::runtime_error("Could not read existing settings file");
  std::ostringstream bytes;
  bytes << input.rdbuf();
  if (input.bad()) throw std::runtime_error("Could not read all settings");
  return bytes.str();
}

void Insert(toml::table& table, const ConfigChange& change) {
  if (change.key.empty() || change.key.find_first_not_of(
      "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_") != std::string::npos)
    throw std::runtime_error("Invalid settings key");
  if (const auto* existing = table.get(change.key);
      existing && (existing->is_table() || existing->is_array()))
    throw std::runtime_error("Setting conflicts with an existing table or array: " + change.key);
  const auto& value = change.value;
  switch (change.type) {
    case ConfigType::kBoolean:
      if (value != "true" && value != "false")
        throw std::runtime_error("Invalid Boolean: " + change.key);
      table.insert_or_assign(change.key, value == "true");
      break;
    case ConfigType::kInteger: {
      int64_t number{};
      auto result = std::from_chars(value.data(), value.data() + value.size(), number);
      if (result.ec != std::errc{} || result.ptr != value.data() + value.size())
        throw std::runtime_error("Invalid integer: " + change.key);
      table.insert_or_assign(change.key, number);
      break;
    }
    case ConfigType::kFloat: {
      double number{};
      auto result = std::from_chars(value.data(), value.data() + value.size(), number);
      if (result.ec != std::errc{} || result.ptr != value.data() + value.size() || !std::isfinite(number))
        throw std::runtime_error("Invalid floating point value: " + change.key);
      table.insert_or_assign(change.key, number);
      break;
    }
    case ConfigType::kString:
      table.insert_or_assign(change.key, value);
      break;
    default:
      throw std::runtime_error("Unknown settings type: " + change.key);
  }
}

// Owns only the exclusively created temporary file; never removes the target.
struct TempFile {
  std::filesystem::path path;
#ifdef _WIN32
  HANDLE handle = INVALID_HANDLE_VALUE;
  void Close() { if (handle != INVALID_HANDLE_VALUE) { CloseHandle(handle); handle = INVALID_HANDLE_VALUE; } }
#else
  int handle = -1;
  void Close() { if (handle != -1) { close(handle); handle = -1; } }
#endif
  ~TempFile() {
    Close();
    if (!path.empty()) { std::error_code ignored; std::filesystem::remove(path, ignored); }
  }
};

void Publish(const std::filesystem::path& target, const std::string& bytes) {
  TempFile temp;
  // Unique, exclusive creation in the destination directory keeps the final
  // rename on one filesystem. Collisions never truncate someone else's file.
  for (unsigned attempt = 0; attempt != 64; ++attempt) {
    auto candidate = target;
#ifdef _WIN32
    candidate += L".settings-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                 std::to_wstring(temp_sequence.fetch_add(1)) + L".tmp";
    auto handle = CreateFileW(candidate.c_str(), GENERIC_WRITE, 0, nullptr,
                              CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle != INVALID_HANDLE_VALUE) { temp.path = candidate; temp.handle = handle; break; }
    if (GetLastError() != ERROR_FILE_EXISTS && GetLastError() != ERROR_ALREADY_EXISTS)
      throw std::system_error(GetLastError(), std::system_category(), "Create settings temporary file");
#else
    candidate += ".settings-" + std::to_string(getpid()) + "-" +
                 std::to_string(temp_sequence.fetch_add(1)) + ".tmp";
    auto handle = open(candidate.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (handle != -1) { temp.path = candidate; temp.handle = handle; break; }
    if (errno != EEXIST) throw std::system_error(errno, std::generic_category(), "Create settings temporary file");
#endif
  }
  if (temp.path.empty()) throw std::runtime_error("Could not allocate a settings temporary file");
  size_t offset = 0;
  while (offset != bytes.size()) {
#ifdef _WIN32
    DWORD written = 0;
    const auto chunk = static_cast<DWORD>(std::min<size_t>(bytes.size() - offset, 1u << 20));
    if (!WriteFile(temp.handle, bytes.data() + offset, chunk, &written, nullptr) || !written)
      throw std::system_error(GetLastError(), std::system_category(), "Write settings");
#else
    auto written = write(temp.handle, bytes.data() + offset, bytes.size() - offset);
    if (written < 0 && errno == EINTR) continue;
    if (written <= 0) throw std::system_error(errno, std::generic_category(), "Write settings");
#endif
    offset += written;
  }
#ifdef _WIN32
  if (!FlushFileBuffers(temp.handle))
    throw std::system_error(GetLastError(), std::system_category(), "Flush settings");
  temp.Close();
  if (!MoveFileExW(temp.path.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    throw std::system_error(GetLastError(), std::system_category(), "Replace settings");
#else
  if (fsync(temp.handle) != 0)
    throw std::system_error(errno, std::generic_category(), "Flush settings");
  temp.Close();
  if (rename(temp.path.c_str(), target.c_str()) != 0)
    throw std::system_error(errno, std::generic_category(), "Replace settings");
  // Rename is committed. A directory sync failure cannot be reported as an
  // unchanged destination; best-effort durability is separate from atomicity.
  const auto directory = target.parent_path().empty() ? std::filesystem::path(".") : target.parent_path();
  const int directory_fd = open(directory.c_str(), O_RDONLY | O_DIRECTORY);
  if (directory_fd != -1) { fsync(directory_fd); close(directory_fd); }
#endif
  temp.path.clear();
}
}  // namespace

bool SaveConfigPatch(const std::filesystem::path& path,
                     std::span<const ConfigChange> changes, std::string& error) {
  std::lock_guard lock(save_mutex);
  error.clear();
  if (changes.empty()) return true;
  try {
    const bool existed = std::filesystem::exists(path);
    const auto original = existed ? ReadConfig(path) : std::string{};
    auto table = toml::parse(original);
    std::set<std::string, std::less<>> seen;
    for (const auto& change : changes) {
      if (!seen.insert(change.key).second) throw std::runtime_error("Duplicate setting: " + change.key);
      Insert(table, change);
    }
    std::ostringstream output;
    output << toml::toml_formatter{table} << '\n';
    // Catch edits while preparing the patch. This is not a cross-process lock;
    // the caller must serialize the application's own config writers too.
    if (std::filesystem::exists(path) != existed || (existed && ReadConfig(path) != original))
      throw std::runtime_error("Settings file changed while saving; retry");
    Publish(path, output.str());
    return true;
  } catch (const std::exception& exception) {
    error = exception.what();
    return false;
  }
}

}  // namespace legodimensions::settings
