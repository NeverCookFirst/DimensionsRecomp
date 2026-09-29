/**
 * @file    gpu/shaders/dxc_link.h
 * @brief   DXC spec constant library compile and link, shared by the runtime
 *          linker and the build-time reblue_prelink. reblue_prelink compiles
 *          this without the rex SDK on its include path, hence <cstdint> and
 *          no logging: failure comes back as an empty vector.
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace legodimensions::gpu_native {

// lib_6_3 DXIL for 'export uint g_SpecConstants() { return value; }'.
std::vector<uint8_t> CompileSpecConstantLib(uint32_t value);

// Links a spec constant DXIL library against that lib for the given target
// profile ("vs_6_0" / "ps_6_0"), returning the complete shader bytecode.
std::vector<uint8_t> LinkSpecConstantLib(const uint8_t *libraryDxil,
                                         uint32_t libraryDxilSize,
                                         const uint8_t *specLib,
                                         size_t specLibSize,
                                         const wchar_t *profile);

}  // namespace legodimensions::gpu_native
