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
  u8 unknown_330C[kGuestDeviceSize - 0x330C];
};

static_assert(sizeof(D3DDevice) == kGuestDeviceSize);
static_assert(offsetof(D3DDevice, set_render_state) == 0x040);
static_assert(offsetof(D3DDevice, set_sampler_state) == 0x1D4);
static_assert(offsetof(D3DDevice, get_render_state) == 0x224);
static_assert(offsetof(D3DDevice, get_sampler_state) == 0x3B8);
static_assert(offsetof(D3DDevice, fetch_constants) == 0x480);
static_assert(offsetof(D3DDevice, viewport) == 0x32E0);
static_assert(offsetof(D3DDevice, scissor) == 0x32FC);

}  // namespace legodimensions::gpu_native
