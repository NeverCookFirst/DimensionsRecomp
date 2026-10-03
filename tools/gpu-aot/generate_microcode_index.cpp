/** Builds the placement-shader lookup without invoking DXC. */
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <vector>
#include <xxhash.h>
#include "../../rexlego/src/gpu_native/shader_texture_usage.h"

namespace {
uint32_t Be32(const uint8_t* p) {
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 |
         uint32_t(p[2]) << 8 | uint32_t(p[3]);
}
struct Entry {
  uint64_t prefix_hash, microcode_hash, container_hash;
  uint32_t microcode_size, stage, texture_mask;
};
void ScanFile(const std::filesystem::path& path,
              std::map<std::pair<uint64_t, uint64_t>, Entry>& entries) {
  std::ifstream input(path, std::ios::binary | std::ios::ate);
  if (!input) return;
  const auto end = input.tellg();
  if (end <= 0) return;
  std::vector<uint8_t> data(static_cast<size_t>(end));
  input.seekg(0);
  input.read(reinterpret_cast<char*>(data.data()), end);
  constexpr size_t kHeaderSize = 0x24;
  for (size_t offset = 0; offset + kHeaderSize <= data.size();) {
    const uint8_t* container = data.data() + offset;
    const uint32_t flags = Be32(container);
    const size_t virtual_size = Be32(container + 4);
    const size_t physical_size = Be32(container + 8);
    const size_t shader_offset = Be32(container + 0x18);
    constexpr size_t kMaximumSectionSize = 4 * 1024 * 1024;
    const bool sizes_ok = virtual_size <= kMaximumSectionSize &&
                          physical_size <= kMaximumSectionSize;
    const bool marker = sizes_ok && virtual_size + 4 <= data.size() - offset &&
                        Be32(container + virtual_size) == physical_size;
    const size_t physical_base = size_t(virtual_size) + (marker ? 4 : 0);
    const size_t total_size = physical_base + physical_size;
    const bool header_ok = sizes_ok &&
                           (flags & 0xFFFFFF00u) == 0x102A1100u &&
                           Be32(container + 0x1C) == 0 &&
                           Be32(container + 0x20) == 0;
    if (!header_ok || total_size > data.size() - offset ||
        shader_offset + 8 > virtual_size) {
      ++offset;
      continue;
    }
    const uint8_t* shader = container + shader_offset;
    const size_t physical_offset = Be32(shader);
    const uint32_t microcode_size = Be32(shader + 4);
    const size_t microcode_offset = physical_base + physical_offset;
    if (microcode_size < 8 || microcode_offset > total_size ||
        microcode_size > total_size - microcode_offset) {
      offset += total_size;
      continue;
    }
    const uint8_t* microcode = container + microcode_offset;
    const uint32_t texture_mask = legodimensions::gpu_native::ShaderTextureUsage(
        {microcode, microcode_size});
    const uint64_t container_hash = XXH3_64bits(container, total_size);
    const uint32_t stage = (flags & 1u) == 0 ? 1u : 0u;
    const auto add_signature = [&](const uint8_t* bytes, uint32_t size) {
      Entry entry{XXH3_64bits(bytes, 8), XXH3_64bits(bytes, size),
                  container_hash, size, stage, texture_mask};
      entries.emplace(std::pair{entry.container_hash, entry.microcode_hash},
                      entry);
    };
    // Depending on the SDK path, placement objects retain either the copied
    // physical section base or the instruction stream inside that section.
    add_signature(container + physical_base,
                  static_cast<uint32_t>(physical_size));
    add_signature(microcode, microcode_size);
    offset += total_size;
  }
}
}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr, "usage: lego_gpu_microcode_index <output.cpp> <input-dir>...\n");
    return 1;
  }
  std::map<std::pair<uint64_t, uint64_t>, Entry> unique;
  for (int i = 2; i < argc; ++i) {
    const std::filesystem::path root(argv[i]);
    if (!std::filesystem::exists(root)) continue;
    for (const auto& item : std::filesystem::recursive_directory_iterator(root))
      if (item.is_regular_file()) ScanFile(item.path(), unique);
  }
  std::vector<Entry> entries;
  for (const auto& [key, entry] : unique) entries.push_back(entry);
  std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
    if (a.prefix_hash != b.prefix_hash) return a.prefix_hash < b.prefix_hash;
    if (a.stage != b.stage) return a.stage < b.stage;
    return a.microcode_hash < b.microcode_hash;
  });
  std::FILE* output = std::fopen(argv[1], "wb");
  if (!output) return 2;
  std::fprintf(output, "#include \"gpu_native/shader_archive.h\"\nShaderMicrocodeEntry g_shaderMicrocodeEntries[] = {\n");
  for (const Entry& e : entries)
    std::fprintf(output, "  { 0x%016llX, 0x%016llX, 0x%016llX, %u, %u, 0x%08X },\n",
                 static_cast<unsigned long long>(e.prefix_hash),
                 static_cast<unsigned long long>(e.microcode_hash),
                 static_cast<unsigned long long>(e.container_hash),
                 e.microcode_size, e.stage, e.texture_mask);
  std::fprintf(output, "};\nconst size_t g_shaderMicrocodeEntryCount = %zu;\n", entries.size());
  std::fclose(output);
  std::printf("Indexed %zu shader microcode blobs\n", entries.size());
  return entries.empty() ? 3 : 0;
}
