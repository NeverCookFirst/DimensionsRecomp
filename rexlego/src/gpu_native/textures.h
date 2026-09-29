// Guest-visible texture/surface resources and their CPU mirrors.
#pragma once

#include <rex/types.h>

namespace plume {
struct RenderTexture;
struct RenderTextureView;
}

namespace legodimensions::gpu_native {

u32 CreateTextureResource(u32 width, u32 height, u32 depth, u32 levels,
                          u32 usage, u32 guest_format, u32 pool, u32 d3d_type);
u32 CreateSurfaceResource(u32 width, u32 height, u32 guest_format,
                          u32 multi_sample, u32 parameters);

bool LockTextureResource(u32 guest_address, u32 level, u32 locked_rect,
                         u32 rect, u32 flags);
bool DescribeTextureResource(u32 guest_address, u32 level, u32 desc_address);
bool DescribeSurfaceResource(u32 guest_address, u32 desc_address);

bool IsNativeTexture(u32 guest_address);
u32 NativeTextureType(u32 guest_address);
u32 AddRefNativeTexture(u32 guest_address);
u32 ReleaseNativeTexture(u32 guest_address);
void ResetTextureResources();

}  // namespace legodimensions::gpu_native
