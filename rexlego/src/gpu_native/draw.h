#pragma once

namespace legodimensions::gpu_native {

// Must run before shader/device teardown because cached PSOs refer to both.
void ResetDrawResources();

}  // namespace legodimensions::gpu_native
