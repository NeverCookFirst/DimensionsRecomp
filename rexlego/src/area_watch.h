// legodimensions - ReXGlue Recompiled Project
//
// Knows which area (hub, open world or story level) the player is in, so the
// Discord presence can say where they are.
//
// It does not hunt for a global. sub_82691090 IS the game's own "give me the
// current area" accessor - it is a method, so it cannot be called from outside
// the game thread, but it is called constantly from inside. Wrapping it and
// keeping whatever it hands back turns that into a value any thread can read.

#pragma once

#include <cstdint>
#include <string>

namespace legodimensions::area_watch {

// Wraps the guest accessor. Call once, after guest functions are registered.
void Install();

// Guest address of the area the game last asked about, or 0 before the first
// call. The pointer is only meaningful while the game is running.
uint32_t CurrentArea();

// Internal name of that area ("HUBS", "LEVEL1_WIZARDOFOZ"), or empty when it
// is not known yet.
std::string CurrentAreaName();

// The same area as something worth showing a person ("Wizard of Oz"), or empty
// when the area is unknown or has no entry. Callers fall back to their own
// text rather than ever showing an internal name.
std::string CurrentAreaDisplayName();

}  // namespace legodimensions::area_watch
