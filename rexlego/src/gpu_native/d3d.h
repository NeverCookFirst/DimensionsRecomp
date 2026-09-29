// LEGO Dimensions TU23 guest D3D layouts used by the native renderer.
#pragma once

#include <cstddef>

#include <rex/types.h>

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
  u8 unknown_06F0[0x32E0 - 0x6F0];
  D3DViewport9 viewport;
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
static_assert(offsetof(D3DDevice, scissor) == 0x32FC);
static_assert(offsetof(D3DDevice, pixel_shader) == 0x330C);
static_assert(offsetof(D3DDevice, vertex_shader) == 0x3310);

}  // namespace legodimensions::gpu_native
