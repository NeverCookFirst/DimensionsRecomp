#include <rex/cvar.h>

// The PM4 route prioritizes the SDK's complete Xenos rendering semantics.
// The detached native renderer remains available for development and A/B tests.
REXCVAR_DEFINE_BOOL(gpu_native_pm4, false, "GPU",
    "Use the original XDK command stream and SDK GPU renderer in native builds")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
