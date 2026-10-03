#pragma once
#include <cstdlib>
#include <cstring>
#include <rex/logging.h>
#include <rex/cvar.h>

REXCVAR_DECLARE(bool, gpu_native_pm4);

namespace legodimensions::gpu_native {
// Select once, before runtime setup and the first XDK hook. Mixing native
// objects and original XDK objects in one session is invalid.
inline bool UsePm4Reference() {
  static const bool enabled = [] {
    const char* value = std::getenv("LEGO_NATIVE_PM4_REFERENCE");
    // An explicit 0/1 override supports controlled A/B probes. Normal launches
    // use the persisted setting, so the tested renderer is reproducible.
    const bool selected = value && std::strcmp(value, "1") == 0 ? true :
        value && std::strcmp(value, "0") == 0 ? false : REXCVAR_GET(gpu_native_pm4);
    REXLOG_INFO("GPU route: {}", selected ? "original XDK PM4 / SDK renderer" :
                "detached native D3D12 renderer");
    return selected;
  }();
  return enabled;
}
}
