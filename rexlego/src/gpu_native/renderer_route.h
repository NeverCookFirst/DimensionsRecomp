#pragma once
#include <rex/hook.h>
#include "gpu_native/renderer_mode.h"

// Local to native GPU translation units: every replacement, including resource
// creation, synchronization and raw placement hooks, takes the same route.
#undef REX_HOOK
#define REX_HOOK(name, function) \
  extern "C" REX_FUNC(__imp__##name); \
  extern "C" REX_FUNC(name) { \
    if (legodimensions::gpu_native::UsePm4Reference()) { \
      __imp__##name(ctx, base); \
      return; \
    } \
    rex::ppc::HostToGuestFunction<function>(ctx, base); \
  }

#undef REX_HOOK_RAW
#define REX_HOOK_RAW(name) \
  extern "C" REX_FUNC(__imp__##name); \
  static REX_FUNC(lego_native_##name); \
  extern "C" REX_FUNC(name) { \
    if (legodimensions::gpu_native::UsePm4Reference()) { \
      __imp__##name(ctx, base); \
      return; \
    } \
    lego_native_##name(ctx, base); \
  } \
  static REX_FUNC(lego_native_##name)
