#define NOMINMAX
#include <plume_d3d12.h>
#include <d3d12sdklayers.h>
#include "gpu_native/draw_preparation.h"
#include "gpu_native/native_recording.h"
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <vector>
#include <immintrin.h>
namespace plume { std::unique_ptr<RenderInterface> CreateD3D12Interface(); }
using namespace legodimensions::gpu_native;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "failed line %d: %s\n", __LINE__, #x); return 23; } } while (0)
std::vector<char> Read(const char* path) {
  std::ifstream f(path, std::ios::binary | std::ios::ate);
  if (!f) std::exit(3);
  std::vector<char> bytes(size_t(f.tellg()));
  f.seekg(0); f.read(bytes.data(), bytes.size()); return bytes;
}
struct Slice {
  plume::RenderBuffer* buffer = nullptr;
  void* mapped = nullptr;
  uint64_t offset = 0;
  explicit operator bool() const { return buffer && mapped; }
};
int main(int argc, char** argv) {
  CHECK(argc == 3 || argc == 4);
  const bool stale_serial = argc == 4;
  const bool detached = NativeFullDrawPreparationEnabled();
  const bool blend_factors = std::getenv("LEGO_TEST_BLEND_FACTORS") != nullptr;
  {
    auto empty = CreateNativeDrawCommands();
    CHECK(empty->Empty());
    empty->Replay(nullptr);
  }
  ID3D12Debug* debug = nullptr;
  if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) { debug->EnableDebugLayer(); debug->Release(); }
  auto api = plume::CreateD3D12Interface();
  auto device = api->createDevice(); CHECK(device);
  auto* raw = static_cast<plume::D3D12Device*>(device.get())->d3d;
  auto queue = device->createCommandQueue(plume::RenderCommandListType::DIRECT);
  auto commands = PrepareNativeCommandList(queue->createCommandList()); auto fence = device->createCommandFence();
  auto vbytes = Read(argv[1]), pbytes = Read(argv[2]);
  auto vs = device->createShader(vbytes.data(), vbytes.size(), "main", plume::RenderShaderFormat::DXIL);
  auto ps = device->createShader(pbytes.data(), pbytes.size(), "main", plume::RenderShaderFormat::DXIL);
  plume::RenderPipelineLayoutBuilder builder;
  builder.begin(false, true);
  for (uint32_t i = 0; i < 3; ++i) builder.addRootDescriptor(i, 0, plume::RenderRootDescriptorType::CONSTANT_BUFFER);
  builder.end(); auto layout = builder.create(device.get());
  builder.begin(false, true);
  builder.addRootDescriptor(7, 0, plume::RenderRootDescriptorType::CONSTANT_BUFFER);
  builder.end(); auto other_layout = builder.create(device.get());
  plume::RenderGraphicsPipelineDesc desc;
  desc.pipelineLayout = layout.get(); desc.vertexShader = vs.get(); desc.pixelShader = ps.get();
  desc.primitiveTopology = plume::RenderPrimitiveTopology::TRIANGLE_LIST;
  desc.renderTargetCount = 1; desc.renderTargetFormat[0] = plume::RenderFormat::R32G32B32A32_FLOAT;
  desc.renderTargetBlend[0] = plume::RenderBlendDesc::Copy();
  if (blend_factors) {
    desc.renderTargetBlend[0].blendEnabled = true;
    desc.renderTargetBlend[0].srcBlend = plume::RenderBlend::BLEND_FACTOR;
  }
  desc.cullMode = plume::RenderCullMode::NONE; desc.fillMode = plume::RenderFillMode::SOLID;
  auto pipeline = device->createGraphicsPipeline(desc);
  ID3D12InfoQueue* messages = nullptr;
  if (SUCCEEDED(raw->QueryInterface(IID_PPV_ARGS(&messages)))) {
    for (UINT64 i = 0; i < messages->GetNumStoredMessages(); ++i) {
      SIZE_T size = 0; messages->GetMessage(i, nullptr, &size);
      std::vector<uint8_t> storage(size); auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
      messages->GetMessage(i, message, &size); std::fprintf(stderr, "%s\n", message->pDescription);
    }
    messages->Release();
  }
  CHECK(pipeline && static_cast<plume::D3D12GraphicsPipeline*>(pipeline.get())->d3d);
  plume::RenderTextureDesc td;
  td.dimension = plume::RenderTextureDimension::TEXTURE_2D; td.width = td.height = 8;
  td.depth = td.arraySize = td.mipLevels = 1; td.format = desc.renderTargetFormat[0];
  td.committed = true; td.flags = plume::RenderTextureFlag::RENDER_TARGET;
  td.multisampling.sampleCount = plume::RenderSampleCount::COUNT_1;
  auto target = device->createTexture(td);
  const plume::RenderTexture* attachments[]{target.get()};
  auto fb = device->createFramebuffer(plume::RenderFramebufferDesc(attachments, 1));
  auto* target_raw = static_cast<plume::D3D12Texture*>(target.get())->d3d;
  auto target_desc = target_raw->GetDesc();
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{}; UINT64 read_bytes;
  raw->GetCopyableFootprints(&target_desc, 0, 1, 0, &footprint, nullptr, nullptr, &read_bytes);
  read_bytes = (read_bytes + 511) & ~UINT64(511);
  constexpr unsigned steps = 64;
  auto readback = device->createBuffer(plume::RenderBufferDesc::ReadbackBuffer(read_bytes * steps));
  auto upload = device->createBuffer(plume::RenderBufferDesc::UploadBuffer(1024 * 1024));
  auto* mapped = static_cast<uint8_t*>(upload->map()); CHECK(mapped);
  unsigned cases = 0;
  for (bool retained : {false, true}) {
    PreparedConstantBanks<Slice> banks;
    std::array<PreparedConstantBanks<Slice>, 8> worker_banks;
    std::array<float, 3> values{0.25f, 0.5f, 0.75f};
    for (uint64_t serial : {1ull, 2ull}) {
      std::memset(mapped, 0xCD, 1024 * 1024);
      uint32_t used = 0;
      auto allocate = [&](uint32_t bytes) {
        auto offset = used; used += (bytes + 255) & ~255u;
        if (used > 1024 * 1024) std::exit(23);
        return Slice{upload.get(), mapped + offset, offset};
      };
      std::array<PreparedConstantBanks<Slice>::Packet, steps> packets{};
      std::array<std::array<float, 3>, steps> expected;
      std::array<std::array<Slice, 3>, steps> prepared;
      for (unsigned step = 0; step < steps; ++step) {
        if (step >= 2 && step <= 4) values[step - 2] += 0.125f;
        expected[step] = values;
        constexpr unsigned register_offsets[]{73 * 16, 191 * 16, 0};
        for (unsigned bank = 0; bank < 3; ++bank)
          std::memcpy(packets[step].data() + PreparedConstantBanks<Slice>::offsets[bank] + register_offsets[bank], &values[bank], 4);
      }
      const unsigned workers = NativePreparationWorkerCount();
      std::vector<std::future<void>> jobs;
      const unsigned chunk = workers ? (steps + workers - 1) / workers : steps;
      for (unsigned first = 0, job = 0; workers && first < steps; first += chunk, ++job) {
        const unsigned end = std::min(steps, first + chunk);
        jobs.push_back(PostNativePreparation([&, first, end, job] {
          auto& local = worker_banks[job];
          auto copy = [](void* dst, const void* src, uint32_t bytes) { std::memcpy(dst, src, bytes); };
          for (unsigned step = first; step < end; ++step) {
            const auto& packet = packets[step];
            auto reserve = [&](uint32_t bytes) {
              if (bytes > 8960) std::abort();
              return Slice{upload.get(), mapped + step * 8960, step * 8960};
            };
            if (!retained) local.Reset();
            local.UpdateFloatBank(0, {packet.data(), 4096}, 0, copy);
            local.UpdateFloatBank(1, {packet.data() + 4096, 4096}, 0, copy);
            local.UpdateShared({packet.data() + 8192, 624});
            local.SetUploadRegisters(step & 1 ? 256 : 74, step & 1 ? 256 : 192);
            if (!local.ResolveUploads(stale_serial ? 1 : serial, reserve, prepared[step])) std::abort();
          }
          _mm_sfence();
        }, (end - first) * sizeof(packets[0])));
      }
      for (auto& job : jobs) job.get();
      commands->begin(); commands->setGraphicsPipelineLayout(layout.get()); commands->setPipeline(pipeline.get());
      commands->setViewports(plume::RenderViewport(0, 0, 8, 8)); commands->setScissors(plume::RenderRect(0, 0, 8, 8));
      auto record_step = [&](unsigned step, plume::RenderCommandList* commands) {
        commands->setGraphicsPipelineLayout(layout.get()); commands->setPipeline(pipeline.get());
        commands->setViewports(plume::RenderViewport(0,0,8,8)); commands->setScissors(plume::RenderRect(0,0,8,8));
        if (step == 5) { commands->setGraphicsPipelineLayout(other_layout.get()); commands->setGraphicsPipelineLayout(layout.get()); }
        if (step == 6) {
          const auto address = static_cast<plume::D3D12Buffer*>(upload.get())->d3d->GetGPUVirtualAddress() + 524288;
          RecordNativeCommand(commands, [address](auto* list) {
            auto* native = static_cast<plume::D3D12CommandList*>(list);
            native->d3d->SetGraphicsRootConstantBufferView(0, address);
            native->notifyCommandStateWasChangedExternally();
          });
        }
        const auto& packet = packets[step];
        std::array<Slice, 3> slices;
        if (workers) slices = prepared[step];
        else if (retained) {
          auto copy = [](void* dst, const void* src, uint32_t bytes) { std::memcpy(dst, src, bytes); };
          banks.UpdateFloatBank(0, {packet.data(), 4096}, 0, copy);
          banks.UpdateFloatBank(1, {packet.data() + 4096, 4096}, 0, copy);
          banks.UpdateShared({packet.data() + 8192, 624});
          banks.SetUploadRegisters(step & 1 ? 256 : 74, step & 1 ? 256 : 192);
          CHECK(banks.ResolveUploads(stale_serial ? 1 : serial, allocate, slices,
              [&](void* dst, const void* src, uint32_t bytes) { RecordNativeUpload(commands, dst, src, bytes); }));
        } else {
          auto allocation = allocate(packet.size());
          std::memcpy(allocation.mapped, packet.data(), packet.size());
          for (unsigned i = 0; i < 3; ++i) slices[i] = {allocation.buffer, mapped + allocation.offset + PreparedConstantBanks<Slice>::offsets[i], allocation.offset + PreparedConstantBanks<Slice>::offsets[i]};
        }
        for (unsigned i = 0; i < 3; ++i) commands->setGraphicsRootDescriptor(slices[i].buffer->at(slices[i].offset), i);
        commands->barriers(plume::RenderBarrierStage::GRAPHICS, plume::RenderTextureBarrier(target.get(), plume::RenderTextureLayout::COLOR_WRITE));
        commands->setFramebuffer(fb.get()); commands->clearColor(0, plume::RenderColor(-1, -1, -1, -1));
        if (blend_factors) {
          const std::array<float, 4> factor{step % 4 ? 0.5f : 0.25f, 0.75f, 0.125f, 1.0f};
          if (step == 6) RecordNativeCommand(commands, [](auto* list) {
            const float wrong[4]{};
            static_cast<plume::D3D12CommandList*>(list)->d3d->OMSetBlendFactor(wrong);
          });
          SetNativeBlendFactor(commands, factor);
          SetNativeBlendFactor(commands, factor);
        }
        commands->drawInstanced(3, 1, 0, 0);
        commands->setFramebuffer(nullptr);
        commands->barriers(plume::RenderBarrierStage::COPY, plume::RenderTextureBarrier(target.get(), plume::RenderTextureLayout::COPY_SOURCE));
        D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
        src.pResource = target_raw; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.pResource = static_cast<plume::D3D12Buffer*>(readback.get())->d3d;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint = footprint; dst.PlacedFootprint.Offset = step * read_bytes;
        RecordNativeCommand(commands, [dst, src](auto* list) {
          static_cast<plume::D3D12CommandList*>(list)->d3d->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        });
        return 0;
      };
      if (detached) {
        std::array<std::unique_ptr<NativeDrawCommands>,steps> draws;
        for(auto& draw:draws)draw=CreateNativeDrawCommands();
        std::vector<std::future<void>> draw_jobs;
        for(unsigned first=0;first<steps;first+=chunk){
          const auto end=std::min(steps,first+chunk);
          draw_jobs.push_back(PostNativePreparation([&,first,end]{
            for(unsigned step=first;step<end;++step)if(record_step(step,draws[step].get()))std::abort();
          },(end-first)*4096));
        }
        for(auto& job:draw_jobs)job.get();
        for(auto& draw:draws){draw->Replay(commands.get());draw->Reset();}
      } else for(unsigned step=0;step<steps;++step)CHECK(record_step(step,commands.get())==0);
      commands->end(); queue->executeCommandLists(NativeImmediateCommands(commands.get()), fence.get()); queue->waitForCommandFence(fence.get());
      auto* bytes = static_cast<const uint8_t*>(readback->map()); CHECK(bytes);
      for (unsigned step = 0; step < steps; ++step) {
        auto* actual = reinterpret_cast<const float*>(bytes + step * read_bytes + 4 * footprint.Footprint.RowPitch + 4 * 16);
        const float factors[]{step % 4 ? 0.5f : 0.25f, 0.75f, 0.125f};
        for (unsigned c = 0; c < 3; ++c) CHECK(std::isfinite(actual[c]) && std::abs(actual[c] - expected[step][c] * (blend_factors ? factors[c] : 1.0f)) < 0.00001f);
        ++cases;
      }
      readback->unmap();
    }
  }
  upload->unmap();
  std::printf("PASS %u GPU readbacks: retained/legacy banks, immutable prior draws, root changes, external state, new recordings\n", cases);
}
