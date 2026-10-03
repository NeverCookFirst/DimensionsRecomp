/**
 * @file    gpu_native/linked_shader_cache.h
 * @brief   Build-time pre-linked spec constant shader variants, emitted by
 *          lego_gpu_prelink into lego-linked-dxil-cache.cpp. Referenced
 *          by unqualified name from the generated cpp, so this must stay in
 *          the global namespace.
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#pragma once

#include <cstddef>
#include <cstdint>

struct LinkedShaderCacheEntry {
  const uint64_t hash;          // == ShaderCacheEntry::hash
  const uint32_t specConstants; // masked g_SpecConstants() value
  const uint32_t dxilOffset;    // into the decompressed linked cache
  const uint32_t dxilSize;
};

// Sorted by (hash, specConstants) for binary search.
extern LinkedShaderCacheEntry g_linkedShaderCacheEntries[];
extern const size_t g_linkedShaderCacheEntryCount;

// miniz (deflate) compressed concatenation of the linked DXIL blobs.
extern const uint8_t g_compressedLinkedDxilCache[];
extern const size_t g_linkedDxilCacheCompressedSize;
extern const size_t g_linkedDxilCacheDecompressedSize;

extern LinkedShaderCacheEntry g_runtimeLinkedShaderCacheEntries[];
extern const size_t g_runtimeLinkedShaderCacheEntryCount;
extern const uint8_t g_runtimeCompressedLinkedDxilCache[];
extern const size_t g_runtimeLinkedDxilCacheCompressedSize;
extern const size_t g_runtimeLinkedDxilCacheDecompressedSize;
