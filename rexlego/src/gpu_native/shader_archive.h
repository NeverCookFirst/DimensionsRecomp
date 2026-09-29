/**
 * @file        gpu_native/shader_archive.h
 * @brief       LEGO Dimensions AOT DXIL cache schema and lookup helpers
 *
 * @copyright   Copyright (c) 2026 NeverCookFirst
 * @license     BSD 3-Clause License
 */

#pragma once

#include <cstddef>
#include <cstdint>

// XenosRecomp emits these names in the global namespace.
struct ShaderCacheEntry {
  const uint64_t hash;
  const uint32_t dxilOffset;
  const uint32_t dxilSize;
  const uint32_t spirvOffset;
  const uint32_t spirvSize;
  const uint32_t specConstantsMask;
};

extern ShaderCacheEntry g_shaderCacheEntries[];
extern const size_t g_shaderCacheEntryCount;

extern const uint8_t g_compressedDxilCache[];
extern const size_t g_dxilCacheCompressedSize;
extern const size_t g_dxilCacheDecompressedSize;

extern const uint8_t g_compressedSpirvCache[];
extern const size_t g_spirvCacheCompressedSize;
extern const size_t g_spirvCacheDecompressedSize;

namespace legodimensions::gpu_native {

struct ShaderBytecode {
  const uint8_t* data = nullptr;
  size_t size = 0;

  explicit operator bool() const { return data != nullptr && size != 0; }
};

// Xbox shader containers place a four-byte separator between the virtual and
// physical programs. LEGO's cache key covers that separator as well.
size_t ShaderContainerByteLength(const void* shader_container);

const ShaderCacheEntry* FindShader(uint64_t hash);

// Hashes the exact Xbox container span used by the AOT generator.
uint64_t HashShaderContainer(const void* shader_container);

// Returns complete DXIL ready for D3D12. Specialization-library entries are
// resolved exclusively through the build-time linked archive.
ShaderBytecode FindDxil(uint64_t hash, uint32_t spec_constants);

}  // namespace legodimensions::gpu_native
