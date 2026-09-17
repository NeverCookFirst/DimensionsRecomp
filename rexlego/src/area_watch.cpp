// legodimensions - ReXGlue Recompiled Project
//
// See area_watch.h for why this wraps a guest accessor instead of hunting for
// a global.
//
// How the call is intercepted matters: the recompiled code calls sub_82691090
// DIRECTLY by symbol, so replacing it in the function dispatcher does nothing -
// that table only backs indirect calls. The generated definition is a WEAK
// alias precisely so a hook can take the name at link time, with __imp__<name>
// left as the original entry point. That is what REX_HOOK_RAW does, and it is
// the only thing that catches a direct bl. A dispatcher SetFunction here logs a
// cheerful "installed" and then never fires.
//
// The one thing that could not be read off the disc is where the area's name
// sits inside the area record, so it is not hardcoded: the first time a record
// is seen, every word of it is tried as a pointer to a string, and the offset
// that yields a name from levels\areas.txt is the one. That check is what makes
// a wrong guess impossible - a random pointer into the heap will not spell
// "LEVEL1_WIZARDOFOZ". The offset is found once and reused after that.

#include "area_watch.h"

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

// The untouched original. The generated alias is weak, so defining
// sub_82691090 below takes the name for us and this stays the real body.
extern "C" void __imp__sub_82691090(PPCContext& __restrict ctx, uint8_t* base);

#include <algorithm>
#include <atomic>
#include <array>
#include <cctype>
#include <cstring>
#include <mutex>
#include <string_view>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <fmt/ranges.h>

