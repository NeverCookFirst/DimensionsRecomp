#pragma once
#include <cstdint>
#include "gpu_native/temporal_coverage.h"
namespace plume {struct RenderShader;}
namespace legodimensions::gpu_native {
plume::RenderShader* ResolveTemporalShader(uint64_t hash, bool pixel, uint32_t spec);
bool HasTemporalShader(uint64_t hash, bool pixel, uint32_t spec);
bool HasTemporalCameraConstants(uint64_t hash, uint32_t spec);
bool HasTemporalCoverageShader(uint64_t hash, TemporalCoverage mode, uint32_t spec);
plume::RenderShader* ResolveTemporalCoverageShader(uint64_t hash, TemporalCoverage mode, uint32_t spec);
void ResetTemporalShaders();
}
