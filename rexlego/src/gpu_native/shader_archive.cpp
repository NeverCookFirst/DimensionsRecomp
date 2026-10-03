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
#include <unordered_map>
#include <vector>

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
  constexpr uint32_t kMaximumSectionSize = 4 * 1024 * 1024;
  if (virtual_size > kMaximumSectionSize || physical_size > kMaximumSectionSize) {
    return size_t{kMaximumSectionSize} * 2 + sizeof(uint32_t);
  }
  // Archive containers repeat physical_size at the section boundary, while
  // shaders built by the game's runtime compiler omit that marker.
  const uint32_t boundary = LoadBigEndianU32(bytes + virtual_size);
  const size_t padding = boundary == physical_size ? sizeof(uint32_t) : 0;
  return size_t{virtual_size} + padding + size_t{physical_size};
}

const ShaderCacheEntry* FindShader(uint64_t hash) {
  const auto find_in = [hash](ShaderCacheEntry* begin, size_t count) {
    ShaderCacheEntry* const end = begin + count;
    ShaderCacheEntry* const result = std::lower_bound(
        begin, end, hash, [](const ShaderCacheEntry& entry, uint64_t key) {
          return entry.hash < key;
        });
    return result != end && result->hash == hash ? result : nullptr;
  };
  if (auto* shader = find_in(g_shaderCacheEntries, g_shaderCacheEntryCount)) {
    return shader;
  }
  return find_in(g_runtimeShaderCacheEntries, g_runtimeShaderCacheEntryCount);
}

uint32_t FindShaderTextureMask(uint64_t hash) {
  static const auto masks = [] {
    std::unordered_map<uint64_t, uint32_t> result;
    for (size_t i = 0; i < g_shaderMicrocodeEntryCount; ++i) {
      const auto& entry = g_shaderMicrocodeEntries[i];
      result[entry.containerHash] |= entry.textureMask;
    }
    return result;
  }();
  const auto it = masks.find(hash);
  return it == masks.end() ? ~uint32_t{0} : it->second;
}

const ShaderCacheEntry* FindShaderByMicrocode(const void* microcode,
                                              uint32_t stage) {
  if (!microcode) {
    return nullptr;
  }
  using Index = std::unordered_multimap<uint64_t, const ShaderMicrocodeEntry*>;
  static std::once_flag index_once;
  static Index index;
  std::call_once(index_once, [] {
    index.reserve(g_shaderMicrocodeEntryCount);
    const auto add = [](Index& target, ShaderMicrocodeEntry* entries, size_t count) {
      for (size_t i = 0; i < count; ++i) {
        if (entries[i].microcodeSize != 0) {
          target.emplace(entries[i].prefixHash, &entries[i]);
        }
      }
    };
    add(index, g_shaderMicrocodeEntries, g_shaderMicrocodeEntryCount);
  });

  const uint64_t prefix = XXH3_64bits(microcode, 8);
  const auto [first, last] = index.equal_range(prefix);
  // Thousands of physical shader sections share eight zero prefix bytes.
  // A missing placement shader used to re-hash the same bytes once for each
  // candidate on every bind. Reuse hashes only within THIS lookup, keyed by
  // the exact byte length; never cache across guest writes or bind calls.
  std::unordered_map<uint32_t, uint64_t> hashes_by_size;
  for (auto it = first; it != last; ++it) {
    const ShaderMicrocodeEntry* entry = it->second;
    if (entry->stage != stage) continue;
    auto [hash, inserted] = hashes_by_size.try_emplace(entry->microcodeSize, 0);
    if (inserted) hash->second = XXH3_64bits(microcode, entry->microcodeSize);
    if (hash->second == entry->microcodeHash) {
      if (const auto* shader = FindShader(entry->containerHash)) return shader;
    }
  }
  return nullptr;
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

  const bool runtime = shader >= g_runtimeShaderCacheEntries &&
                       shader < g_runtimeShaderCacheEntries +
                                    g_runtimeShaderCacheEntryCount;
  static std::once_flag main_dxil_once;
  static std::once_flag runtime_dxil_once;
  static std::unique_ptr<uint8_t[]> main_dxil;
  static std::unique_ptr<uint8_t[]> runtime_dxil;
  auto& once = runtime ? runtime_dxil_once : main_dxil_once;
  auto& dxil = runtime ? runtime_dxil : main_dxil;
  const size_t compressed_size = runtime ? g_runtimeDxilCacheCompressedSize
                                         : g_dxilCacheCompressedSize;
  const size_t decompressed_size = runtime ? g_runtimeDxilCacheDecompressedSize
                                           : g_dxilCacheDecompressedSize;
  const uint8_t* compressed = runtime ? g_runtimeCompressedDxilCache
                                      : g_compressedDxilCache;
  std::call_once(once, [&] {
    auto data = std::make_unique<uint8_t[]>(decompressed_size);
    const size_t result =
        ZSTD_decompress(data.get(), decompressed_size, compressed, compressed_size);
    if (ZSTD_isError(result) || result != decompressed_size) {
      REXLOG_ERROR("Native GPU {} DXIL archive decompression failed: {} of {} bytes",
                   runtime ? "runtime" : "main", result, decompressed_size);
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
  const LinkedShaderCacheEntry* const first = runtime
      ? g_runtimeLinkedShaderCacheEntries : g_linkedShaderCacheEntries;
  const size_t linked_count = runtime ? g_runtimeLinkedShaderCacheEntryCount
                                      : g_linkedShaderCacheEntryCount;
  const LinkedShaderCacheEntry* const end = first + linked_count;
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

  static std::once_flag main_linked_once;
  static std::once_flag runtime_linked_once;
  static std::unique_ptr<uint8_t[]> main_linked_dxil;
  static std::unique_ptr<uint8_t[]> runtime_linked_dxil;
  auto& linked_once = runtime ? runtime_linked_once : main_linked_once;
  auto& linked_dxil = runtime ? runtime_linked_dxil : main_linked_dxil;
  const size_t linked_compressed_size = runtime
      ? g_runtimeLinkedDxilCacheCompressedSize : g_linkedDxilCacheCompressedSize;
  const size_t linked_decompressed_size = runtime
      ? g_runtimeLinkedDxilCacheDecompressedSize : g_linkedDxilCacheDecompressedSize;
  const uint8_t* linked_compressed = runtime
      ? g_runtimeCompressedLinkedDxilCache : g_compressedLinkedDxilCache;
  std::call_once(linked_once, [&] {
    auto data = std::make_unique<uint8_t[]>(linked_decompressed_size);
    mz_ulong size = static_cast<mz_ulong>(linked_decompressed_size);
    const int result = mz_uncompress(
        data.get(), &size, linked_compressed,
        static_cast<mz_ulong>(linked_compressed_size));
    if (result != MZ_OK || size != linked_decompressed_size) {
      REXLOG_ERROR("Native GPU {} linked DXIL archive decompression failed",
                   runtime ? "runtime" : "main");
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