namespace legodimensions::area_watch {
namespace {

// How far into the area record to look for the name. The flags the portrait
// patch reads live at +152, so the record is at least that big; 1 KiB is room
// to spare without ever walking off a small object.
constexpr uint32_t kMaxProbeOffset = 2048;
constexpr uint32_t kMaxNameLength = 63;

// Area names from levels\areas.txt in GAME.DAT. Only used to recognise a real
// name during the one-time probe, so it does not have to be exhaustive - the
// DLC worlds add their own areas and are deliberately not listed.
constexpr std::array<std::string_view, 10> kKnownAreaNames = {
    "HUBS",           "TardisInterior",    "LEVEL1_WIZARDOFOZ",
    "LEVEL2_THESIMPSONS", "LEVEL4_DOCTORWHO", "LEVEL5_METROPOLIS",
    "LEVEL8_MIDDLEEARTH", "LEVEL9_GHOSTBUSTERS", "LEVEL11_SCOOBYDOO",
    "LEVEL15_FINALDIMENSION"};


// Internal name -> what a person should see. Deliberately not a prettifier:
// "LEVEL3_MASTERCHENSISLAND" cannot be turned into "Master Chen's Island" by
// rule, and a half-right guess in someone's Discord profile is worse than the
// generic line. An area missing from here shows the fallback text instead.
//
// The hub's per-franchise event areas map to the franchise: standing in the
// LOTR corner of the hub reads as Middle-earth, which is what the player sees.
constexpr std::array<std::pair<std::string_view, std::string_view>, 31> kAreaDisplayNames = {{
    {"HUBS", "Vorton"},
    {"TardisInterior", "The TARDIS"},
    {"LOTR_Dummy_Event", "Middle-earth"},
    {"Movie_DummyEvent", "The LEGO Movie"},
    {"DC_DummyEvent", "DC Comics"},
    {"Ninjago_DummyEvent", "Ninjago"},
    {"Simpsons_DummyEvent", "Springfield"},
    {"Scooby_DummyEvent", "Scooby-Doo"},
    {"Chima_DummyEvent", "Chima"},
    {"Portal_DummyEvent", "Portal 2"},
    {"Oz_DummyEvent", "The Land of Oz"},
    {"BTTF_Past_DummyEvent", "Hill Valley, 1955"},
    {"BTTF_Pres_DummyEvent", "Hill Valley, 1985"},
    {"BTTF_Future_DummyEvent", "Hill Valley, 2015"},
    {"LEVEL1_WIZARDOFOZ", "Follow the LEGO Brick Road"},
    {"LEVEL2_THESIMPSONS", "Meltdown at Sector 7-G"},
    {"LEVEL3_MASTERCHENSISLAND", "Elements of Surprise"},
    {"LEVEL4_DOCTORWHO", "Once Upon a Time Machine in the West"},
    {"LEVEL5_METROPOLIS", "Painting the Town Black"},
    {"LEVEL6_THE_WILD_WEST", "Riddle-earth"},
    {"LEVEL7_APERTURE_LABS", "Aperture Science"},
    {"LEVEL8_MIDDLEEARTH", "Middle-earth"},
    {"LEVEL9_GHOSTBUSTERS", "Eight Course Meal"},
    {"LEVEL10_RETROGAMES", "Mystery Mansion Mash-Up"},
    {"LEVEL11_SCOOBYDOO", "A Dalek Christmas Carol"},
    {"LEVEL12_FOUNDATIONPRIME", "Prime Time"},
    {"LEVEL13_THETRI", "The Final Dimension"},
    {"LEVEL15_FINALDIMENSION", "The Final Dimension"},
    {"DEMO", "Demo"},
    {"LEVEL_BONUS", "Bonus Level"},
    {"DLC1", "Adventure World"},
}};

std::string_view LookUpDisplayName(std::string_view internal) {
  for (const auto& [key, shown] : kAreaDisplayNames) {
    if (key == internal) {
      return shown;
    }
  }
  return {};
}

// Written on the guest thread, read by the Discord thread.
std::atomic<uint32_t> g_current_area{0};

// -1 until the probe has succeeded. Once set it is never re-probed.
std::atomic<int32_t> g_name_offset{-1};
// Set when the name is stored in the record itself rather than behind a pointer.
std::atomic<bool> g_name_is_inline{false};

std::mutex g_name_lock;
std::string g_current_name;

uint8_t* GuestBase() {
  auto* ks = rex::system::kernel_state();
  return ks ? ks->memory()->virtual_membase() : nullptr;
}

// Is this guest address actually backed by committed memory?
//
// A range check is NOT enough, and assuming it was crashed the game: a record
// is full of floats, and 0x3DCCCCCD - plain 0.1f - sits comfortably inside any
// plausible-looking address window. Following it read unmapped memory and took
// the process down. So ask the memory system instead of guessing: a value only
// gets dereferenced if the page it points at exists.
bool IsReadableGuestAddress(uint32_t address) {
  if (address < 0x00010000u) {
    return false;
  }
  auto* ks = rex::system::kernel_state();
  auto* memory = ks ? ks->memory() : nullptr;
  if (!memory) {
    return false;
  }
  auto* heap = memory->LookupHeap(address);
  if (!heap) {
    return false;
  }
  uint32_t protect = 0;
  return heap->QueryProtect(address, &protect) && protect != 0;
}

// Both ends, because a string may start on a committed page and run off it.
bool IsReadableGuestRange(uint32_t address, uint32_t length) {
  return IsReadableGuestAddress(address) && IsReadableGuestAddress(address + length - 1);
}

// Reads a NUL-terminated ASCII identifier. Empty when it is not one, which is
// the common case and the whole point: it rejects the noise.
std::string ReadIdentifier(uint8_t* base, uint32_t address) {
  std::string out;
  for (uint32_t i = 0; i < kMaxNameLength; i++) {
    const char c = static_cast<char>(base[address + i]);
    if (c == '\0') {
      break;
    }
    const bool ok = std::isalnum(static_cast<unsigned char>(c)) || c == '_';
    if (!ok) {
      return {};
    }
    out.push_back(c);
  }
  return out.size() >= 3 ? out : std::string{};
}

bool IsKnownAreaName(std::string_view name) {
  return std::find(kKnownAreaNames.begin(), kKnownAreaNames.end(), name) !=
         kKnownAreaNames.end();
}

// Walks the record once looking for an offset that spells a known area name,
// either directly or through a pointer. Returns true when it locked on.
bool ProbeNameOffset(uint8_t* base, uint32_t area) {
  // Everything that looked like a name but was not one of the known ones, kept
  // so a failed probe can say what it DID see. Without this a layout we guessed
  // wrong about is silent, and silence is indistinguishable from "not in an
  // area yet".
  std::vector<std::string> candidates;

  for (uint32_t offset = 0; offset < kMaxProbeOffset; offset += 4) {
    uint32_t word = 0;
    std::memcpy(&word, base + area + offset, sizeof(word));
    word = __builtin_bswap32(word);

    if (IsReadableGuestRange(word, kMaxNameLength + 1)) {
      const std::string name = ReadIdentifier(base, word);
      if (!name.empty() && !IsKnownAreaName(name) && candidates.size() < 24) {
        candidates.push_back(fmt::format("+{}->{}", offset, name));
      }
      if (!name.empty() && IsKnownAreaName(name)) {
        g_name_is_inline.store(false, std::memory_order_relaxed);
        g_name_offset.store(static_cast<int32_t>(offset), std::memory_order_release);
        REXLOG_INFO("Area: name found at +{} through a pointer (\"{}\").", offset, name);
        return true;
      }
    }

    const std::string inln = ReadIdentifier(base, area + offset);
    if (!inln.empty() && !IsKnownAreaName(inln) && candidates.size() < 24) {
      candidates.push_back(fmt::format("+{}={}", offset, inln));
    }
    if (!inln.empty() && IsKnownAreaName(inln)) {
      g_name_is_inline.store(true, std::memory_order_relaxed);
      g_name_offset.store(static_cast<int32_t>(offset), std::memory_order_release);
      REXLOG_INFO("Area: name stored in the record at +{} (\"{}\").", offset, inln);
      return true;
    }
  }

  // Report once. Repeating this for every call would flood the log, and the
  // first record tells us as much as the hundredth.
  static std::atomic<bool> reported{false};
  if (!reported.exchange(true)) {
    REXLOG_WARN("Area: no known area name in the record at 0x{:08X}. Strings seen: {}",
                area, candidates.empty() ? std::string("none") : fmt::format("{}", fmt::join(candidates, " ")));

    // With no strings to go on, the raw words are the only evidence left, so
    // print them rather than making the next run guess again. Two hundred and
    // fifty-six bytes is enough to see the shape of the object and to spot the
    // flags word at +152 that says this really is an area.
    std::string dump;
    for (uint32_t offset = 0; offset < 256; offset += 4) {
      uint32_t word = 0;
      std::memcpy(&word, base + area + offset, sizeof(word));
      dump += fmt::format("+{}:{:08X} ", offset, __builtin_bswap32(word));
    }
    REXLOG_WARN("Area: record dump {}", dump);
  }
  return false;
}

std::string ReadAreaName(uint8_t* base, uint32_t area) {
  const int32_t offset = g_name_offset.load(std::memory_order_acquire);
  if (offset < 0) {
    return {};
  }
  if (g_name_is_inline.load(std::memory_order_relaxed)) {
    return ReadIdentifier(base, area + static_cast<uint32_t>(offset));
  }
  uint32_t pointer = 0;
  std::memcpy(&pointer, base + area + static_cast<uint32_t>(offset), sizeof(pointer));
  pointer = __builtin_bswap32(pointer);
  return IsReadableGuestRange(pointer, kMaxNameLength + 1) ? ReadIdentifier(base, pointer)
                                                           : std::string{};
}

// Everything the hook does once the original has run. Kept out of the hook so
// the hot path stays a call and a compare.
void ObserveArea(uint32_t area, uint8_t* base) {
  if (!area || !IsReadableGuestRange(area, kMaxProbeOffset)) {
    return;
  }
  if (g_current_area.exchange(area, std::memory_order_relaxed) == area &&
      g_name_offset.load(std::memory_order_acquire) >= 0) {
    // Same area as last time and the layout is known: nothing to do. This is
    // the path taken on almost every call, and it must stay cheap - the game
    // asks for the area many times a frame.
    return;
  }

  if (g_name_offset.load(std::memory_order_acquire) < 0 && !ProbeNameOffset(base, area)) {
    return;
  }

  std::string name = ReadAreaName(base, area);
  if (name.empty()) {
    return;
  }

  std::lock_guard<std::mutex> lock(g_name_lock);
  if (g_current_name != name) {
    REXLOG_INFO("Area: now in \"{}\".", name);
    g_current_name = std::move(name);
  }
}

}  // namespace

// Bridges the C hook below into the file's internals. Not in the header: only
// the hook calls it.
void ObserveAreaFromHook(uint32_t area, uint8_t* base) { ObserveArea(area, base); }

void Install() {
  // Nothing to install: the hook below is a link-time symbol override and is
  // already live. This only says so, because a silent subsystem is impossible
  // to tell from a broken one in a log.
  REXLOG_INFO("Area: hooked sub_82691090; the current area will be tracked.");
}

uint32_t CurrentArea() { return g_current_area.load(std::memory_order_relaxed); }

std::string CurrentAreaName() {
  std::lock_guard<std::mutex> lock(g_name_lock);
  return g_current_name;
}

std::string CurrentAreaDisplayName() {
  std::string internal = CurrentAreaName();
  if (internal.empty()) {
    return {};
  }
  const std::string_view shown = LookUpDisplayName(internal);
  return std::string(shown);
}

}  // namespace legodimensions::area_watch

// The hook itself. Outside the anonymous namespace and outside the project
// namespace: it must define the exact C symbol the recompiled code calls.
REX_HOOK_RAW(sub_82691090) {
  __imp__sub_82691090(ctx, base);
  legodimensions::area_watch::ObserveAreaFromHook(ctx.r3.u32, base);
}
