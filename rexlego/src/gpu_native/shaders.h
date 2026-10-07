// Guest shader lifetime and AOT host-shader resolution.
#pragma once

#include <memory>

#include <rex/types.h>
#include "gpu_native/pool_copy.h"

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
// Returns the latest guest request, including an unavailable nonnull binding.
// Optional failure status is sampled under the same lock as the address.
u32 BoundShaderAddress(ShaderStage stage, bool* binding_failed = nullptr);
u32 BoundShaderTextureMask(ShaderStage stage);
u64 BoundShaderHash(ShaderStage stage);
// Same microcode skip list used by the existing Graphics/DoF setting.
bool ShouldSkipBoundPixelShader();

bool IsNativeShader(u32 guest_address);
u32 AddRefNativeShader(u32 guest_address);
u32 ReleaseNativeShader(u32 guest_address);
// XDK placement storage can be re-registered at the same guest address.
// The next bind must resolve its new physical code rather than a stale record.
void InvalidatePlacementShader(u32 guest_address);
u32 InvalidatePoolCopyShaders(PhysicalCopyRange destination);

// Must run before the Plume device is destroyed.
void ResetShaderResources();

}  // namespace legodimensions::gpu_native
