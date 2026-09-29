// Xbox 360 D3D/Xenos format conversion for the native renderer.
#pragma once

#include <plume_render_interface.h>
#include <rex/types.h>

namespace legodimensions::gpu_native {

plume::RenderFormat ConvertGuestTextureFormat(u32 guest_format);
plume::RenderFormat ConvertXenosTextureFormat(u32 xenos_format);
bool IsDepthFormat(plume::RenderFormat format);
bool IsRenderTargetFormat(plume::RenderFormat format);
bool IsBlockCompressedFormat(plume::RenderFormat format);
u32 FormatBlockBytes(plume::RenderFormat format);

}  // namespace legodimensions::gpu_native
