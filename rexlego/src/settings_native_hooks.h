#pragma once
#include <rex/ppc/func.h>
namespace legodimensions::settings {
// Guest frontend thread. True when native mode owns the action, including
// duplicate presses while a native transition is pending.
bool ShowNativeMenuExtra(PPCContext&, uint8_t*, bool quit);
}
