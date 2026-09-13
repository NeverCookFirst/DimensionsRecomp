// Investigation aid for the flat-vs-3D character portrait.
//
// The HUD portrait is a live 3D render of the character inside a level, but a
// flat texture from icons\characters\<name>.tex in the hub and the open
// worlds. The switch is bit 0x20000 of the area flags: the areas.txt parser
// turns the keyword `hub_area` into `flags &= ~0x20000`, and editing that word
// out of areas.txt does give the hub a 3D portrait - at the cost of breaking
// the world portals and the shared stud pool, which read the same bit.
//
// Writing the bit into guest memory after the fact changes nothing at all, so
// the flag is consumed once while an area loads and its answer is kept
// somewhere else. That is also why a watch taken while standing still in the
// hub finds twenty-odd readers of the flags word and none that tests 0x20000:
// by then the interesting read is long done. Catch a load, not a lull.
//
// The flags live in guest memory at a known address, so their page is armed
// with PAGE_GUARD and every access is caught. Windows hands the handler both
// the faulting instruction and the address it touched, so accesses to whatever
// else shares the page are dropped and only true reads of the flags word are
// kept - which is what makes this precise enough to be worth doing.
//
// Addresses are recorded as RVAs and symbolised offline against the PDB; the
// handler itself only writes into a fixed array, because taking the logger's
// lock inside an exception handler is a good way to deadlock the game.
//
// Delete this file and its CMakeLists entry once the reader is known.

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>

#ifdef _WIN32
#include <windows.h>
#endif

// Makes sub_82E9FB80 answer yes. That function is the only place found so far
// that turns the level bit into a plain yes/no - it reads the copy of the area
// flags kept at +56 of a live object and returns 1 when 0x20000 is set, the
// caller's default otherwise. Whether the portrait is what asks is exactly the
// open question; this switch exists to answer it.
//
// Writing the bit into the area object itself was tried first and does nothing
// at all, which is why that code is gone: the flag is consumed while an area
// loads, and by the time anything can be typed it has already been turned into
// answers like this one.
REXCVAR_DEFINE_BOOL(hub_3d_portrait, false, "Icon probe",
                    "Force the level-bit question to answer yes (experiment)");

// Arms the watch as soon as the areas exist, which is early enough to catch
// the first area load of the session. The read we are after happens during a
// load, so leaving the arming to a person means racing a loading screen.
REXCVAR_DEFINE_BOOL(icon_probe_autowatch, false, "Icon probe",
                    "Arm the area-flag watch automatically at startup");

// The exhaustive code sweep says nothing branches on the level bit except one
// function that turned out not to be the portrait, so the bit is consumed while
// an area loads and turned into state elsewhere. The watch already catches every
// instruction that reads the flags during a load - 35 of them - and knows which
// one is faulting, so the bit can be handed to a chosen few and withheld from
// the rest. List their RVAs here (hex, comma separated, as WatchStop prints
// them) and only those reads see a level. Halve the list between runs and six
// runs find the reader that owns the portrait.
REXCVAR_DEFINE_STRING(icon_probe_yes_rvas, "", "Icon probe",
                      "RVAs that should read the level bit as set, comma separated hex");

