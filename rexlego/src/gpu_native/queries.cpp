#include "gpu_native/native_recording.h"
#include "gpu_native/native_worker.h"
#include "gpu_native/queries.h"
#include "gpu_native/device.h"
#include <plume_d3d12.h>
#include <wrl/client.h>
#include "gpu_native/renderer_route.h"
#include "gpu_native/fsr3_settings.h"
#include "gpu_native/long_probe.h"
#include <array>
#include <cmath>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <algorithm>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace legodimensions::gpu_native {
namespace {
using Microsoft::WRL::ComPtr;
constexpr u32 kInvalid = 0x8876086Cu, kUnsupported = 0x80004001u, kFailure = 0x80004005u;
struct Samples { ComPtr<ID3D12QueryHeap> heap; ComPtr<ID3D12Resource> readback; };
struct Query {
  bool active = false, ended = false, failed = false;
  // Set once the samples were read back; later polls return it without mapping again.
  bool summed = false;
  u32 sum = 0;
  // Each draw's samples with the factor that brings them back to guest resolution.
  std::vector<std::pair<std::shared_ptr<Samples>, double>> draws;
  ComPtr<ID3D12Fence> completion;
  u64 completion_value = 0;
};
std::mutex mutex;
std::unordered_map<u32, std::shared_ptr<Query>> queries;
std::unordered_map<u32, unsigned> pending_issues;
// Query generations retain samples through every submission that uses them.
// The pool's sole reference is therefore proof that both GPU work and readers
// have released a pair. Never reuse by frame number or an assumed latency.
std::vector<std::shared_ptr<Samples>> sample_pool;
constexpr size_t kMaxPooledSamples = 256;
ComPtr<ID3D12Fence> query_completion;
u64 query_completion_value = 0;
std::atomic<u32> diagnostics{0};
be_u32* Header(u32 address) { return REX_KERNEL_MEMORY()->TranslateVirtual<be_u32*>(address); }
u32 Unsupported(const char* operation, u32 value) {
  if (diagnostics.fetch_add(1) < 32)
    REXLOG_ERROR("Native GPU: unsupported query {}={} (no fabricated result)", operation, value);
  return kUnsupported;
}
void TraceQueryValue(u32 address,size_t draws,u64 samples) {
  static const u64 from=[] {
    const char* value=std::getenv("LEGO_NATIVE_QUERY_TRACE_FROM");
    return value ? std::strtoull(value,nullptr,10) : ~u64{0};
  }();
  if (g_probe_frame.load()<from) return;
  const bool enabled=NativeFsr3Enabled();
  const auto size=NativeSceneRenderSize();
  const unsigned mode=!enabled?0:size.width==1920?1:size.width==1280?2:size.width==1129?3:size.width==960?4:5;
  static std::array<u32,6> counts{};
  if (counts[mode]++<128)
    REXLOG_INFO("Native query value frame={} fsr={} size={}x{} query={:08X} draws={} samples={}",
        g_probe_frame.load(),enabled,size.width,size.height,address,draws,samples);
}
u32 CreateQuery(u32 device_address, u32 type, u32 tiles) {
  auto recording = NativeFullDrawPreparationEnabled() ? std::unique_lock(NativeRecordingMutex()) : HostDevice::LockRecording();
  if (type != 9 || HostDevice::BackendName() != "D3D12") {
    Unsupported("type/backend", type); return 0;
  }
  auto* device = static_cast<plume::D3D12Device*>(HostDevice::Device());
  if (!device) return 0;
  auto query = std::make_shared<Query>();
  if (!query_completion && FAILED(device->d3d->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                    IID_PPV_ARGS(&query_completion)))) return 0;
  query->completion = query_completion;
  auto* memory = REX_KERNEL_MEMORY();
  const u32 address = memory->SystemHeapAlloc(156, 16);
  if (!address) return 0;
  memory->Zero(address, 156);
  auto* words = Header(address);
  words[0] = device_address; words[1] = type; words[3] = 1;
  words[37] = 1; words[38] = tiles;  // Native target is whole, no Xbox tile replay.
  std::lock_guard lock(mutex);
  queries.emplace(address, std::move(query));
  if (diagnostics.fetch_add(1) < 32)
    REXLOG_INFO("Native GPU: created hardware occlusion query 0x{:08X} tiles={} with asynchronous readback generations", address, tiles);
  return address;
}
u32 IssueQuery(u32 address, u32 flags) {
  auto recording = HostDevice::LockRecording();
  std::shared_ptr<Query> query;
  {
    std::lock_guard lock(mutex);
    auto it = queries.find(address);
    if (it == queries.end()) return kInvalid;
    query = it->second;
    if (flags == 2 && query->active) return kInvalid;
  }
  if (flags == 2) {
    auto next = std::make_shared<Query>();
    next->completion = query->completion;
    // Each BEGIN owns fresh query/readback storage. Older GPU work keeps its
    // generation alive at every pending submission, without draining the GPU.
    next->active = true;
    HostDevice::RetireResource(query);
    {
      std::lock_guard lock(mutex);
      queries[address] = std::move(next);
    }
    return 0;
  }
  if (flags != 1) return Unsupported("issue flags", flags);
  {
    std::lock_guard lock(mutex);
    if (!query->active) return kInvalid;
    query->active = false;
  }
  // Each draw has already ended/resolved its query. Submit before signaling.
  const bool submitted = HostDevice::SubmitRecordedWork();
  auto* queue = static_cast<plume::D3D12CommandQueue*>(HostDevice::Queue());
  // All query ends are ordered on the same direct queue. One monotonic fence
  // replaces a kernel/driver fence allocation for every guest BEGIN.
  const u64 value = query_completion_value == UINT64_MAX ? 0 : ++query_completion_value;
  const bool signaled = submitted && queue && value &&
      SUCCEEDED(queue->d3d->Signal(query->completion.Get(), value));
  std::lock_guard lock(mutex);
  query->completion_value = value;
  query->ended = true; query->failed |= !signaled;
  return signaled ? 0 : kFailure;
}
// Threaded recording: BEGIN/END must stay in order with the queued draws whose
// samples they bracket. The guest only distinguishes success here.
u32 IssueQueryQueued(u32 address, u32 flags) {
  if (NativeWorkerShouldQueue()) {
    const bool track = NativeFullDrawPreparationEnabled();
    if (track) { std::lock_guard lock(mutex); ++pending_issues[address]; }
    // Preparation reads active queries, so a barrier holds later batches until the recorder applies BEGIN or END.
    NativeWorkerSubmit([address, flags, track] {
      IssueQuery(address, flags);
      if (track) {
        std::lock_guard lock(mutex);
        if (--pending_issues[address] == 0) pending_issues.erase(address);
      }
    }, false, false, false, track ? NativeTaskStage::kRecorderBarrier : NativeTaskStage::kRecorder);
    return 0;
  }
  return IssueQuery(address, flags);
}
u32 GetQueryData(u32 address, u32 output, u32 size, u32 /*flags*/) {
  // Query state has its own mutex and readbacks map after their fence, so full preparation skips the gate.
  static const bool gate_free = [] {
    const char* value = std::getenv("LEGO_NATIVE_GATE_FREE_QUERY_DATA");
    return !value || value[0] != '0';
  }();
  std::unique_lock<NativeRecordingGate> recording;
  if (!NativeFullDrawPreparationEnabled()) recording = HostDevice::LockRecording();
  else if (!gate_free) recording = std::unique_lock(NativeRecordingMutex());
  std::lock_guard lock(mutex);
  auto it = queries.find(address);
  if (it == queries.end()) return kInvalid;
  const auto& query = *it->second;
  if (size != 4 || !output) return Unsupported("GetData size", size);
  if (pending_issues.contains(address)) return 1;
  if (query.failed) return kFailure;
  if (!query.ended) return 1;
  const u64 completed = query.completion->GetCompletedValue();
  if (completed == UINT64_MAX) return kFailure;  // Device removal isn't completion.
  if (!query.completion_value || completed < query.completion_value) return 1;
  if (query.summed) {
    *Header(output) = query.sum;
    return 0;
  }
  double scaled = 0;
  for (const auto& [draw, scale] : query.draws) {
    const D3D12_RANGE read_range{0, sizeof(u64)};
    void* mapped = nullptr;
    if (FAILED(draw->readback->Map(0, &read_range, &mapped))) return kFailure;
    scaled += double(*static_cast<const u64*>(mapped)) * scale;
    const D3D12_RANGE no_writes{0, 0};
    draw->readback->Unmap(0, &no_writes);
  }
  const u64 sum = u64(std::llround(scaled));
  *Header(output) = static_cast<u32>(std::min<u64>(sum, UINT32_MAX));
  it->second->summed = true;
  it->second->sum = static_cast<u32>(std::min<u64>(sum, UINT32_MAX));
  TraceQueryValue(address,query.draws.size(),sum);
  if (diagnostics.fetch_add(1) < 32)
    REXLOG_INFO("Native GPU: query 0x{:08X} complete draws={} samples={}", address, query.draws.size(), sum);
  return 0;
}
u32 ReleaseQuery(u32 address) {
  // Full preparation: the map is its own lock and the retire queues behind the tapes.
  auto recording = NativeFullDrawPreparationEnabled() && NativeWorkerShouldQueue()
      ? std::unique_lock<NativeRecordingGate>() : HostDevice::LockRecording();
  std::shared_ptr<Query> released;
  {
    std::lock_guard lock(mutex);
    if (!queries.contains(address)) return 0;
    const u32 refs = Header(address)[3];
    if (refs > 1) { Header(address)[3] = refs - 1; return refs - 1; }
    released = std::move(queries[address]);
    released->active = false;
    queries.erase(address);
  }
  // GPU commands refer to host heaps/readbacks, never the guest header.
  // Retain the generation even for release-before-END; return the header now.
  HostDevice::RetireResource(std::move(released));
  REX_KERNEL_MEMORY()->SystemHeapFree(address);
  return 0;
}
}
struct QueryDrawScope::Impl {
  plume::RenderCommandList* commands;
  double sample_scale = 1.0;
  std::shared_ptr<Samples> samples;
  std::vector<std::shared_ptr<Query>> consumers;
};
QueryDrawScope::QueryDrawScope(plume::RenderCommandList* commands, double sample_scale) {
  std::unique_ptr<Impl> scope;
  {
    std::lock_guard lock(mutex);
    if (std::none_of(queries.begin(),queries.end(),
        [](const auto& entry) { return entry.second->active; })) return;
    scope = std::make_unique<Impl>();
    for (const auto& [address, query] : queries)
      if (query->active) scope->consumers.push_back(query);
  }
  if (scope->consumers.empty()) return;
  scope->commands = commands;
  scope->sample_scale = sample_scale;
  {
    std::lock_guard lock(mutex);
    for (const auto& samples : sample_pool) {
      if (samples.use_count() == 1) {
        scope->samples = samples;
        break;
      }
    }
  }
  if (scope->samples) {
    RecordNativeCommand(commands, [samples = scope->samples](auto* list) {
      static_cast<plume::D3D12CommandList*>(list)->d3d->BeginQuery(samples->heap.Get(), D3D12_QUERY_TYPE_OCCLUSION, 0);
    });
    impl_ = std::move(scope);
    return;
  }
  auto* device = static_cast<plume::D3D12Device*>(HostDevice::Device());
  auto samples = std::make_shared<Samples>();
  D3D12_QUERY_HEAP_DESC query_desc{};
  query_desc.Type = D3D12_QUERY_HEAP_TYPE_OCCLUSION; query_desc.Count = 1;
  D3D12_HEAP_PROPERTIES heap{};
  heap.Type = D3D12_HEAP_TYPE_READBACK; heap.CreationNodeMask = heap.VisibleNodeMask = 1;
  D3D12_RESOURCE_DESC desc{};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; desc.Width = sizeof(u64);
  desc.Height = desc.DepthOrArraySize = desc.MipLevels = desc.SampleDesc.Count = 1;
  desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  if (!device || !commands ||
      FAILED(device->d3d->CreateQueryHeap(&query_desc, IID_PPV_ARGS(&samples->heap))) ||
      FAILED(device->d3d->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
          D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&samples->readback)))) {
    std::lock_guard lock(mutex);
    for (auto& query : scope->consumers) query->failed = true;
    return;
  }
  scope->samples = std::move(samples);
  {
    std::lock_guard lock(mutex);
    if (sample_pool.size() < kMaxPooledSamples) sample_pool.push_back(scope->samples);
  }
  RecordNativeCommand(commands, [samples = scope->samples](auto* list) {
    static_cast<plume::D3D12CommandList*>(list)->d3d->BeginQuery(samples->heap.Get(), D3D12_QUERY_TYPE_OCCLUSION, 0);
  });
  impl_ = std::move(scope);
}
QueryDrawScope::~QueryDrawScope() {
  if (!impl_) return;
  auto& scope = *impl_;
  RecordNativeCommand(scope.commands, [samples = scope.samples](auto* list) {
    auto* native = static_cast<plume::D3D12CommandList*>(list);
    native->d3d->EndQuery(samples->heap.Get(), D3D12_QUERY_TYPE_OCCLUSION, 0);
    native->d3d->ResolveQueryData(samples->heap.Get(), D3D12_QUERY_TYPE_OCCLUSION,
                                0, 1, samples->readback.Get(), 0);
  });
  std::lock_guard lock(mutex);
  for (auto& query : scope.consumers) query->draws.emplace_back(scope.samples, scope.sample_scale);
}
void FailActiveQueries() {
  std::lock_guard lock(mutex);
  for (auto& [address, query] : queries) {
    if (!query->active) continue;
    if (!query->failed && diagnostics.fetch_add(1) < 32)
      REXLOG_WARN("Native GPU: query 0x{:08X} invalidated by failed draw", address);
    query->failed = true;
  }
}
void ResetQueryResources() {
  std::lock_guard lock(mutex);
  for (const auto& [address, query] : queries) REX_KERNEL_MEMORY()->SystemHeapFree(address);
  queries.clear();
  pending_issues.clear();
  sample_pool.clear();
  query_completion.Reset();
  query_completion_value = 0;
}
}
REX_HOOK(sub_83FB2360, legodimensions::gpu_native::CreateQuery);
REX_HOOK(sub_83FB0738, legodimensions::gpu_native::IssueQueryQueued);
REX_HOOK(sub_83FB09E8, legodimensions::gpu_native::GetQueryData);
REX_HOOK(sub_83FB05B0, legodimensions::gpu_native::ReleaseQuery);
