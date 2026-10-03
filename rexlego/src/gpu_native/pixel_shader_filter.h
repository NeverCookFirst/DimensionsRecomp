#pragma once
#include <charconv>
#include <cctype>
#include <cstdint>
#include <string_view>
#include <unordered_set>

namespace legodimensions::gpu_native {
inline std::unordered_set<uint64_t> ParsePixelShaderFilter(std::string_view list) {
  std::unordered_set<uint64_t> hashes;
  while (!list.empty()) {
    const auto comma = list.find(',');
    auto token = list.substr(0, comma);
    while (!token.empty() && std::isspace(static_cast<unsigned char>(token.front()))) token.remove_prefix(1);
    while (!token.empty() && std::isspace(static_cast<unsigned char>(token.back()))) token.remove_suffix(1);
    if (token.starts_with("0x") || token.starts_with("0X")) token.remove_prefix(2);
    uint64_t value = 0;
    const auto result = std::from_chars(token.data(), token.data() + token.size(), value, 16);
    if (!token.empty() && result.ec == std::errc{} && result.ptr == token.data() + token.size())
      hashes.insert(value);
    if (comma == std::string_view::npos) break;
    list.remove_prefix(comma + 1);
  }
  return hashes;
}
}  // namespace legodimensions::gpu_native
