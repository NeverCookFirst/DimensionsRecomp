// LEGO Dimensions TU23 guest D3D layouts used by the native renderer.
#pragma once

#include <cstddef>

#include <rex/types.h>

#include "gpu_native/state.h"

namespace legodimensions::gpu_native {

constexpr u32 kGuestDeviceSize = 0x6080;
constexpr u32 kRenderStateCount = 101;
constexpr u32 kSamplerStateCount = 20;
constexpr u32 kFetchConstantCount = 26;

struct D3DViewport9 {
  be_u32 x;
  be_u32 y;
  be_u32 width;
  be_u32 height;
  be_f32 min_z;
  be_f32 max_z;
};
static_assert(sizeof(D3DViewport9) == 0x18);

// TU23 83FBA710 stores numeric floats in the private device shadow. The
// public D3DVIEWPORT9 argument above keeps its unsigned integer ABI.
struct D3DViewportState {
  be_f32 x;
  be_f32 y;
  be_f32 width;
  be_f32 height;
  be_f32 min_z;
  be_f32 max_z;
};
static_assert(sizeof(D3DViewportState) == 0x18);

struct D3DRect {
  be_i32 left;
  be_i32 top;
  be_i32 right;
  be_i32 bottom;
};
static_assert(sizeof(D3DRect) == 0x10);

struct FetchConstant {
  be_u32 dword[6];
};
static_assert(sizeof(FetchConstant) == 0x18);

// Header shared by Xbox 360 D3D resources. The shader create routines in the
// Sep'13 runtime initialize shaders with type 6 (vertex) / 7 (pixel), a
// reference count of one, and 0xFFFF0000 in BaseFlush.
struct D3DResource {
  be_u32 common;
  be_u32 reference_count;
  be_u32 fence;
  be_u32 read_fence;
  be_u32 identifier;
  be_u32 base_flush;
};
static_assert(sizeof(D3DResource) == 0x18);

struct D3DBuffer {
  D3DResource resource;
  be_u32 fetch_lo;
  be_u32 fetch_hi;
};
static_assert(sizeof(D3DBuffer) == 0x20);

enum class D3DResourceType : u32 {
  kSurface = 1,
  kTexture = 3,
  kVolumeTexture = 17,
  kCubeTexture = 18,
};

struct D3DTexture {
  D3DResource resource;
  be_u32 mip_flush;
  FetchConstant format;
};
static_assert(sizeof(D3DTexture) == 0x34);
static_assert(offsetof(D3DTexture, format) == 0x1C);

struct D3DSurface {
  D3DResource resource;
  be_u32 surface_info;
  be_u32 depth_info;
  be_u32 hi_control;
  be_u32 size_bits;
  be_u32 format;
  be_u32 size;
};
static_assert(sizeof(D3DSurface) == 0x30);

struct D3DSurfaceDesc {
  be_u32 format;
  be_u32 type;
  be_u32 usage;
  be_u32 pool;
  be_u32 multi_sample_type;
  be_u32 multi_sample_quality;
  be_u32 width;
  be_u32 height;
};
static_assert(sizeof(D3DSurfaceDesc) == 0x20);

struct D3DLockedRect {
  be_u32 pitch;
  be_u32 bits;
};
static_assert(sizeof(D3DLockedRect) == 8);

struct ShaderContainer {
  be_u32 flags;
  be_u32 virtual_size;
  be_u32 physical_size;
  be_u32 field_0c;
  be_u32 constant_table_offset;
  be_u32 definition_table_offset;
  be_u32 shader_offset;
  be_u32 field_1c;
  be_u32 field_20;
};
static_assert(sizeof(ShaderContainer) == 0x24);

// Only fields proven from TU23 code are named. Keep unknown spans opaque until
// an accessor or state writer establishes their meaning.
struct D3DDevice {
  u8 unknown_0000[0x040];
  be_u32 set_render_state[kRenderStateCount];
  be_u32 set_sampler_state[kSamplerStateCount];
  be_u32 get_render_state[kRenderStateCount];
  be_u32 get_sampler_state[kSamplerStateCount];
  u8 unknown_0408[0x480 - 0x408];
  FetchConstant fetch_constants[kFetchConstantCount];
  u8 unknown_06F0[0x2FD0 - 0x6F0];
  be_u32 vertex_declaration;
  u8 unknown_2FD4[0x3208 - 0x2FD4];
  be_u32 fvf;
  be_u32 index_buffer;
  be_u32 render_targets[kNativeRenderTargets];
  be_u32 depth_stencil;
  be_u32 vertex_stream_buffers[18];
  u8 unknown_326C[0x3278 - 0x326C];
  be_u32 textures[kNativeTextureSlots];
  D3DViewportState viewport;
  be_u32 viewport_reserved;
  D3DRect scissor;
  be_u32 pixel_shader;
  be_u32 vertex_shader;
  u8 unknown_3314[kGuestDeviceSize - 0x3314];
};

static_assert(sizeof(D3DDevice) == kGuestDeviceSize);
static_assert(offsetof(D3DDevice, set_render_state) == 0x040);
static_assert(offsetof(D3DDevice, set_sampler_state) == 0x1D4);
static_assert(offsetof(D3DDevice, get_render_state) == 0x224);
static_assert(offsetof(D3DDevice, get_sampler_state) == 0x3B8);
static_assert(offsetof(D3DDevice, fetch_constants) == 0x480);
static_assert(offsetof(D3DDevice, viewport) == 0x32E0);
static_assert(offsetof(D3DDevice, index_buffer) == 0x320C);
static_assert(offsetof(D3DDevice, vertex_declaration) == 0x2FD0);
static_assert(offsetof(D3DDevice, render_targets) == 0x3210);
static_assert(offsetof(D3DDevice, depth_stencil) == 0x3220);
static_assert(offsetof(D3DDevice, vertex_stream_buffers) == 0x3224);
static_assert(offsetof(D3DDevice, textures) == 0x3278);
static_assert(offsetof(D3DDevice, scissor) == 0x32FC);
static_assert(offsetof(D3DDevice, pixel_shader) == 0x330C);
static_assert(offsetof(D3DDevice, vertex_shader) == 0x3310);

void SetNativeTexture(D3DDevice* device, u32 sampler, u32 texture, u64 dirty);

}  // namespace legodimensions::gpu_native
