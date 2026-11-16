// CPU scheduling reproduction: real SDK pages/watch faults and extracted native
// capture/cache/queue/read functions. Storage/publication/worker scheduling are
// explicit seams; no device, GPU, window, game entry point or launcher is used.
#define NOMINMAX
#include "gpu_native/textures.h"
#include "gpu_native/d3d.h"
#include "gpu_native/guest_metadata.h"
#include "gpu_native/memory_watch.h"
#include "gpu_native/texture_upload_source.h"
#include <rex/graphics/xenos.h>
#include <rex/system/xmemory.h>
#include <windows.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <mutex>
#include <thread>

namespace fixture { rex::memory::Memory* memory = nullptr; }
#define REX_KERNEL_MEMORY() (fixture::memory)
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr,"line %d: %s\n",__LINE__,#c); std::exit(23); } } while (0)

namespace legodimensions::gpu_native {
constexpr u32 kHeader = 0xA2001000, kPayload = 0xA2008000, kBytes = 256;
std::vector<u8> submitted_payload;
std::vector<std::shared_ptr<void>> retired;
u32 cache_touches = 0;
struct TextureUploadPublication {
  std::vector<u8> payload;
  explicit TextureUploadPublication(std::vector<u8> value) : payload(std::move(value)) {}
  void Enqueue(plume::RenderCommandList*) { submitted_payload = payload; }
};
struct PublicationCache {
  void Touch(const std::shared_ptr<TextureUploadPublication>&) { ++cache_touches; }
} g_texture_upload_publications;
struct HostDevice { static void RetireResource(std::shared_ptr<void> owner) { retired.push_back(std::move(owner)); } };
struct TextureResource {
  std::mutex mutex;
  std::atomic<bool> replaced{false};
  u32 guest_address = kHeader, guest_format = 6, adopted_header_type = 3;
  bool owns_guest_memory = false, surface = false, resolved_on_host = false, guest_uploaded = true;
  u64 depth_alias_generation = 0;
  plume::RenderFormat format = plume::RenderFormat::R8G8B8A8_UNORM;
  plume::RenderTexture* texture = reinterpret_cast<plume::RenderTexture*>(0x1000);
  rex::graphics::xenos::xe_gpu_texture_fetch_t guest_fetch{};
  CpuMemoryStamp cpu_stamp;
  std::weak_ptr<TextureUploadPublication> prepared_upload;
  std::shared_ptr<TextureUploadPublication> fixture_cache_owner;
};
std::shared_ptr<TextureResource> current;
std::shared_ptr<TextureResource> FindTexture(u32 address) { return address == kHeader ? current : nullptr; }
bool DepthAliasesEnabled() { return false; }
bool LongProbeEnabled() { return false; }
bool IsDepthFormat(plume::RenderFormat format) { return format == plume::RenderFormat::D32_FLOAT_S8_UINT; }
void RefreshPublishedTextureAuthority(TextureResource& resource) {
  CHECK(!resource.surface && !resource.resolved_on_host); // production early-return branch
}
plume::RenderCommandList* NativeResourceCommands(plume::RenderCommandList*) {
  return reinterpret_cast<plume::RenderCommandList*>(0x2000);
}
bool SettleDepthAlias(u32, TextureResource&, plume::RenderCommandList*, bool&) { CHECK(false); return false; }
TexturePreparationFootprint TexturePreparationFootprintOf(TextureResource&, const std::array<u32,7>*) {
  TexturePreparationFootprint result;
  result.ranges[0] = {REX_KERNEL_MEMORY()->GetPhysicalAddress(kHeader), sizeof(D3DTexture)};
  result.ranges[1] = {REX_KERNEL_MEMORY()->GetPhysicalAddress(kPayload), kBytes};
  return result;
}
TextureResourceView TextureResourceViewOf(u32, const std::shared_ptr<TextureResource>& resource) {
  TextureResourceView result;
  result.texture = resource->texture;
  result.descriptor_index = 1;
  result.format = resource->format;
  return result;
}

// Native worker entry point remains unchanged; execution of queued callables
// is held by the fixture to choose a permitted, deterministic schedule.
std::mutex g_stage_mutex;
std::vector<std::function<void()>> g_staged_captures, ready;
#include "capture-constant.inc"
bool StagedCapturesEnabled() { return true; }
void StartNativeWorker() {}
void WaitForQueueSpace() {}
void NativeWorkerSubmit(std::function<void()> task, bool, bool, bool) { ready.push_back(std::move(task)); }
void FlushStagedCapturesLocked() {
  for (auto& task : g_staged_captures) ready.push_back(std::move(task));
  g_staged_captures.clear();
}
#include "texture-capture-production.inc"

void Header(TextureResource& resource) {
  auto* header = REX_KERNEL_MEMORY()->TranslateVirtual<D3DTexture*>(kHeader);
  header->resource.common = resource.adopted_header_type;
  std::array<u32,6> words{};
  std::memcpy(words.data(), &resource.guest_fetch, sizeof(resource.guest_fetch));
  for (u32 i=0; i<6; ++i) header->format.dword[i] = words[i];
}
void Write(u8 pattern) {
  // A real protected alias write faults through the pinned shipped SDK.
  std::memset(REX_KERNEL_MEMORY()->TranslateVirtual<void*>(kPayload), pattern, kBytes);
}
std::vector<u8> Read() {
  std::vector<u8> bytes(kBytes);
  CHECK(CopyTextureUploadSpan(kPayload, bytes));
  return bytes;
}
void PrepareCurrent() {
  const CpuMemorySpan span{kPayload, kBytes};
  current->cpu_stamp = WatchCpuMemory({&span,1});
  current->fixture_cache_owner = std::make_shared<TextureUploadPublication>(Read());
  current->prepared_upload = current->fixture_cache_owner;
  current->guest_uploaded = true;
}
void ExpectPayload(u8 pattern) {
  CHECK(submitted_payload.size() == kBytes);
  for (const auto byte : submitted_payload) CHECK(byte == pattern);
}
void DrainStaged() {
  { std::lock_guard lock(g_stage_mutex); FlushStagedCapturesLocked(); }
  auto tasks = std::move(ready); ready.clear();
  for (auto& task : tasks) task();
}
} // namespace legodimensions::gpu_native

