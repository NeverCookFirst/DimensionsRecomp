// legodimensions - ReXGlue Recompiled Project
//
// The animated 3D character portrait, in the hub and the open worlds too.
// See hub_portrait.cpp for what the patch actually does.

#pragma once

namespace legodimensions::hub_portrait {

// Replaces one guest function with our own. Call once the module is registered
// and before the title runs - OnPostSetup is the place. Does nothing unless the
// fix_hub_portrait cvar is on.
void Install();

}  // namespace legodimensions::hub_portrait
