#include <rex/cvar.h>

// Retain config compatibility without enabling packet rendering.
REXCVAR_DEFINE_BOOL(gpu_native_pm4, false, "GPU",
    "Legacy setting ignored by the direct native renderer")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
