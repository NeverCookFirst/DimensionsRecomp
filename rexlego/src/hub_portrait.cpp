// legodimensions - ReXGlue Recompiled Project
//
// The HUD character portrait is a live 3D render inside story levels and a flat
// texture in the hub and the open worlds. Nothing in the data files chooses
// between the two: the switch is bit 0x20000 of the area flags, which means
// "this area is a level" to the whole game, so clearing it in the data gives the
// hub its 3D portrait and breaks the world portals and the shared stud pool
// along with it. Only a targeted code patch can separate the two.
//
// Of everything that touches the bit, exactly one function BRANCHES on it rather
// than copying it along: sub_8388D2E8. It fetches an area object through
// sub_82691090, reads the flags at +152, and if either 0x40000 or 0x20000 is set
// it skips the body. So the body is the not-a-level path, and running the game as
// though every area were a level means making that test see the bit set.
//
// Rather than reimplement 214 instructions, this wraps the original: fetch the
// same area object, set the bit, call the untouched original, put the bit back.
// The flags word is restored immediately, so nothing else in the game observes
// the change - which is what makes this different from the data mod that broke
// the portals.
//
// Tried before this and disproven, do not go back to it: sub_82B12F8C, which
// releases the member at +468 only on a level. Releasing it everywhere changed
// nothing on screen, so that member is not the flat portrait.

#include "hub_portrait.h"

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/kernel_state.h>

REXCVAR_DEFINE_BOOL(fix_hub_portrait, false, "Fixes",
                    "Show the animated 3D character portrait in the hub and the open worlds, "
                    "not only inside story levels. Experimental: the game drops one HUD member "
                    "when an area is a level, and this drops it everywhere.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace legodimensions::hub_portrait {
namespace {

// The two guest addresses this patch is built around. Both were read out of the
// recompiled sources; if a future codegen moves them, the warnings below say so.
constexpr uint32_t kPatchedFunction = 0x8388D2E8;
constexpr uint32_t kAreaGetter = 0x82691090;
constexpr uint32_t kLevelBit = 0x20000;

PPCFunc* g_original = nullptr;
PPCFunc* g_area_getter = nullptr;

// sub_8388D2E8, run as though the area were always a level.
void PatchedFunction(PPCContext& __restrict ctx, uint8_t* base) {
  const uint32_t arg0 = ctx.r3.u32;
  const uint32_t arg1 = ctx.r4.u32;

  // The function does not test its own argument: it asks sub_82691090 for the
  // area object first, so the bit has to be set on whatever that returns.
  uint32_t area = 0;
  if (g_area_getter) {
    g_area_getter(ctx, base);
    area = ctx.r3.u32;
  }

  volatile uint32_t* flags_word = nullptr;
  uint32_t saved_flags = 0;
  if (area) {
    flags_word = reinterpret_cast<volatile uint32_t*>(base + area + 152);
    saved_flags = __builtin_bswap32(*flags_word);
    *flags_word = __builtin_bswap32(saved_flags | kLevelBit);
  }

  ctx.r3.u32 = arg0;
  ctx.r4.u32 = arg1;
  g_original(ctx, base);

  // Put it back at once: every other reader must still see the real area.
  if (flags_word) {
    *flags_word = __builtin_bswap32(saved_flags);
  }
}

}  // namespace

void Install() {
  if (!REXCVAR_GET(fix_hub_portrait)) {
    return;
  }

  auto* ks = rex::system::kernel_state();
  auto* dispatcher = ks ? ks->function_dispatcher() : nullptr;
  if (!dispatcher) {
    REXLOG_WARN("Hub portrait: no function dispatcher, patch not installed.");
    return;
  }

  g_original = dispatcher->GetFunction(kPatchedFunction);
  g_area_getter = dispatcher->GetFunction(kAreaGetter);
  if (!g_original || !g_area_getter) {
    REXLOG_WARN("Hub portrait: 0x{:08X} or 0x{:08X} is not registered, patch not installed.",
                kPatchedFunction, kAreaGetter);
    return;
  }
  if (!dispatcher->SetFunction(kPatchedFunction, PatchedFunction)) {
    REXLOG_WARN("Hub portrait: could not replace 0x{:08X}.", kPatchedFunction);
    return;
  }
  REXLOG_INFO("Hub portrait: 0x{:08X} wrapped; it now runs as though every area were a level.",
              kPatchedFunction);
}

}  // namespace legodimensions::hub_portrait