int main(int argc, char**) {
  using namespace legodimensions::gpu_native;
  char loaded[MAX_PATH]{};
  CHECK(GetModuleFileNameA(GetModuleHandleA("rexruntime.dll"),loaded,MAX_PATH));
  CHECK(std::getenv("LEGO_TEST_RUNTIME_EXPECTED"));
  CHECK(!_stricmp(loaded,std::getenv("LEGO_TEST_RUNTIME_EXPECTED")));
  rex::memory::Memory memory; CHECK(memory.Initialize()); fixture::memory = &memory;
  auto* heap = memory.LookupHeap(0xA2000000); CHECK(heap);
  CHECK(heap->AllocFixed(0xA2000000,65536,65536,
      rex::memory::kMemoryAllocationReserve | rex::memory::kMemoryAllocationCommit,
      rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite));
  current = std::make_shared<TextureResource>(); Header(*current);

  Write(0x11); PrepareCurrent();
  const auto initial = CaptureTexturePreparation(kHeader,current);
  CHECK(initial.owner == current && CpuMemoryUnchanged(current->cpu_stamp));
  CHECK(PrepareCachedTexture(initial).texture); ExpectPayload(0x11);

  // Later consumer B updates the same resource's reusable upload. A retained
  // resource/header is insufficient to preserve the earlier capture A.
  const auto old_stamp = current->cpu_stamp;
  std::thread writer([] { Write(0x22); }); writer.join();
  CHECK(!CpuMemoryUnchanged(old_stamp));
  PrepareCurrent();
  CHECK(PrepareCachedTexture(initial).texture); ExpectPayload(0x22);
  std::puts("REPRO captureA -> real CPU fault/writeB -> prepareB -> prepareA consumes B");
  if (argc > 1) { ExpectPayload(0x11); } // desired invariant currently fails

  // Before any later upload, the cache correctly rejects dirty current bytes.
  // The ordinary upload's source capture still reads the later payload B.
  Write(0x33); PrepareCurrent();
  const auto dirty_capture = CaptureTexturePreparation(kHeader,current);
  Write(0x44); CHECK(!PrepareCachedTexture(dirty_capture).texture);
  auto later = TextureUploadSource::Capture(kPayload,kBytes,0,0,CopyTextureUploadSpan);
  CHECK(later && later->Base().front() == 0x44);
  std::puts("REPRO captureA -> real CPU fault/writeB -> cache refusal -> source capture reads B");

  Write(0x55); PrepareCurrent();
  const auto submit_expected = Read();
  std::vector<u8> coordinator_payload;
  NativeWorkerSubmitCapture([&] {
    auto capture = CaptureTexturePreparation(kHeader,current);
    CHECK(capture.owner == current);
    coordinator_payload = Read();
  });
  CHECK(coordinator_payload.empty() && g_staged_captures.size() == 1);
  Write(0x66); DrainStaged();
  CHECK(submit_expected.front() == 0x55 && coordinator_payload.front() == 0x66);
  std::puts("REPRO submitA -> real CPU fault/writeB -> actual staged capture reads B");

  // Controls: real header mismatches reject the cache; replacing the registry
  // owner alone does not erase the existing strong owner in a snapshot.
  Write(0x77); PrepareCurrent();
  const auto header_capture = CaptureTexturePreparation(kHeader,current);
  current->adopted_header_type = 17; Header(*current);
  CHECK(!PrepareCachedTexture(header_capture).texture);
  current->adopted_header_type = 3; Header(*current);
  const auto retained_capture = CaptureTexturePreparation(kHeader,current);
  auto retained = current;
  current = std::make_shared<TextureResource>(); current->texture = nullptr;
  CHECK(PrepareCachedTexture(retained_capture).texture); ExpectPayload(0x77);
  CHECK(retained_capture.owner == retained && retained_capture.owner != current);
  retired.clear(); current.reset(); ShutdownCpuMemoryWatch();
  std::puts("PASS scheduling reproduction and header/retained-owner controls; no GPU or game");
}
