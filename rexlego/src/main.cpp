// legodimensions - ReXGlue Recompiled Project

#include "generated/default/legodimensions_init.h"

#include "legodimensions_app.h"

// Nvidia Optimus / AMD PowerXpress hints.
//
// On a laptop or an APU desktop the driver picks between the integrated and
// the discrete GPU by looking at the export table of the *main executable*.
// The SDK declares the same two symbols in graphics_system.cpp, but that
// translation unit links into rexgpu-xenos.dll, where no driver ever looks -
// so until these lived here the game started on the iGPU and players had to
// add a manual exception in the Nvidia control panel to get a playable frame
// rate. They must stay in the exe.
#if defined(_WIN32)
extern "C" {
__declspec(dllexport) unsigned long NvOptimusEnablement = 0x00000001;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}  // extern "C"
#endif  // _WIN32

REX_DEFINE_APP(legodimensions, LegodimensionsApp::Create)
