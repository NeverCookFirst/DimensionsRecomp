/**
 * @file    tools/prelink_shader_cache.cpp
 * @brief   Build-time DXC pre-linker: emits linked_shader_cache.cpp with a
 *          DXIL blob for every (shader, specConstantsMask subset) pair.
 *
 *          Standalone console tool: fprintf/stderr diagnostics by design
 *          (the engine's BD_* logging is not linked here).
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fcntl.h>
#include <map>
#include <string>
#include <thread>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <io.h>
#include <share.h>
#include <sys/stat.h>
#else
#include <unistd.h>
#endif
#define MINIZ_HEADER_FILE_ONLY
#include <miniz.h>
#include <zstd.h>

#include "gpu_native/dxc_link.h"
#include "gpu/shaders/shader_cache.h"
#include "prelink_jobs.h"

namespace {

// Both prelink targets publish through this transaction. A failed write,
// flush, close or replacement must leave the previously generated bank intact.
class PrelinkOutput {
 public:
  explicit PrelinkOutput(const char* destination) : destination_(destination) {
    static std::atomic<uint64_t> serial{0};
#ifdef _WIN32
    const auto process = GetCurrentProcessId();
#else
    const auto process = getpid();
#endif
    for (unsigned attempt = 0; attempt < 64; ++attempt) {
      temporary_ = destination_;
      temporary_ += ".tmp." + std::to_string(process) + "." +
                    std::to_string(serial.fetch_add(1, std::memory_order_relaxed));
#ifdef _WIN32
      int descriptor = -1;
      const auto error = _wsopen_s(&descriptor, temporary_.c_str(),
          _O_CREAT | _O_EXCL | _O_WRONLY | _O_BINARY, _SH_DENYRW,
          _S_IREAD | _S_IWRITE);
      if (error) {
        errno = error;
        if (error == EEXIST) continue;
        temporary_.clear();
        return;
      }
      file_ = _wfdopen(descriptor, L"wb");
      if (!file_) _close(descriptor);
#else
      const int descriptor = open(temporary_.c_str(), O_CREAT | O_EXCL | O_WRONLY, 0666);
      if (descriptor < 0) {
        if (errno == EEXIST) continue;
        temporary_.clear();
        return;
      }
      file_ = fdopen(descriptor, "wb");
      if (!file_) close(descriptor);
#endif
      return;
    }
    // Every attempted name belonged to another transaction; do not remove it.
    temporary_.clear();
  }

  PrelinkOutput(const PrelinkOutput&) = delete;
  PrelinkOutput& operator=(const PrelinkOutput&) = delete;
  ~PrelinkOutput() {
    if (file_) std::fclose(file_);
    if (!temporary_.empty()) {
      std::error_code ignored;
      std::filesystem::remove(temporary_, ignored);
    }
  }

  FILE* file() const { return file_; }

  bool commit() {
    if (!file_) return false;
    bool success = std::ferror(file_) == 0;
    if (std::fflush(file_) != 0) success = false;
#ifdef _WIN32
    if (success && _commit(_fileno(file_)) != 0) success = false;
#else
    if (success && fsync(fileno(file_)) != 0) success = false;
#endif
    if (std::fclose(file_) != 0) success = false;
    file_ = nullptr;
    if (!success) return false;
#ifdef _WIN32
    if (!MoveFileExW(temporary_.c_str(), destination_.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return false;
#else
    if (std::rename(temporary_.c_str(), destination_.c_str()) != 0) return false;
#endif
    temporary_.clear();
    return true;
  }

 private:
  std::filesystem::path destination_;
  std::filesystem::path temporary_;
  FILE* file_ = nullptr;
};

void EmitBytes(FILE* f, const uint8_t* data, size_t size) {
  // Integer initializers create one AST node per byte. The linked bank can
  // contain tens of millions of bytes, so emit a single string initializer
  // instead. MSVC limits concatenated string length, so retain its original
  // initializer form. Large banks still require the COFF embedding path to
  // avoid Clang's per-byte constant-evaluation memory cost. The
  // recorded compressed size excludes the string form's trailing C++ NUL.
  std::fputs("#if defined(_MSC_VER) && !defined(__clang__)\n{\n", f);
  if (!size) std::fputs("0,", f);
  for (size_t i = 0; i < size; ++i) {
    std::fprintf(f, "%u,", data[i]);
    if ((i & 31) == 31) std::fputc('\n', f);
  }
  std::fputs("\n}\n#else\n"
             "#if defined(__clang__)\n"
             "#pragma clang diagnostic push\n"
             "#pragma clang diagnostic ignored \"-Woverlength-strings\"\n"
             "#endif\n", f);
  constexpr size_t kLineBytes = 256;
  constexpr char kHex[] = "0123456789ABCDEF";
  if (!size) std::fputs("\"\"\n", f);
  for (size_t begin = 0; begin < size; begin += kLineBytes) {
    const size_t count = std::min(kLineBytes, size - begin);
    char line[kLineBytes * 4 + 3];
    line[0] = '"';
    for (size_t i = 0; i < count; ++i) {
      const uint8_t byte = data[begin + i];
      line[1 + i * 4] = '\\';
      line[2 + i * 4] = 'x';
      line[3 + i * 4] = kHex[byte >> 4];
      line[4 + i * 4] = kHex[byte & 15];
    }
    line[1 + count * 4] = '"';
    line[2 + count * 4] = '\n';
    std::fwrite(line, 1, count * 4 + 3, f);
  }
  std::fputs("#if defined(__clang__)\n"
             "#pragma clang diagnostic pop\n"
             "#endif\n#endif\n", f);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: lego_gpu_prelink <output.cpp>\n");
    return 1;
  }

  std::vector<uint8_t> dxil(g_dxilCacheDecompressedSize);
  const size_t n =
      ZSTD_decompress(dxil.data(), dxil.size(), g_compressedDxilCache,
                      g_dxilCacheCompressedSize);
  if (ZSTD_isError(n) || n != dxil.size()) {
    std::fprintf(stderr, "prelink: DXIL cache decompression failed\n");
    return 1;
  }

  struct Job {
    const ShaderCacheEntry* entry;
    uint32_t masked;
  };
  std::vector<Job> jobs;
  size_t spec_shader_count = 0;
  for (size_t i = 0; i < g_shaderCacheEntryCount; ++i) {
    const auto& e = g_shaderCacheEntries[i];
    if (!e.specConstantsMask) continue;
    ++spec_shader_count;
    // Every subset of the mask, including 0 (a spec-constant library still
    // needs g_SpecConstants() resolved for the value-0 variant).
    uint32_t s = 0;
    do {
      jobs.push_back({&e, s});
      s = (s - e.specConstantsMask) & e.specConstantsMask;
    } while (s);
  }

  const auto worker_count = lego_gpu_aot::PrelinkWorkerCount(
      jobs.size(), std::thread::hardware_concurrency(),
      std::getenv("LEGO_GPU_PRELINK_JOBS"));
  if (!worker_count) {
    std::fprintf(stderr, "prelink: LEGO_GPU_PRELINK_JOBS must be a positive unsigned decimal integer\n");
    return 1;
  }

  std::map<uint32_t, std::vector<uint8_t>> specLibs;
  for (const auto& j : jobs) specLibs.try_emplace(j.masked);
  for (auto& [value, blob] : specLibs) {
    blob = legodimensions::gpu_native::CompileSpecConstantLib(value);
    if (blob.empty()) {
      std::fprintf(stderr, "prelink: spec lib compile failed (value=%u)\n",
                   value);
      return 1;
    }
  }

  struct Result {
    uint64_t hash;
    uint32_t masked;
    std::vector<uint8_t> dxil;
  };
  std::vector<Result> results(jobs.size());
  std::atomic<size_t> next{0};
  std::atomic<bool> failed{false};

  auto worker = [&] {
    for (;;) {
      const size_t i = next.fetch_add(1);
      if (i >= jobs.size() || failed.load()) return;
      const Job& j = jobs[i];
      const uint8_t* lib = dxil.data() + j.entry->dxilOffset;
      const auto& spec = specLibs.find(j.masked)->second;
      // The cache stores no shader type, and a library's main only validates
      // against its own stage, so the failing profile identifies it.
      auto vs = legodimensions::gpu_native::LinkSpecConstantLib(
          lib, j.entry->dxilSize, spec.data(), spec.size(), L"vs_6_0");
      auto ps = legodimensions::gpu_native::LinkSpecConstantLib(
          lib, j.entry->dxilSize, spec.data(), spec.size(), L"ps_6_0");
      if (vs.empty() == ps.empty()) {
        std::fprintf(stderr,
                     "prelink: %s link hash=%016llX mask=0x%X (vs=%zu ps=%zu)\n",
                     vs.empty() ? "failed" : "ambiguous",
                     static_cast<unsigned long long>(j.entry->hash), j.masked,
                     vs.size(), ps.size());
        failed.store(true);
        return;
      }
      results[i] = {j.entry->hash, j.masked,
                    vs.empty() ? std::move(ps) : std::move(vs)};
    }
  };
  {
    std::vector<std::thread> pool;
    for (unsigned t = 0; t < *worker_count; ++t) pool.emplace_back(worker);
    for (auto& t : pool) t.join();
  }
  if (failed.load()) return 1;

  std::sort(results.begin(), results.end(), [](const Result& a,
                                               const Result& b) {
    if (a.hash != b.hash) return a.hash < b.hash;
    return a.masked < b.masked;
  });

  struct OutEntry {
    uint64_t hash;
    uint32_t masked;
    uint32_t offset;
    uint32_t size;
  };
  std::vector<OutEntry> entries;
  std::vector<uint8_t> blob;
  for (const auto& r : results) {
    entries.push_back({r.hash, r.masked, static_cast<uint32_t>(blob.size()),
                       static_cast<uint32_t>(r.dxil.size())});
    blob.insert(blob.end(), r.dxil.begin(), r.dxil.end());
  }
  if (blob.empty()) blob.push_back(0);  // keep the C arrays non-empty

  mz_ulong comp_len = mz_compressBound(static_cast<mz_ulong>(blob.size()));
  std::vector<uint8_t> comp(comp_len);
  if (mz_compress2(comp.data(), &comp_len, blob.data(),
                   static_cast<mz_ulong>(blob.size()),
                   MZ_UBER_COMPRESSION) != MZ_OK) {
    std::fprintf(stderr, "prelink: deflate failed\n");
    return 1;
  }
  // Exercise the exact runtime decompressor before emitting the translation
  // unit. A corrupt linked archive must fail the build, not the first draw.
  std::vector<uint8_t> round_trip(blob.size());
  mz_ulong round_trip_len = static_cast<mz_ulong>(round_trip.size());
  if (mz_uncompress(round_trip.data(), &round_trip_len, comp.data(), comp_len) != MZ_OK ||
      round_trip_len != blob.size() || round_trip != blob) {
    std::fprintf(stderr, "prelink: deflate round-trip validation failed\n");
    return 1;
  }

  PrelinkOutput output(argv[1]);
  FILE* f = output.file();
  if (!f) {
    std::fprintf(stderr, "prelink: cannot create temporary output for %s\n", argv[1]);
    return 1;
  }
  std::fprintf(f, "#include \"gpu/shaders/linked_shader_cache.h\"\n");
  std::fprintf(f, "LinkedShaderCacheEntry g_linkedShaderCacheEntries[] = {\n");
  for (const auto& e : entries) {
    std::fprintf(f, "\t{ 0x%llX, 0x%X, %u, %u },\n",
                 static_cast<unsigned long long>(e.hash), e.masked, e.offset,
                 e.size);
  }
  if (entries.empty()) std::fprintf(f, "\t{ 0, 0, 0, 0 },\n");
  std::fprintf(f, "};\n");
  std::fprintf(f, "const size_t g_linkedShaderCacheEntryCount = %zu;\n",
               entries.size());
  std::fprintf(f, "const uint8_t g_compressedLinkedDxilCache[] =\n");
  EmitBytes(f, comp.data(), comp_len);
  std::fprintf(f, ";\n");
  std::fprintf(f, "const size_t g_linkedDxilCacheCompressedSize = %zu;\n",
               static_cast<size_t>(comp_len));
  std::fprintf(f, "const size_t g_linkedDxilCacheDecompressedSize = %zu;\n",
               blob.size());
  if (!output.commit()) {
    std::fprintf(stderr, "prelink: cannot publish %s; previous output preserved\n", argv[1]);
    return 1;
  }

  std::printf("prelink: %zu variant(s) from %zu spec-constant shader(s), "
              "%zu -> %zu bytes\n",
              entries.size(), spec_shader_count, blob.size(),
              static_cast<size_t>(comp_len));
  return 0;
}
