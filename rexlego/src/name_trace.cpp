// legodimensions - ReXGlue Recompiled Project
//
// trace_file_names: logs the file names the game hashes to look them up in
// its archive indexes. The reads trace (trace_file_reads) only sees files that
// were FOUND; when a character never loads, what we need is the name the game
// asked for and did not find.
//
// sub_82A3D980 is TT's path hash: FNV-1a (seed 0x811C9DC5, prime 0x199933)
// over a lowercased path with '/' folded to '\'. It takes a string record in
// r3 - char* at +0, s16 length at +6 - and returns the hash in r3.

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>

extern "C" void __imp__sub_82A3D980(PPCContext& __restrict ctx, uint8_t* base);

REXCVAR_DEFINE_STRING(trace_file_names, "", "Runtime",
                      "Log every archive name lookup containing one of these comma-separated "
                      "substrings (case-insensitive, e.g. fern,finn), with its hash. Chatty.");

REX_HOOK_RAW(sub_82A3D980) {
  const std::string& filter = REXCVAR_GET(trace_file_names);
  std::string name;
  if (!filter.empty()) {
    uint32_t record = ctx.r3.u32;
    uint32_t ptr;
    uint16_t len;
    std::memcpy(&ptr, base + record, 4);
    std::memcpy(&len, base + record + 6, 2);
    ptr = __builtin_bswap32(ptr);
    len = __builtin_bswap16(len);
    if (ptr && len > 0 && len < 512) {
      name.assign(reinterpret_cast<const char*>(base + ptr), len);
      std::transform(name.begin(), name.end(), name.begin(),
                     [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    }
  }
  __imp__sub_82A3D980(ctx, base);
  if (name.empty()) return;
  size_t at = 0;
  while (at <= filter.size()) {
    size_t comma = filter.find(',', at);
    if (comma == std::string::npos) comma = filter.size();
    std::string token = filter.substr(at, comma - at);
    std::transform(token.begin(), token.end(), token.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (!token.empty() && name.find(token) != std::string::npos) {
      REXLOG_INFO("[name] {:08X} {}", ctx.r3.u32, name);
      return;
    }
    at = comma + 1;
  }
}
