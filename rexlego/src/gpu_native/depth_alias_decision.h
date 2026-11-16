#pragma once

#include <memory>

namespace legodimensions::gpu_native {

// A depth restore from its EDRAM colour alias: decided in capture order, applied at preparation.
struct DepthAliasDecision {
  std::shared_ptr<void> source, destination;
};

}  // namespace legodimensions::gpu_native
