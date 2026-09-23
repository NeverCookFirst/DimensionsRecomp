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
#include <rex/system/function_dispatcher.h>
#include <rex/system/kernel_state.h>

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

// Research: who asks for the "update required" tag message and with what.
// sub_83470AF8(this, state, value) shows TAG_REQUIRES_PATCH when value == -1;
// sub_834B8168(this, value, state) is its caller for tags.
REXCVAR_DEFINE_BOOL(trace_tag_prompt, false, "Runtime",
                    "Log the arguments and caller of the toy-tag prompt functions.");
extern "C" void __imp__sub_83470AF8(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_834B8168(PPCContext& __restrict ctx, uint8_t* base);
REX_HOOK_RAW(sub_83470AF8) {
  if (REXCVAR_GET(trace_tag_prompt)) {
    REXLOG_INFO("[tagprompt] 83470AF8 state={} value={:#x} lr={:#x}", (int32_t)ctx.r4.u32,
                ctx.r5.u32, (uint32_t)ctx.lr);
  }
  __imp__sub_83470AF8(ctx, base);
}
REX_HOOK_RAW(sub_834B8168) {
  if (REXCVAR_GET(trace_tag_prompt)) {
    REXLOG_INFO("[tagprompt] 834B8168 value={:#x} state={} lr={:#x}", ctx.r4.u32,
                (int32_t)ctx.r5.u32, (uint32_t)ctx.lr);
  }
  __imp__sub_834B8168(ctx, base);
}

// sub_836F5AD0(mgr, tag_id) is a hard-coded switch mapping a character tag id
// to the content pack it needs; 73 (Lord Vortech) falls to the default -1,
// which is what shows "An update is required". Give him Batman Excalibur's
// pack (id 69 -> 20), the wave he was listed with in dlccharacters.txt.
REXCVAR_DEFINE_BOOL(vortech_tag, false, "Runtime",
                    "Treat the Lord Vortech tag (id 73) as known content (experiment).");
extern "C" void __imp__sub_836F5AD0(PPCContext& __restrict ctx, uint8_t* base);
REX_HOOK_RAW(sub_836F5AD0) {
  const int32_t id = static_cast<int32_t>(ctx.r4.u32);
  if (REXCVAR_GET(vortech_tag)) REXLOG_INFO("[vortech] 836F5AD0 id={}", id);
  __imp__sub_836F5AD0(ctx, base);
  if (id == 73 && REXCVAR_GET(vortech_tag)) {
    REXLOG_INFO("[vortech] tag 73 -> content {} (was {})", 20, (int32_t)ctx.r3.u32);
    ctx.r3.u64 = 20;
  }
}

// The character manager is *(0x848BFE74); sub_83715F00 asks its vtable slot
// +180 for the character behind a tag id and gets -1 for Lord Vortech.
REXCVAR_DEFINE_BOOL(trace_tag_lookup, false, "Runtime",
                    "Once: log which function the character manager uses to map a tag id.");
namespace {
uint32_t ReadBE32(uint8_t* base, uint32_t addr) {
  uint32_t v;
  std::memcpy(&v, base + addr, 4);
  return __builtin_bswap32(v);
}
}  // namespace
extern "C" void __imp__sub_83715F00(PPCContext& __restrict ctx, uint8_t* base);
REX_HOOK_RAW(sub_83715F00) {
  static bool installed = false;
  if (!installed && REXCVAR_GET(vortech_tag)) {
    installed = true;
    auto* ks = rex::system::kernel_state();
    auto* d = ks ? ks->function_dispatcher() : nullptr;
    // Reached through a vtable, so the link-time hook alone never fires.
    bool ok = d && d->SetFunction(0x836F5AD0, sub_836F5AD0);
    REXLOG_INFO("[vortech] dispatcher hook on 836F5AD0: {}", ok);
  }
  static bool done = false;
  if (!done && REXCVAR_GET(trace_tag_lookup)) {
    uint32_t mgr = ReadBE32(base, 0x848BFE74);
    if (mgr) {
      uint32_t vt = ReadBE32(base, mgr);
      REXLOG_INFO("[taglookup] manager={:#x} vtable={:#x} slot36={:#x} slot180={:#x}", mgr, vt,
                  ReadBE32(base, vt + 36), ReadBE32(base, vt + 180));
      done = true;
    }
  }
  __imp__sub_83715F00(ctx, base);
}

