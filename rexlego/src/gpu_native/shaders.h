// Guest shader lifetime and AOT host-shader resolution.
#pragma once

#include <memory>

#include <rex/types.h>

namespace plume {
struct RenderShader;
}

namespace legodimensions::gpu_native {

enum class ShaderStage : u32 {
  kVertex = 6,
  kPixel = 7,
};

// Creates the guest-visible resource and records the corresponding AOT hash.
// The returned value is a guest virtual address, as expected by D3D9.
u32 CreateShaderResource(mapped_u32 function, ShaderStage stage);

// Binding retains host metadata independently of the guest resource lifetime.
bool BindShader(ShaderStage stage, u32 guest_address);

// Materializes the correct prelinked specialization at pipeline creation time.
// A null result marks the containing render pass as unsupported by native GPU.
plume::RenderShader* ResolveBoundShader(ShaderStage stage, u32 spec_constants);

bool IsNativeShader(u32 guest_address);
u32 AddRefNativeShader(u32 guest_address);
u32 ReleaseNativeShader(u32 guest_address);

// Must run before the Plume device is destroyed.
void ResetShaderResources();

}  // namespace legodimensions::gpu_native
