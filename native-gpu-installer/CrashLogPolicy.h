#pragma once
#include <string>
#include <string_view>

// The SDK's WriteDumpImpl logs [FATAL] for manual console dumps as well as
// crash dumps. A dump-result notification alone is not a crash indication.
class FatalLogTail {
  std::string pending;
public:
  void Reset() { pending.clear(); }
  bool Consume(std::string_view bytes) {
    pending.append(bytes);
    bool fatal = false;
    size_t consumed = 0;
    for (size_t end; (end = pending.find('\n', consumed)) != std::string::npos; consumed = end + 1) {
      const std::string_view line(pending.data() + consumed, end - consumed);
      const auto marker = line.find("[FATAL]");
      if (marker == std::string_view::npos) continue;
      const auto message = line.substr(marker);
      constexpr std::string_view written = "[FATAL] Minidump written to ";
      constexpr std::string_view failed = "[FATAL] Minidump failed (";
      if (message.substr(0, written.size()) == written ||
          message.substr(0, failed.size()) == failed) continue;
      fatal = true;
    }
    pending.erase(0, consumed);
    return fatal;
  }
};
