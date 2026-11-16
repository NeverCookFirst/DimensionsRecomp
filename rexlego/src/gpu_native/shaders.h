// Guest shader lifetime and AOT host-shader resolution.
#pragma once

#include <memory>
#include <array>

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
// Revalidate borrowed program identity and retry failed streaming binds before
// draw filtering, PSO identity and texture-slot masks are read.
void RefreshBoundShaderResources();

struct ShaderBindingsSnapshot {
  std::array<std::shared_ptr<void>, 2> owners;
  std::array<u32, 2> addresses{};
};
ShaderBindingsSnapshot CaptureShaderBindings();
ShaderBindingsSnapshot CaptureShaderBindings(u32 vertex, u32 pixel);
class ShaderBindingsScope {
  const ShaderBindingsSnapshot* previous_;
 public:
  explicit ShaderBindingsScope(const ShaderBindingsSnapshot& bindings);
  ~ShaderBindingsScope();
};

// Materializes the correct prelinked specialization at pipeline creation time.
// A null result marks the containing render pass as unsupported by native GPU.
plume::RenderShader* ResolveBoundShader(ShaderStage stage, u32 spec_constants);
// Returns the latest guest request, including an unavailable nonnull binding.
// Optional failure status is sampled under the same lock as the address.
u32 BoundShaderAddress(ShaderStage stage, bool* binding_failed = nullptr);
u32 BoundShaderTextureMask(ShaderStage stage);
u32 BoundShaderFloatConstantCount(ShaderStage stage);
u64 BoundShaderHash(ShaderStage stage);
// Same microcode skip list used by the existing Graphics/DoF setting.
bool ShouldSkipBoundPixelShader();

bool IsNativeShader(u32 guest_address);
u32 AddRefNativeShader(u32 guest_address);
u32 ReleaseNativeShader(u32 guest_address);
// XDK placement storage can be re-registered at the same guest address.
// The next bind must resolve its new physical code rather than a stale record.
void InvalidatePlacementShader(u32 guest_address);
// Run after the XDK initializer has finished writing the new placement object.
void RefreshPlacementShaderBinding(u32 guest_address);
u32 InvalidatePoolCopyShaders(PhysicalCopyRange destination);
// Capture thread: false only when no registered borrowed shader shares a page with the range.
bool PoolCopyMayTouchShaders(PhysicalCopyRange range);

// Must run before the Plume device is destroyed.
void ResetShaderResources();

// Moves on every shader registry insert.
u64 ShaderRegistryGeneration();

}  // namespace legodimensions::gpu_native
