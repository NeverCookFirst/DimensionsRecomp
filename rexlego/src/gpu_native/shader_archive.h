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

// Signatures cover both a complete physical shader section and its nested
// instruction stream because both pointer forms are produced by XDK helpers.
struct ShaderMicrocodeEntry {
  const uint64_t prefixHash;
  const uint64_t microcodeHash;
  const uint64_t containerHash;
  const uint32_t microcodeSize;
  // 0 = vertex, 1 = pixel.
  const uint32_t stage;
  const uint32_t textureMask = ~uint32_t{0};
};

extern ShaderCacheEntry g_shaderCacheEntries[];
extern const size_t g_shaderCacheEntryCount;

extern const uint8_t g_compressedDxilCache[];
extern const size_t g_dxilCacheCompressedSize;
extern const size_t g_dxilCacheDecompressedSize;

extern const uint8_t g_compressedSpirvCache[];
extern const size_t g_spirvCacheCompressedSize;
extern const size_t g_spirvCacheDecompressedSize;

extern ShaderCacheEntry g_runtimeShaderCacheEntries[];
extern const size_t g_runtimeShaderCacheEntryCount;
extern const uint8_t g_runtimeCompressedDxilCache[];
extern const size_t g_runtimeDxilCacheCompressedSize;
extern const size_t g_runtimeDxilCacheDecompressedSize;

extern ShaderMicrocodeEntry g_shaderMicrocodeEntries[];
extern const size_t g_shaderMicrocodeEntryCount;

namespace legodimensions::gpu_native {

struct ShaderBytecode {
  const uint8_t* data = nullptr;
  size_t size = 0;

  explicit operator bool() const { return data != nullptr && size != 0; }
};

// Archive containers repeat physical_size between their virtual and physical
// programs. Runtime-created containers omit that marker; both layouts occur in
// LEGO Dimensions and are detected from the container contents.
size_t ShaderContainerByteLength(const void* shader_container);

const ShaderCacheEntry* FindShader(uint64_t hash);
uint32_t FindShaderTextureMask(uint64_t hash);

// Matches an XDK placement shader, which retains only its physical microcode
// pointer, back to the original container compiled into the AOT archive.
const ShaderCacheEntry* FindShaderByMicrocode(const void* microcode,
                                              uint32_t stage);

// Hashes the exact Xbox container span used by the AOT generator.
uint64_t HashShaderContainer(const void* shader_container);

// Returns complete DXIL ready for D3D12. Specialization-library entries are
// resolved exclusively through the build-time linked archive.
ShaderBytecode FindDxil(uint64_t hash, uint32_t spec_constants);

}  // namespace legodimensions::gpu_native
