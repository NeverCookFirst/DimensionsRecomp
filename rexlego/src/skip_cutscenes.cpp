// legodimensions - ReXGlue Recompiled Project
//
// Skippable cutscenes without typing N0CUT5 every session.
//
// The code-entry handler (sub_83A0AB68) matches "N0CUT5" and then does exactly
// one thing: sub_83835188(obj, 1), which is `stb r4, 344(r3)`, with obj read
// from the global at 0x848BFFAC. So the whole cheat is one byte, +344 in that
// object, and it is not saved - it lasts until the game is closed.
//
// Hooking the getter (sub_83835180, `lbz r3, 344(r3)`) to return 1 was tried on
// 2026-09-03 and did nothing: the engine reads the byte directly as well, so the
// byte itself has to be set. A small thread does that, the same write the code
// makes, and repeats it in case the object is rebuilt between levels.

#include "skip_cutscenes.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

REXCVAR_DEFINE_BOOL(skip_cutscenes, true, "Gameplay",
                    "Cutscenes can be skipped, as if the N0CUT5 code had been typed")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace legodimensions::skip_cutscenes {
namespace {

constexpr uint32_t kCheatObjectGlobal = 0x848BFFAC;
constexpr uint32_t kFlagOffset = 344;

std::atomic<bool> g_running{false};

// A heap lookup only says the address is in some heap's range, not that the
// page behind it exists: reading 0x818200xx that way crashed the first build at
// startup. Ask for the page's protection, as area_watch.cpp does.
template <typename MemoryT>
bool IsCommitted(MemoryT* memory, uint32_t address) {
  auto* heap = memory->LookupHeap(address);
  uint32_t protect = 0;
  return heap && heap->QueryProtect(address, &protect) && protect != 0;
}

uint32_t Load32(const uint8_t* base, uint32_t address) {
  uint32_t v;
  std::memcpy(&v, base + address, sizeof(v));
  return __builtin_bswap32(v);
}

void Loop() {
  bool logged = false;
  bool last_wanted = !REXCVAR_GET(skip_cutscenes);
  while (g_running.load()) {
    auto* ks = rex::system::kernel_state();
    auto* memory = ks ? ks->memory() : nullptr;
    if (memory) {
      uint8_t* base = memory->virtual_membase();
      const bool wanted = REXCVAR_GET(skip_cutscenes);
      // Before the title's image is mapped the global itself is not readable.
      const uint32_t object =
          IsCommitted(memory, kCheatObjectGlobal) ? Load32(base, kCheatObjectGlobal) : 0;
      if (object && IsCommitted(memory, object) && IsCommitted(memory, object + kFlagOffset)) {
        uint8_t& flag = base[object + kFlagOffset];
        if (wanted && flag == 0) {
          flag = 1;
          if (!logged) {
            REXLOG_INFO("Skip cutscenes: on (N0CUT5 flag set at {:08X})", object + kFlagOffset);
            logged = true;
          }
        } else if (!wanted && last_wanted && flag != 0) {
          // Only undo it when the setting was just turned off; a code typed by
          // hand with the setting off is left alone.
          flag = 0;
          REXLOG_INFO("Skip cutscenes: off");
        }
      }
      last_wanted = wanted;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
  }
}

}  // namespace

void Start() {
  if (g_running.exchange(true)) {
    return;
  }
  // Detached: the process hard-exits on close, and this only touches guest
  // memory that outlives it for that whole time.
  std::thread(Loop).detach();
}

}  // namespace legodimensions::skip_cutscenes
