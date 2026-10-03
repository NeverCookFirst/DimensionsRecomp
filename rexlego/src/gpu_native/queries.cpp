#include "gpu_native/queries.h"
#include "gpu_native/device.h"
#include <plume_d3d12.h>
#include <wrl/client.h>
#include "gpu_native/renderer_route.h"
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
  std::vector<std::shared_ptr<Samples>> draws;
  ComPtr<ID3D12Fence> completion;
};
std::mutex mutex;
std::unordered_map<u32, std::shared_ptr<Query>> queries;
std::atomic<u32> diagnostics{0};
be_u32* Header(u32 address) { return REX_KERNEL_MEMORY()->TranslateVirtual<be_u32*>(address); }
u32 Unsupported(const char* operation, u32 value) {
  if (diagnostics.fetch_add(1) < 32)
    REXLOG_ERROR("Native GPU: unsupported query {}={} (no fabricated result)", operation, value);
  return kUnsupported;
}
u32 CreateQuery(u32 device_address, u32 type, u32 tiles) {
  auto recording = HostDevice::LockRecording();
  if (type != 9 || HostDevice::BackendName() != "D3D12") {
    Unsupported("type/backend", type); return 0;
  }
  auto* device = static_cast<plume::D3D12Device*>(HostDevice::Device());
  if (!device) return 0;
  auto query = std::make_shared<Query>();
  if (FAILED(device->d3d->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                    IID_PPV_ARGS(&query->completion)))) return 0;
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
    auto* device = static_cast<plume::D3D12Device*>(HostDevice::Device());
    auto next = std::make_shared<Query>();
    if (!device || FAILED(device->d3d->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                                  IID_PPV_ARGS(&next->completion)))) return kFailure;
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
  const bool signaled = submitted && queue &&
      SUCCEEDED(queue->d3d->Signal(query->completion.Get(), 1));
  std::lock_guard lock(mutex);
  query->ended = true; query->failed |= !signaled;
  return signaled ? 0 : kFailure;
}
u32 GetQueryData(u32 address, u32 output, u32 size, u32 /*flags*/) {
  auto recording = HostDevice::LockRecording();
  std::lock_guard lock(mutex);
  auto it = queries.find(address);
  if (it == queries.end()) return kInvalid;
  const auto& query = *it->second;
  if (size != 4 || !output) return Unsupported("GetData size", size);
  if (query.failed) return kFailure;
  if (!query.ended) return 1;
  const u64 completed = query.completion->GetCompletedValue();
  if (completed == UINT64_MAX) return kFailure;  // Device removal isn't completion.
  if (completed < 1) return 1;
  u64 sum = 0;
  for (const auto& draw : query.draws) {
    const D3D12_RANGE read_range{0, sizeof(u64)};
    void* mapped = nullptr;
    if (FAILED(draw->readback->Map(0, &read_range, &mapped))) return kFailure;
    sum += *static_cast<const u64*>(mapped);
    const D3D12_RANGE no_writes{0, 0};
    draw->readback->Unmap(0, &no_writes);
  }
  *Header(output) = static_cast<u32>(std::min<u64>(sum, UINT32_MAX));
  if (diagnostics.fetch_add(1) < 32)
    REXLOG_INFO("Native GPU: query 0x{:08X} complete draws={} samples={}", address, query.draws.size(), sum);
  return 0;
}
u32 ReleaseQuery(u32 address) {
  auto recording = HostDevice::LockRecording();
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
  plume::D3D12CommandList* commands;
  std::shared_ptr<Samples> samples;
  std::vector<std::shared_ptr<Query>> consumers;
};
QueryDrawScope::QueryDrawScope(plume::RenderCommandList* commands) {
  auto scope = std::make_unique<Impl>();
  {
    std::lock_guard lock(mutex);
    for (const auto& [address, query] : queries)
      if (query->active) scope->consumers.push_back(query);
  }
  if (scope->consumers.empty()) return;
  scope->commands = static_cast<plume::D3D12CommandList*>(commands);
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
  scope->commands->d3d->BeginQuery(scope->samples->heap.Get(), D3D12_QUERY_TYPE_OCCLUSION, 0);
  impl_ = std::move(scope);
}
QueryDrawScope::~QueryDrawScope() {
  if (!impl_) return;
  auto& scope = *impl_;
  scope.commands->d3d->EndQuery(scope.samples->heap.Get(), D3D12_QUERY_TYPE_OCCLUSION, 0);
  scope.commands->d3d->ResolveQueryData(scope.samples->heap.Get(), D3D12_QUERY_TYPE_OCCLUSION,
                                      0, 1, scope.samples->readback.Get(), 0);
  std::lock_guard lock(mutex);
  for (auto& query : scope.consumers) query->draws.push_back(scope.samples);
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
}
}
REX_HOOK(sub_83FB2360, legodimensions::gpu_native::CreateQuery);
REX_HOOK(sub_83FB0738, legodimensions::gpu_native::IssueQuery);
REX_HOOK(sub_83FB09E8, legodimensions::gpu_native::GetQueryData);
REX_HOOK(sub_83FB05B0, legodimensions::gpu_native::ReleaseQuery);
