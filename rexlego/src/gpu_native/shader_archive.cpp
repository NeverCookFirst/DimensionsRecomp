/**
 * @file        gpu_native/shader_archive.cpp
 * @brief       LEGO Dimensions AOT DXIL cache lookup
 *
 * @copyright   Copyright (c) 2026 NeverCookFirst
 * @license     BSD 3-Clause License
 */

#include "gpu_native/shader_archive.h"

#include <algorithm>
#include <memory>
#include <mutex>
#include <utility>

#define MINIZ_HEADER_FILE_ONLY
#include <miniz.h>
#include <xxhash.h>
#include <zstd.h>

#include <rex/logging.h>

#include "gpu_native/linked_shader_cache.h"

namespace legodimensions::gpu_native {

namespace {

uint32_t LoadBigEndianU32(const uint8_t* bytes) {
  uint32_t value = 0;
  for (size_t i = 0; i != sizeof(value); ++i) {
    value = (value << 8) | bytes[i];
  }
  return value;
}

}  // namespace

size_t ShaderContainerByteLength(const void* shader_container) {
  const auto* bytes = static_cast<const uint8_t*>(shader_container);
  const uint32_t virtual_size = LoadBigEndianU32(bytes + sizeof(uint32_t));
  const uint32_t physical_size = LoadBigEndianU32(bytes + 2 * sizeof(uint32_t));
  return size_t{virtual_size} + sizeof(uint32_t) + size_t{physical_size};
}

const ShaderCacheEntry* FindShader(uint64_t hash) {
  const ShaderCacheEntry* const begin = g_shaderCacheEntries;
  const ShaderCacheEntry* const end = begin + g_shaderCacheEntryCount;
  const ShaderCacheEntry* const result = std::lower_bound(
      begin, end, hash,
      [](const ShaderCacheEntry& entry, uint64_t key) { return entry.hash < key; });
  return result != end && result->hash == hash ? result : nullptr;
}

uint64_t HashShaderContainer(const void* shader_container) {
  if (!shader_container) {
    return 0;
  }
  return XXH3_64bits(shader_container, ShaderContainerByteLength(shader_container));
}

ShaderBytecode FindDxil(uint64_t hash, uint32_t spec_constants) {
  const ShaderCacheEntry* const shader = FindShader(hash);
  if (!shader) {
    REXLOG_WARN("Native GPU shader cache miss: hash=0x{:016X}", hash);
    return {};
  }

  static std::once_flag dxil_once;
  static std::unique_ptr<uint8_t[]> dxil;
  std::call_once(dxil_once, [] {
    auto data = std::make_unique<uint8_t[]>(g_dxilCacheDecompressedSize);
    const size_t result = ZSTD_decompress(data.get(), g_dxilCacheDecompressedSize,
                                          g_compressedDxilCache,
                                          g_dxilCacheCompressedSize);
    if (ZSTD_isError(result) || result != g_dxilCacheDecompressedSize) {
      REXLOG_ERROR("Native GPU DXIL archive decompression failed: {} of {} bytes",
                   result, g_dxilCacheDecompressedSize);
      return;
    }
    dxil = std::move(data);
  });
  if (!dxil) {
    return {};
  }

  if (shader->specConstantsMask == 0) {
    return {dxil.get() + shader->dxilOffset, shader->dxilSize};
  }

  const uint32_t masked = spec_constants & shader->specConstantsMask;
  const LinkedShaderCacheEntry* const first = g_linkedShaderCacheEntries;
  const LinkedShaderCacheEntry* const end = first + g_linkedShaderCacheEntryCount;
  const auto key = std::pair{hash, masked};
  const LinkedShaderCacheEntry* const linked = std::lower_bound(
      first, end, key,
      [](const LinkedShaderCacheEntry& entry, const auto& rhs) {
        return entry.hash < rhs.first ||
               (entry.hash == rhs.first && entry.specConstants < rhs.second);
      });
  if (linked == end || linked->hash != hash || linked->specConstants != masked) {
    REXLOG_ERROR("Native GPU linked shader miss: hash=0x{:016X} spec=0x{:X}",
                 hash, masked);
    return {};
  }

  static std::once_flag linked_once;
  static std::unique_ptr<uint8_t[]> linked_dxil;
  std::call_once(linked_once, [] {
    auto data = std::make_unique<uint8_t[]>(g_linkedDxilCacheDecompressedSize);
    mz_ulong size = static_cast<mz_ulong>(g_linkedDxilCacheDecompressedSize);
    const int result = mz_uncompress(
        data.get(), &size, g_compressedLinkedDxilCache,
        static_cast<mz_ulong>(g_linkedDxilCacheCompressedSize));
    if (result != MZ_OK || size != g_linkedDxilCacheDecompressedSize) {
      REXLOG_ERROR("Native GPU linked DXIL archive decompression failed");
      return;
    }
    linked_dxil = std::move(data);
  });
  if (!linked_dxil) {
    return {};
  }
  return {linked_dxil.get() + linked->dxilOffset, linked->dxilSize};
}

}  // namespace legodimensions::gpu_native
