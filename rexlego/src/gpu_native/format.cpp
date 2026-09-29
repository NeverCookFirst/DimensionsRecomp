#include "gpu_native/format.h"

#include <atomic>

#include <rex/graphics/xenos.h>
#include <rex/logging.h>

namespace legodimensions::gpu_native {
namespace {

// Full X360 D3DFMT words observed in LEGO/Unleashed-class renderers.
enum class D3DFormat : u32 {
  kA16B16G16R16F = 0x1A22AB60,
  kA16B16G16R16FAlt = 0x1A2201BF,
  kA8B8G8R8 = 0x1A200186,
  kA8R8G8B8 = 0x18280186,
  kX8R8G8B8 = 0x28280086,
  kTT8888 = 0x28280106,
  kTT8888Alt = 0x28280186,
  kTT2101010As16161616 = 0x182801B6,
  kD24FS8 = 0x1A220197,
  kD24S8 = 0x2D200196,
  kR32F = 0x2DA2ABA4,
  kG16R16F = 0x2D22AB9F,
  kG16R16FAlt = 0x2D20AB8D,
  kIndex16 = 1,
  kIndex32 = 6,
  kL8 = 0x28000102,
  kL8Alt = 0x28000002,
  kA8 = 0x04900102,
};

void WarnUnknown(u32 value) {
  static std::atomic<u32> count{0};
  if (count.fetch_add(1, std::memory_order_relaxed) < 16) {
    REXLOG_WARN("Native GPU: unknown guest texture format 0x{:08X}", value);
  }
}

}  // namespace

plume::RenderFormat ConvertGuestTextureFormat(u32 guest_format) {
  using RF = plume::RenderFormat;

  switch (static_cast<D3DFormat>(guest_format)) {
    case D3DFormat::kA16B16G16R16F:
    case D3DFormat::kA16B16G16R16FAlt:
    case D3DFormat::kTT2101010As16161616:
      return RF::R16G16B16A16_FLOAT;
    case D3DFormat::kA8B8G8R8:
    case D3DFormat::kA8R8G8B8:
    case D3DFormat::kX8R8G8B8:
    case D3DFormat::kTT8888:
    case D3DFormat::kTT8888Alt:
      return RF::R8G8B8A8_UNORM;
    case D3DFormat::kD24FS8:
    case D3DFormat::kD24S8:
      // LEGO uses stencil shadows, so retaining the stencil plane is required.
      return RF::D32_FLOAT_S8_UINT;
    case D3DFormat::kR32F:
      return RF::R32_FLOAT;
    case D3DFormat::kG16R16F:
    case D3DFormat::kG16R16FAlt:
      return RF::R16G16_FLOAT;
    case D3DFormat::kIndex16:
      return RF::R16_UINT;
    case D3DFormat::kIndex32:
      return RF::R32_UINT;
    case D3DFormat::kL8:
    case D3DFormat::kL8Alt:
    case D3DFormat::kA8:
      return RF::R8_UNORM;
    default:
      WarnUnknown(guest_format);
      return RF::UNKNOWN;
  }
}

plume::RenderFormat ConvertXenosTextureFormat(u32 xenos_format) {
  using TF = rex::graphics::xenos::TextureFormat;
  using RF = plume::RenderFormat;

  // Some TT asset paths pass the six-bit Xenos texture format rather than a
  // full D3DFMT word. Connor's Diorama parser confirms Dimensions assets use
  // DDS DXT1/DXT3/DXT5; DXN/DXT5A cover its normal/mask variants.
  switch (static_cast<TF>(xenos_format)) {
    case TF::k_8:
    case TF::k_8_A:
    case TF::k_8_B:
      return RF::R8_UNORM;
    case TF::k_8_8:
      return RF::R8G8_UNORM;
    case TF::k_8_8_8_8:
    case TF::k_8_8_8_8_A:
      return RF::R8G8B8A8_UNORM;
    case TF::k_DXT1:
      return RF::BC1_UNORM;
    case TF::k_DXT2_3:
      return RF::BC2_UNORM;
    case TF::k_DXT4_5:
      return RF::BC3_UNORM;
    case TF::k_DXN:
      return RF::BC5_UNORM;
    case TF::k_DXT3A:
    case TF::k_DXT5A:
      return RF::BC4_UNORM;
    case TF::k_16_FLOAT:
      return RF::R16_FLOAT;
    case TF::k_16_16_FLOAT:
      return RF::R16G16_FLOAT;
    case TF::k_16_16_16_16_FLOAT:
      return RF::R16G16B16A16_FLOAT;
    case TF::k_32_FLOAT:
      return RF::R32_FLOAT;
    case TF::k_32_32_FLOAT:
      return RF::R32G32_FLOAT;
    case TF::k_32_32_32_32_FLOAT:
      return RF::R32G32B32A32_FLOAT;
    case TF::k_24_8:
    case TF::k_24_8_FLOAT:
      return RF::D32_FLOAT_S8_UINT;
    case TF::k_2_10_10_10_AS_16_16_16_16:
      return RF::R16G16B16A16_FLOAT;
    default:
      WarnUnknown(xenos_format);
      return RF::UNKNOWN;
  }
}

bool IsDepthFormat(plume::RenderFormat format) {
  return format == plume::RenderFormat::D32_FLOAT ||
         format == plume::RenderFormat::D32_FLOAT_S8_UINT;
}

bool IsRenderTargetFormat(plume::RenderFormat format) {
  using RF = plume::RenderFormat;
  switch (format) {
    case RF::R8_UNORM:
    case RF::R8G8_UNORM:
    case RF::R8G8B8A8_UNORM:
    case RF::B8G8R8A8_UNORM:
    case RF::R16_FLOAT:
    case RF::R16G16_FLOAT:
    case RF::R16G16B16A16_FLOAT:
    case RF::R16G16B16A16_UNORM:
    case RF::R32_FLOAT:
    case RF::R32G32_FLOAT:
    case RF::R32G32B32A32_FLOAT:
      return true;
    default:
      return false;
  }
}

bool IsBlockCompressedFormat(plume::RenderFormat format) {
  using RF = plume::RenderFormat;
  switch (format) {
    case RF::BC1_UNORM:
    case RF::BC2_UNORM:
    case RF::BC3_UNORM:
    case RF::BC4_UNORM:
    case RF::BC5_UNORM:
    case RF::BC7_UNORM:
      return true;
    default:
      return false;
  }
}

u32 FormatBlockBytes(plume::RenderFormat format) {
  using RF = plume::RenderFormat;
  switch (format) {
    case RF::BC1_UNORM:
    case RF::BC4_UNORM:
      return 8;
    case RF::BC2_UNORM:
    case RF::BC3_UNORM:
    case RF::BC5_UNORM:
    case RF::BC7_UNORM:
      return 16;
    default:
      return 0;
  }
}

}  // namespace legodimensions::gpu_native
