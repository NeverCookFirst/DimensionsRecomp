// xexdump - dumps the decrypted guest image, and finds byte patterns in it.
//
//   xexdump <game_root> <xex_relative> [--out <file>] [--find <ascii>]...
//
// Why this exists: N0CUT5 is a TT Games engine cheat code, so it lives in the
// executable rather than in any data file, and a scan of the shipped XEX finds
// nothing because the image is encrypted on disk. Runtime decrypts it, and the
// codegen path does that with no window and no GPU, so it can run headlessly.
//
// The image is read straight out of guest memory after the load rather than
// through codegen's BinaryView: the SDK install does not export a codegen
// target, and the mapped image is what a memory search would see anyway.

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <rex/kernel/init.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/user_module.h>
#include <rex/system/xex_module.h>
#include <rex/system/xmemory.h>
#include <rex/system/xtypes.h>

// X_STATUS_SUCCESS casts to an unqualified X_STATUS, so the type has to be
// visible at file scope for the macro to expand outside namespace rex.
using rex::X_STATUS;

namespace {

void Usage() {
  std::puts("usage: xexdump <game_root> <xex_relative> [--out <file>] [--find <ascii>]...");
}

// Straight memcmp scan. The image is tens of megabytes, so there is no reason
// for anything cleverer.
void Find(const uint8_t* data, size_t size, uint32_t base, std::string_view needle) {
  if (needle.empty() || size < needle.size()) {
    return;
  }
  size_t hits = 0;
  const size_t last = size - needle.size();
  for (size_t i = 0; i <= last; ++i) {
    if (std::memcmp(data + i, needle.data(), needle.size()) == 0) {
      std::printf("  FOUND \"%.*s\" at guest 0x%08X\n", int(needle.size()), needle.data(),
                  unsigned(base + i));
      ++hits;
    }
  }
  if (hits == 0) {
    std::printf("  \"%.*s\": not present\n", int(needle.size()), needle.data());
  }
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    Usage();
    return 2;
  }

  const std::filesystem::path game_root = argv[1];
  std::string xex_rel = argv[2];
  std::filesystem::path out_file;
  std::vector<std::string> needles;

  for (int i = 3; i < argc; ++i) {
    std::string_view arg = argv[i];
    if (arg == "--out" && i + 1 < argc) {
      out_file = argv[++i];
    } else if (arg == "--find" && i + 1 < argc) {
      needles.emplace_back(argv[++i]);
    } else {
      Usage();
      return 2;
    }
  }

  auto runtime = std::make_unique<rex::Runtime>(game_root.string());
  // tool_mode keeps the runtime headless: no graphics, no window, no guest
  // threads - exactly what the codegen uses.
  auto status = runtime->Setup(rex::RuntimeConfig{
      .kernel_init = rex::kernel::InitializeKernel,
      .tool_mode = true,
  });
  if (status != X_STATUS_SUCCESS) {
    std::printf("Runtime setup failed: 0x%08X\n", unsigned(status));
    return 1;
  }

  std::replace(xex_rel.begin(), xex_rel.end(), '/', '\\');
  status = runtime->LoadXexImage("game:\\" + xex_rel);
  if (status != X_STATUS_SUCCESS) {
    std::printf("LoadXexImage failed: 0x%08X\n", unsigned(status));
    return 1;
  }

  auto module = runtime->kernel_state()->GetExecutableModule();
  if (!module || !module->xex_module()) {
    std::puts("no executable module after load");
    return 1;
  }

  auto* xex = module->xex_module();
  const uint32_t base = xex->base_address();
  const uint32_t size = xex->image_size();
  const uint8_t* host = runtime->memory()->TranslateVirtual(base);
  if (!host || size == 0) {
    std::puts("image is not mapped");
    return 1;
  }
  std::printf("base=0x%08X size=0x%X (%u MB)\n", base, size, unsigned(size / (1024 * 1024)));

  if (!out_file.empty()) {
    if (out_file.has_parent_path()) {
      std::filesystem::create_directories(out_file.parent_path());
    }
    std::ofstream out(out_file, std::ios::binary);
    out.write(reinterpret_cast<const char*>(host), size);
    std::printf("wrote %s\n", out_file.string().c_str());
  }

  for (const auto& needle : needles) {
    Find(host, size, base, needle);
  }

  return 0;
}
