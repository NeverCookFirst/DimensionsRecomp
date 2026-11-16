#pragma once
namespace legodimensions::gpu_native {
// Native builds never route D3D calls through the packet renderer.
inline constexpr bool UsePm4Reference() { return false; }
}
