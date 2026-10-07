/**
 * @file        gpu_native/shader_archive.cpp
 * @brief       LEGO Dimensions AOT DXIL cache lookup
 *
 * @copyright   Copyright (c) 2026 NeverCookFirst
 * @license     BSD 3-Clause License
 */

#include "gpu_native/shader_archive.h"
#include "gpu_native/shader_container.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <limits>
#include <string_view>
#include <set>
#include <tuple>
#include "native_gpu_build_info.h"
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

size_t ShaderContainerByteLength(const void* shader_container, size_t readable_size) {
  return BoundedShaderContainerByteLength(shader_container, readable_size);
}

static const ShaderCacheEntry* FindBuiltinShader(uint64_t hash) {
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


namespace {
constexpr size_t kPackHeaderBytes = 160;
constexpr size_t kMaximumPackBytes = 64 * 1024 * 1024;
constexpr size_t kMaximumPublishedBytes = 256 * 1024 * 1024;
constexpr std::string_view kPackCommonHeader =
    "11fb0c3e07bd0d8cf4930a73035f98ab623f9877c3d167e82b0c0fccf56f1a9a";
constexpr std::string_view kPackCompilerRevision =
    "339af41df2c23dbe3256c1c377716b81a0e0fe6b";
struct PrecompiledBank {
  std::vector<ShaderCacheEntry> shaders;
  std::vector<uint32_t> stages;
  std::vector<uint32_t> texture_masks;
  std::vector<ShaderMicrocodeEntry> microcodes;
  std::vector<LinkedShaderCacheEntry> variants;
  std::vector<uint8_t> dxil;
};
std::mutex g_pack_mutex;
std::vector<std::unique_ptr<PrecompiledBank>> g_packs;
std::unordered_map<uint64_t, std::pair<PrecompiledBank*, size_t>> g_pack_shaders;
size_t g_published_pack_bytes = 0;
std::atomic<uint64_t> g_pack_generation{0};

uint32_t PackU32(const uint8_t* p) {
  return uint32_t(p[0]) | uint32_t(p[1]) << 8 |
         uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}
uint64_t PackU64(const uint8_t* p) {
  return PackU32(p) | uint64_t(PackU32(p + 4)) << 32;
}
bool ValidPackDxil(std::span<const uint8_t> bytes, uint32_t stage) {
  if (bytes.size() < 36 || std::memcmp(bytes.data(), "DXBC", 4) ||
      PackU32(bytes.data() + 24) != bytes.size()) return false;
  const uint32_t chunks = PackU32(bytes.data() + 28);
  if (!chunks || chunks > 64 || chunks > (bytes.size() - 32) / 4) return false;
  bool dxil = false;
  std::vector<std::pair<size_t, size_t>> ranges;
  for (uint32_t i = 0; i < chunks; ++i) {
    const size_t offset = PackU32(bytes.data() + 32 + i * 4);
    if (offset < 32 + size_t(chunks) * 4 || offset > bytes.size() - 8) return false;
    const size_t size = PackU32(bytes.data() + offset + 4);
    if (size > bytes.size() - offset - 8) return false;
    ranges.emplace_back(offset, offset + 8 + size);
    if (!std::memcmp(bytes.data() + offset, "DXIL", 4)) {
      // DXIL program header: ShaderKind/version, word count, DXIL magic,
      // DXIL version, bitcode offset and bitcode size.
      if (size < 24 || std::memcmp(bytes.data() + offset + 16, "DXIL", 4)) return false;
      const uint32_t program = PackU32(bytes.data() + offset + 8);
      if ((program >> 16) != (stage ? 0u : 1u) ||
          ((program >> 4) & 0xFu) != 6u ||
          uint64_t(PackU32(bytes.data() + offset + 12)) * 4 != size) return false;
      const uint32_t bitcode_offset = PackU32(bytes.data() + offset + 24);
      const uint32_t bitcode_size = PackU32(bytes.data() + offset + 28);
      if (bitcode_offset < 16 || bitcode_offset > size - 8 ||
          bitcode_size < 4 || bitcode_size > size - 8 - bitcode_offset ||
          std::memcmp(bytes.data() + offset + 16 + bitcode_offset, "BC\xC0\xDE", 4)) return false;
      dxil = true;
    }
  }
  std::sort(ranges.begin(), ranges.end());
  for (size_t i = 1; i < ranges.size(); ++i)
    if (ranges[i].first < ranges[i - 1].second) return false;
  return dxil;
}
}  // namespace

const ShaderCacheEntry* FindShader(uint64_t hash) {
  if (const auto* builtin = FindBuiltinShader(hash)) return builtin;
  if (!PrecompiledShaderPackGeneration()) return nullptr;
  std::lock_guard lock(g_pack_mutex);
  const auto it = g_pack_shaders.find(hash);
  return it == g_pack_shaders.end() ? nullptr :
      &it->second.first->shaders[it->second.second];
}

uint64_t PrecompiledShaderPackGeneration() {
  return g_pack_generation.load(std::memory_order_acquire);
}

bool LoadPrecompiledShaderPack(std::span<const uint8_t> bytes, std::string& error) {
  const auto fail = [&](const char* message) { error = message; return false; };
  error.clear();
  if (bytes.size() < kPackHeaderBytes || bytes.size() > kMaximumPackBytes)
    return fail("pack size outside bounds");
  if (std::memcmp(bytes.data(), "LEGODX1\0", 8) || PackU32(bytes.data() + 8) != 1 ||
      PackU32(bytes.data() + 12) != 624)
    return fail("pack format or constants ABI mismatch");
  const uint32_t ns = PackU32(bytes.data() + 16);
  const uint32_t nm = PackU32(bytes.data() + 20);
  const uint32_t nv = PackU32(bytes.data() + 24);
  const uint32_t nb = PackU32(bytes.data() + 28);
  if (!ns || ns > 256 || !nm || nm > 2048 || !nv || nv > 1024)
    return fail("pack table count outside bounds");
  if (std::string_view(reinterpret_cast<const char*>(bytes.data() + 32), 16) !=
          kNativeGpuBuildFingerprint ||
      std::string_view(reinterpret_cast<const char*>(bytes.data() + 48), 64) !=
          kPackCommonHeader ||
      std::string_view(reinterpret_cast<const char*>(bytes.data() + 112), 40) !=
          kPackCompilerRevision)
    return fail("pack build or compiler provenance mismatch");
  const uint64_t tables_end = kPackHeaderBytes + uint64_t(ns) * 24 +
      uint64_t(nm) * 40 + uint64_t(nv) * 24;
  if (tables_end + nb != bytes.size() || !nb)
    return fail("pack table/payload size mismatch");
  if (XXH3_64bits(bytes.data() + kPackHeaderBytes,
                 bytes.size() - kPackHeaderBytes) != PackU64(bytes.data() + 152))
    return fail("pack checksum mismatch");
  auto bank = std::make_unique<PrecompiledBank>();
  size_t at = kPackHeaderBytes;
  std::unordered_map<uint64_t, size_t> shader_indexes;
  for (uint32_t i = 0; i < ns; ++i, at += 24) {
    const auto* p = bytes.data() + at;
    const uint64_t hash = PackU64(p);
    const uint32_t stage = PackU32(p + 8), mask = PackU32(p + 12);
    const uint32_t texture_mask = PackU32(p + 16);
    if (!hash || stage > 1 || (mask & ~(stage ? 2u : 1u)) ||
        PackU32(p + 20) || (i && hash <= bank->shaders.back().hash))
      return fail("invalid or unsorted shader metadata");
    shader_indexes.emplace(hash, i);
    bank->shaders.push_back({hash, 0, 0, 0, 0, mask});
    bank->stages.push_back(stage);
    bank->texture_masks.push_back(texture_mask);
  }
  std::vector<uint32_t> masks(ns, 0), micro_counts(ns, 0);
  using MicroKey = std::tuple<uint32_t, uint32_t, uint64_t>;
  std::set<MicroKey> micro_keys;
  for (uint32_t i = 0; i < nm; ++i, at += 40) {
    const auto* p = bytes.data() + at;
    const uint64_t prefix = PackU64(p), physical = PackU64(p + 8), hash = PackU64(p + 16);
    const uint32_t size = PackU32(p + 24), stage = PackU32(p + 28), texture = PackU32(p + 32);
    const auto it = shader_indexes.find(hash);
    if (it == shader_indexes.end() || stage != bank->stages[it->second] ||
        size < 8 || size > 4 * 1024 * 1024 || PackU32(p + 36) ||
        !micro_keys.emplace(stage, size, physical).second)
      return fail("invalid or ambiguous microcode metadata");
    masks[it->second] |= texture;
    ++micro_counts[it->second];
    bank->microcodes.push_back({prefix, physical, hash, size, stage, texture});
  }
  for (size_t i = 0; i < ns; ++i)
    if (!micro_counts[i] || masks[i] != bank->texture_masks[i])
      return fail("missing microcode or texture metadata mismatch");
  std::vector<std::vector<uint32_t>> specs(ns);
  size_t next_payload_offset = 0;
  for (uint32_t i = 0; i < nv; ++i, at += 24) {
    const auto* p = bytes.data() + at;
    const uint64_t hash = PackU64(p);
    const uint32_t spec = PackU32(p + 8), offset = PackU32(p + 12), size = PackU32(p + 16);
    const auto it = shader_indexes.find(hash);
    if (it == shader_indexes.end() || (spec & ~bank->shaders[it->second].specConstantsMask) ||
        PackU32(p + 20) || offset != next_payload_offset || !size ||
        uint64_t(offset) + size > nb ||
        (i && std::pair{hash, spec} <= std::pair{bank->variants.back().hash,
                                               bank->variants.back().specConstants}) ||
        !ValidPackDxil(bytes.subspan(size_t(tables_end) + offset, size),
                       bank->stages[it->second]))
      return fail("invalid or unsorted DXIL variant");
    specs[it->second].push_back(spec);
    bank->variants.push_back({hash, spec, offset, size});
    next_payload_offset += size;
  }
  if (next_payload_offset != nb) return fail("unreferenced DXIL payload bytes");
  for (size_t i = 0; i < ns; ++i) {
    const uint32_t mask = bank->shaders[i].specConstantsMask;
    if (specs[i] != (mask ? std::vector<uint32_t>{0, mask} : std::vector<uint32_t>{0}))
      return fail("incomplete specialization variants");
  }
  // Publish only after complete validation. Existing pointers never move and
  // conflicting built-in or previously published identities are rejected.
  std::lock_guard lock(g_pack_mutex);
  if (g_packs.size() >= 64 || bytes.size() > kMaximumPublishedBytes - g_published_pack_bytes)
    return fail("process pack lifetime budget exceeded");
  for (const auto& shader : bank->shaders)
    if (FindBuiltinShader(shader.hash) || g_pack_shaders.contains(shader.hash))
      return fail("pack attempts to replace an existing shader");
  const auto conflicts = [&](const ShaderMicrocodeEntry& m) {
    return micro_keys.contains(MicroKey{m.stage, m.microcodeSize, m.microcodeHash});
  };
  for (size_t i = 0; i < g_shaderMicrocodeEntryCount; ++i)
    if (conflicts(g_shaderMicrocodeEntries[i])) return fail("built-in microcode identity conflict");
  for (const auto& existing : g_packs)
    for (const auto& m : existing->microcodes)
      if (conflicts(m)) return fail("published microcode identity conflict");
  bank->dxil.assign(bytes.begin() + size_t(tables_end), bytes.end());
  auto* published = bank.get();
  // Build the replacement index before changing visibility: unordered_map
  // insertion can allocate even after reserve. Any failure leaves publication
  // and its generation unchanged.
  auto next_index = g_pack_shaders;
  next_index.reserve(next_index.size() + ns);
  for (size_t i = 0; i < published->shaders.size(); ++i)
    next_index.emplace(published->shaders[i].hash, std::pair{published, i});
  g_packs.reserve(g_packs.size() + 1);
  g_packs.push_back(std::move(bank));
  g_pack_shaders.swap(next_index);
  g_published_pack_bytes += bytes.size();
  g_pack_generation.fetch_add(1, std::memory_order_release);
  return true;
}

uint32_t FindShaderPhysicalSize(uint64_t hash, uint32_t stage) {
  uint32_t size = 0;
  const auto visit = [&](const ShaderMicrocodeEntry& e) {
    if (e.containerHash == hash && e.stage == stage) size = std::max(size, e.microcodeSize);
  };
  for (size_t i = 0; i < g_shaderMicrocodeEntryCount; ++i) visit(g_shaderMicrocodeEntries[i]);
  std::lock_guard lock(g_pack_mutex);
  for (const auto& bank : g_packs) for (const auto& e : bank->microcodes) visit(e);
  return size;
}

std::unordered_set<uint64_t> FindPixelShaderContainers(
    const std::unordered_set<uint64_t>& physical_hashes) {
  std::unordered_set<uint64_t> result;
  const auto visit = [&](const ShaderMicrocodeEntry& e) {
    if (e.stage == 1 && physical_hashes.contains(e.microcodeHash)) result.insert(e.containerHash);
  };
  for (size_t i = 0; i < g_shaderMicrocodeEntryCount; ++i) visit(g_shaderMicrocodeEntries[i]);
  std::lock_guard lock(g_pack_mutex);
  for (const auto& bank : g_packs) for (const auto& e : bank->microcodes) visit(e);
  return result;
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
  if (it != masks.end()) return it->second;
  if (FindBuiltinShader(hash)) return ~uint32_t{0};
  std::lock_guard lock(g_pack_mutex);
  const auto overlay = g_pack_shaders.find(hash);
  return overlay == g_pack_shaders.end() ? ~uint32_t{0} :
      overlay->second.first->texture_masks[overlay->second.second];
}

static const ShaderCacheEntry* FindPrecompiledByMicrocode(
    const void* microcode, uint32_t stage, size_t readable_size, uint64_t prefix,
    std::unordered_map<uint32_t, uint64_t>& hashes_by_size) {
  if (!PrecompiledShaderPackGeneration()) return nullptr;
  {
    std::lock_guard lock(g_pack_mutex);
    for (const auto& bank : g_packs) for (const auto& entry : bank->microcodes) {
      if (entry.stage != stage || entry.prefixHash != prefix || entry.microcodeSize > readable_size) continue;
      auto [hash, inserted] = hashes_by_size.try_emplace(entry.microcodeSize, 0);
      if (inserted) hash->second = XXH3_64bits(microcode, entry.microcodeSize);
      if (hash->second == entry.microcodeHash) {
        const auto shader = g_pack_shaders.find(entry.containerHash);
        return &shader->second.first->shaders[shader->second.second];
      }
    }
  }
  return nullptr;
}

const ShaderCacheEntry* FindShaderByMicrocode(const void* microcode,
                                              uint32_t stage, size_t readable_size) {
  if (!microcode || readable_size < 8) {
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
  if (const auto* overlay = FindPrecompiledByMicrocode(
          microcode, stage, readable_size, prefix, hashes_by_size)) return overlay;
  for (auto it = first; it != last; ++it) {
    const ShaderMicrocodeEntry* entry = it->second;
    if (entry->stage != stage || entry->microcodeSize > readable_size) continue;
    auto [hash, inserted] = hashes_by_size.try_emplace(entry->microcodeSize, 0);
    if (inserted) hash->second = XXH3_64bits(microcode, entry->microcodeSize);
    if (hash->second == entry->microcodeHash) {
      if (const auto* shader = FindShader(entry->containerHash)) return shader;
    }
  }
  return nullptr;
}

uint64_t HashShaderContainer(const void* shader_container, size_t readable_size) {
  const size_t byte_length = ShaderContainerByteLength(shader_container, readable_size);
  return byte_length ? XXH3_64bits(shader_container, byte_length) : 0;
}

ShaderBytecode FindDxil(uint64_t hash, uint32_t spec_constants) {
  const ShaderCacheEntry* const shader = FindBuiltinShader(hash);
  if (!shader) {
    std::lock_guard lock(g_pack_mutex);
    const auto it = g_pack_shaders.find(hash);
    if (it != g_pack_shaders.end()) {
      const auto& bank = *it->second.first;
      const uint32_t masked = spec_constants & bank.shaders[it->second.second].specConstantsMask;
      const auto key = std::pair{hash, masked};
      const auto variant = std::lower_bound(bank.variants.begin(), bank.variants.end(), key,
          [](const LinkedShaderCacheEntry& e, const auto& k) { return std::pair{e.hash, e.specConstants} < k; });
      if (variant == bank.variants.end() || std::pair{variant->hash, variant->specConstants} != key) return {};
      return {bank.dxil.data() + variant->dxilOffset, variant->dxilSize};
    }
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
