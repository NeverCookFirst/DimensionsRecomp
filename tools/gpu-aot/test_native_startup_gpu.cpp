#define NOMINMAX
#include <cstdio>
#include <vector>
#include <d3d12sdklayers.h>
#include <plume_render_interface.h>
#include <plume_render_interface_builders.h>
#include <plume_d3d12.h>
#include "gpu_native/shaders/copy_vs.h"
#include "gpu_native/shaders/copy_color_ps.h"
namespace plume { std::unique_ptr<RenderInterface> CreateD3D12Interface(); }
int main() {
  ID3D12Debug* debug = nullptr;
  const auto debug_result = D3D12GetDebugInterface(IID_PPV_ARGS(&debug));
  std::printf("debug_hresult=%08X\n", unsigned(debug_result));
  if (debug) { debug->EnableDebugLayer(); debug->Release(); }
  auto render_interface = plume::CreateD3D12Interface();
  auto device = render_interface->createDevice();
  if (!device) return 2;
  auto* native = static_cast<plume::D3D12Device*>(device.get())->d3d;
  std::printf("adapter=%s\n", device->getDescription().name.c_str());
  D3D12_FEATURE_DATA_SHADER_MODEL model{D3D_SHADER_MODEL_6_6};
  const auto model_result = native->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &model, sizeof(model));
  std::printf("shader_model_hresult=%08X shader_model=%X\n", unsigned(model_result), unsigned(model.HighestShaderModel));
  ID3D12InfoQueue* messages = nullptr;
  native->QueryInterface(IID_PPV_ARGS(&messages));
  plume::RenderPipelineLayoutBuilder builder;
  builder.begin(false, true);
  plume::RenderDescriptorSetBuilder textures;
  textures.begin(); textures.addTexture(0, 65536); textures.end(true, 65536);
  for (int i = 0; i < 3; ++i) builder.addDescriptorSet(textures);
  plume::RenderDescriptorSetBuilder samplers;
  samplers.begin(); samplers.addSampler(0, 1024); samplers.end(true, 1024);
  builder.addDescriptorSet(samplers);
  for (unsigned i : {0u, 1u, 2u, 4u}) builder.addRootDescriptor(i, 4, plume::RenderRootDescriptorType::CONSTANT_BUFFER);
  builder.addPushConstant(3, 4, 16, plume::RenderShaderStageFlag::PIXEL);
  builder.end();
  auto layout = builder.create(device.get());
  auto vs = device->createShader(g_copy_vs_dxil, sizeof(g_copy_vs_dxil), "main", plume::RenderShaderFormat::DXIL);
  auto ps = device->createShader(g_copy_color_ps_dxil, sizeof(g_copy_color_ps_dxil), "main", plume::RenderShaderFormat::DXIL);
  plume::RenderGraphicsPipelineDesc desc;
  desc.pipelineLayout = layout.get(); desc.vertexShader = vs.get(); desc.pixelShader = ps.get();
  desc.depthFunction = plume::RenderComparisonFunction::ALWAYS;
  desc.depthEnabled = false; desc.depthWriteEnabled = false;
  desc.primitiveTopology = plume::RenderPrimitiveTopology::TRIANGLE_LIST;
  desc.cullMode = plume::RenderCullMode::NONE; desc.fillMode = plume::RenderFillMode::SOLID;
  desc.renderTargetCount = 1; desc.renderTargetFormat[0] = plume::RenderFormat::B8G8R8A8_UNORM;
  desc.renderTargetBlend[0] = plume::RenderBlendDesc::Copy();
  auto pipeline = device->createGraphicsPipeline(desc);
  const bool ready = pipeline && static_cast<plume::D3D12GraphicsPipeline*>(pipeline.get())->d3d;
  std::printf("present_pipeline=%s\n", ready ? "ready" : "failed");
  if (messages) {
    for (UINT64 i = 0; i < messages->GetNumStoredMessages(); ++i) {
      SIZE_T size = 0;
      messages->GetMessage(i, nullptr, &size);
      std::vector<unsigned char> storage(size);
      auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
      if (SUCCEEDED(messages->GetMessage(i, message, &size)))
        std::printf("D3D12 severity=%u id=%u %s\n", unsigned(message->Severity), unsigned(message->ID), message->pDescription);
    }
    messages->Release();
  }
  return ready ? 0 : 3;
}