namespace {

// Offset of the u64 area-flags word inside the area object, as used by both
// the areas.txt parser and every area flag accessor.
constexpr uint32_t kAreaFlagsOffset = 152;
// The flags are big-endian, so the bit lives in the low half, at +156.
constexpr uint32_t kAreaFlagsLowOffset = kAreaFlagsOffset + 4;
constexpr uint32_t kLevelBit = 0x20000;

// Every area object an accessor has been called with. The game keeps dozens
// alive at once - one per area_start block in areas.txt - and walks the whole
// set in a single frame, so remembering only the latest one points at an
// arbitrary area rather than the one being played.
std::mutex g_areas_mutex;
std::unordered_set<uint32_t> g_areas;
// Cheap guard so the common repeat call does not touch the mutex at all.
std::atomic<uint32_t> g_last_seen{0};

uint8_t* GuestBase() {
  auto* ks = rex::system::kernel_state();
  return ks ? ks->memory()->virtual_membase() : nullptr;
}

uint32_t LoadBE32(const uint8_t* p) {
  return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

void StoreBE32(uint8_t* p, uint32_t v) {
  p[0] = static_cast<uint8_t>(v >> 24);
  p[1] = static_cast<uint8_t>(v >> 16);
  p[2] = static_cast<uint8_t>(v >> 8);
  p[3] = static_cast<uint8_t>(v);
}

void MaybeAutoWatch(size_t known);

// Called from the flag accessors, i.e. very often - the atomic short-circuits
// the repeat calls and the mutex only sees genuinely new areas.
//
// When the force switch is on the bit is re-applied here rather than once from
// a command: the game rebuilds area objects on every load, so a one-shot write
// only lasts until the next loading screen.
void NoteArea(uint32_t area) {
  if (area == 0) {
    return;
  }
  if (g_last_seen.exchange(area) == area) {
    return;
  }
  size_t known = 0;
  {
    std::lock_guard<std::mutex> lock(g_areas_mutex);
    g_areas.insert(area);
    known = g_areas.size();
  }
  MaybeAutoWatch(known);
}

void SetLevelBit(bool on) {
  uint8_t* base = GuestBase();
  if (base == nullptr) {
    return;
  }
  std::lock_guard<std::mutex> lock(g_areas_mutex);
  if (g_areas.empty()) {
    REXLOG_WARN("[icon-probe] no area captured yet - load a level or a hub first");
    return;
  }
  int changed = 0;
  for (uint32_t area : g_areas) {
    uint8_t* p = base + area + kAreaFlagsLowOffset;
    const uint32_t before = LoadBE32(p);
    const uint32_t after = on ? (before | kLevelBit) : (before & ~kLevelBit);
    if (after != before) {
      StoreBE32(p, after);
      changed++;
    }
  }
  REXLOG_INFO("[icon-probe] level bit {} on {} of {} area(s)", on ? "set" : "cleared", changed,
              g_areas.size());
}

// Read-only: what the areas look like right now, to check the probe is aimed
// at the right structure before trusting anything it reports.
void DumpAreas() {
  uint8_t* base = GuestBase();
  if (base == nullptr) {
    return;
  }
  std::lock_guard<std::mutex> lock(g_areas_mutex);
  int set = 0;
  for (uint32_t area : g_areas) {
    if (LoadBE32(base + area + kAreaFlagsLowOffset) & kLevelBit) {
      set++;
    }
  }
  REXLOG_INFO("[icon-probe] {} area(s) known, level bit set on {}", g_areas.size(), set);
}

#ifdef _WIN32

//=============================================================================
// Guard-page watch on the area flags
//=============================================================================

constexpr size_t kMaxHits = 512;

struct Hit {
  uint64_t rva;       // faulting instruction, relative to the module base
  uint32_t accessed;  // guest address of the byte touched
};

std::atomic<bool> g_watch_active{false};
Hit g_hits[kMaxHits];
std::atomic<size_t> g_hit_count{0};
// Distinct RVAs only; the same instruction fires thousands of times a second.
std::mutex g_hits_mutex;
std::unordered_set<uint64_t> g_seen_rvas;

struct Range {
  uintptr_t lo, hi;
};
std::vector<Range> g_ranges;
std::vector<void*> g_guarded_pages;
uintptr_t g_module_base = 0;
uintptr_t g_guest_base = 0;
size_t g_page_size = 4096;

void* g_veh = nullptr;
// Set between a guard fault and the single-step that follows it, so the page
// is re-armed only after the faulting instruction has actually run.
thread_local void* t_rearm_page = nullptr;
// Set alongside t_rearm_page when a read was answered "level": the flags word
// has to go back to the truth the moment that one instruction is done.
thread_local uint8_t* t_restore_at = nullptr;
thread_local uint32_t t_restore_value = 0;
std::unordered_set<uint64_t> g_yes_rvas;

void ArmPage(void* page) {
  DWORD old = 0;
  VirtualProtect(page, 1, PAGE_READWRITE | PAGE_GUARD, &old);
}

// Runs on guest threads, inside an exception. No allocation, no logging - just
// a bounded append behind a lock nothing else takes while the watch is live.
LONG CALLBACK GuardHandler(EXCEPTION_POINTERS* info) {
  const DWORD code = info->ExceptionRecord->ExceptionCode;

  if (code == STATUS_GUARD_PAGE_VIOLATION) {
    if (!g_watch_active.load(std::memory_order_relaxed)) {
      // Raced with the stop: the OS has already cleared the guard for this
      // access, so simply letting the instruction retry is correct. Passing it
      // on instead would leave the thread with nobody to handle it.
      t_rearm_page = nullptr;
      return EXCEPTION_CONTINUE_EXECUTION;
    }
    const uintptr_t accessed =
        static_cast<uintptr_t>(info->ExceptionRecord->ExceptionInformation[1]);
    bool interesting = false;
    for (const Range& r : g_ranges) {
      if (accessed >= r.lo && accessed < r.hi) {
        interesting = true;
        break;
      }
    }
    if (interesting) {
      const uint64_t rva = info->ContextRecord->Rip - g_module_base;
      bool is_new = false;
      {
        std::lock_guard<std::mutex> lock(g_hits_mutex);
        is_new = g_seen_rvas.insert(rva).second;
      }
      if (is_new) {
        const size_t slot = g_hit_count.fetch_add(1);
        if (slot < kMaxHits) {
          g_hits[slot].rva = rva;
          g_hits[slot].accessed = static_cast<uint32_t>(accessed - g_guest_base);
        }
      }
    }
    // Hand this particular reader a level, if it is on the list. The write is
    // undone on the single step below, so no other reader ever sees it.
    if (interesting && !g_yes_rvas.empty()) {
      const uint64_t rva = info->ContextRecord->Rip - g_module_base;
      if (g_yes_rvas.count(rva)) {
        for (const Range& r : g_ranges) {
          if (accessed >= r.lo && accessed < r.hi) {
            auto* low = reinterpret_cast<uint8_t*>(r.lo + 4);
            t_restore_value = LoadBE32(low);
            t_restore_at = low;
            StoreBE32(low, t_restore_value | 0x20000u);
            break;
          }
        }
      }
    }

    // The OS has already cleared PAGE_GUARD for this access. Step one
    // instruction, then put it back, so the next reader faults too.
    t_rearm_page = reinterpret_cast<void*>(accessed & ~(g_page_size - 1));
    info->ContextRecord->EFlags |= 0x100;  // trap flag
    return EXCEPTION_CONTINUE_EXECUTION;
  }

  if (code == STATUS_SINGLE_STEP && t_rearm_page != nullptr) {
    if (t_restore_at != nullptr) {
      StoreBE32(t_restore_at, t_restore_value);
      t_restore_at = nullptr;
    }
    void* page = t_rearm_page;
    t_rearm_page = nullptr;
    if (g_watch_active.load(std::memory_order_relaxed)) {
      ArmPage(page);
    }
    return EXCEPTION_CONTINUE_EXECUTION;
  }

  return EXCEPTION_CONTINUE_SEARCH;
}

void WatchStart() {
  if (g_watch_active.load()) {
    REXLOG_WARN("[icon-probe] watch already running");
    return;
  }
  uint8_t* base = GuestBase();
  if (base == nullptr) {
    return;
  }
  SYSTEM_INFO si{};
  GetSystemInfo(&si);
  g_page_size = si.dwPageSize;
  g_module_base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
  g_guest_base = reinterpret_cast<uintptr_t>(base);

  std::unordered_set<uintptr_t> pages;
  g_ranges.clear();
  g_guarded_pages.clear();
  {
    std::lock_guard<std::mutex> lock(g_areas_mutex);
    if (g_areas.empty()) {
      REXLOG_WARN("[icon-probe] no area captured yet - load a level or a hub first");
      return;
    }
    for (uint32_t area : g_areas) {
      const uintptr_t lo = g_guest_base + area + kAreaFlagsOffset;
      g_ranges.push_back({lo, lo + 8});
      pages.insert(lo & ~(g_page_size - 1));
      pages.insert((lo + 7) & ~(g_page_size - 1));
    }
  }

  g_yes_rvas.clear();
  {
    const std::string list = REXCVAR_GET(icon_probe_yes_rvas);
    size_t at = 0;
    while (at < list.size()) {
      size_t end = list.find(',', at);
      if (end == std::string::npos) {
        end = list.size();
      }
      std::string token = list.substr(at, end - at);
      size_t first = token.find_first_not_of(" 	");
      if (first != std::string::npos) {
        g_yes_rvas.insert(std::strtoull(token.c_str() + first, nullptr, 16));
      }
      at = end + 1;
    }
    if (!g_yes_rvas.empty()) {
      REXLOG_INFO("[icon-probe] {} reader(s) will be told the area is a level",
                  g_yes_rvas.size());
    }
  }

  {
    std::lock_guard<std::mutex> lock(g_hits_mutex);
    g_seen_rvas.clear();
  }
  g_hit_count.store(0);

  if (g_veh == nullptr) {
    g_veh = AddVectoredExceptionHandler(1, GuardHandler);
  }
  g_watch_active.store(true);
  for (uintptr_t p : pages) {
    void* page = reinterpret_cast<void*>(p);
    g_guarded_pages.push_back(page);
    ArmPage(page);
  }
  REXLOG_INFO("[icon-probe] watch armed on {} page(s) covering {} area flag word(s)",
              g_guarded_pages.size(), g_ranges.size());
}

void WatchStop() {
  if (!g_watch_active.exchange(false)) {
    REXLOG_WARN("[icon-probe] watch was not running");
    return;
  }
  // Disarming has to be explicit. Pages sit re-armed between accesses, and with
  // the watch switched off the handler passes their faults on to whoever is
  // next - which is nobody, so the thread wedges. That is what hung the game
  // the first time this ran.
  for (void* page : g_guarded_pages) {
    DWORD old = 0;
    VirtualProtect(page, 1, PAGE_READWRITE, &old);
  }
  g_guarded_pages.clear();

  const size_t n = g_hit_count.load();
  const size_t shown = n > kMaxHits ? kMaxHits : n;
  REXLOG_INFO("[icon-probe] watch stopped, {} distinct reader(s) of the area flags:", shown);
  for (size_t i = 0; i < shown; i++) {
    REXLOG_INFO("[icon-probe]   rva {:#x}  touched guest {:#010x}", g_hits[i].rva,
                g_hits[i].accessed);
  }
  REXLOG_INFO("[icon-probe] module base was {:#x}", g_module_base);
}

// One button for the whole measurement: arm, hold for a few seconds, disarm
// and report. Holding it by hand means the game runs guarded for as long as it
// takes to find the menu entry again, which is both slower and easier to get
// wrong than letting a timer do it.
void WatchBurst() {
  WatchStart();
  if (!g_watch_active.load()) {
    return;
  }
  // Long enough to arm it, close the overlay and trigger an area load: the
  // flags are consumed while an area loads, not while one is being played, so
  // a burst taken standing still catches everything except what matters.
  std::thread([] {
    std::this_thread::sleep_for(std::chrono::seconds(40));
    WatchStop();
  }).detach();
}

#endif  // _WIN32

// Fires once, the moment enough areas are known that the parse is clearly done
// but the session's first area is still loading.
void MaybeAutoWatch(size_t known) {
#ifdef _WIN32
  static std::atomic<bool> done{false};
  if (known < 10 || !REXCVAR_GET(icon_probe_autowatch) || done.exchange(true)) {
    return;
  }
  WatchBurst();
#else
  (void)known;
#endif
}

}  // namespace

// bool AreaHasFlag_0x10(area*) - the area object arrives in r3.
REX_EXTERN(__imp__sub_83AEFCA0);
REX_HOOK_RAW(sub_83AEFCA0) {
  NoteArea(ctx.r3.u32);
  __imp__sub_83AEFCA0(ctx, base);
}

// bool AreaHasFlag_0x40(area*) - same shape, r3 again.
REX_EXTERN(__imp__sub_83AEFCC0);
REX_HOOK_RAW(sub_83AEFCC0) {
  NoteArea(ctx.r3.u32);
  __imp__sub_83AEFCC0(ctx, base);
}

// bool f(?, area*) - this one takes the area in r4.
REX_EXTERN(__imp__sub_83AEF048);
REX_HOOK_RAW(sub_83AEF048) {
  NoteArea(ctx.r4.u32);
  __imp__sub_83AEF048(ctx, base);
}

// The candidate: returns 1 when the area's level bit is set, and the caller's
// own default when it is not. Overriding the answer is the cheapest way to ask
// whether the portrait is one of its callers.
REX_EXTERN(__imp__sub_82E9FB80);
REX_HOOK_RAW(sub_82E9FB80) {
  __imp__sub_82E9FB80(ctx, base);
  if (REXCVAR_GET(hub_3d_portrait)) {
    ctx.r3.u64 = 1;
  }
}

REXCVAR_DEFINE_COMMAND(
    icon_probe_level_bit_on, [] { SetLevelBit(true); }, "Icon probe",
    "Set area flag 0x20000 (treat every area as a level)");

REXCVAR_DEFINE_COMMAND(
    icon_probe_level_bit_off, [] { SetLevelBit(false); }, "Icon probe",
    "Clear area flag 0x20000 (treat every area as a hub)");

REXCVAR_DEFINE_COMMAND(
    icon_probe_dump, [] { DumpAreas(); }, "Icon probe",
    "Report how many areas carry flag 0x20000 right now");

#ifdef _WIN32
REXCVAR_DEFINE_COMMAND(
    icon_probe_watch_5s, [] { WatchBurst(); }, "Icon probe",
    "Catch every read of the area flags for 40 seconds, then report (arm it, then load an area)");

REXCVAR_DEFINE_COMMAND(
    icon_probe_watch_start, [] { WatchStart(); }, "Icon probe",
    "Start catching every read of the area flags (slow - run a few seconds only)");

REXCVAR_DEFINE_COMMAND(
    icon_probe_watch_stop, [] { WatchStop(); }, "Icon probe",
    "Stop the watch and list the readers into the log");
#endif
