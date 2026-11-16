#pragma once
#include "gpu_native/present_rect.h"
#include <d3d12.h>
#include <wrl/client.h>
#include <array>
#include <cmath>
#include <cstdint>
#include <map>
#include <algorithm>
#include <bit>

// Reuse the exact local ReXGlue EASU/RCAS programs and constant layout. This
// spatial presenter is independent of the native FSR 3 temporal integration.
namespace legodimensions::gpu_native::fsr1_shaders {
#include "../../../rexglue-sdk/src/ui/shaders/bytecode/d3d12_5_1/guest_output_triangle_strip_rect_vs.h"
#include "../../../rexglue-sdk/src/ui/shaders/bytecode/d3d12_5_1/guest_output_ffx_fsr_easu_ps.h"
#include "../../../rexglue-sdk/src/ui/shaders/bytecode/d3d12_5_1/guest_output_ffx_fsr_rcas_ps.h"
#include "../../../rexglue-sdk/src/ui/shaders/bytecode/d3d12_5_1/guest_output_bilinear_ps.h"
#include "../../../rexglue-sdk/src/ui/shaders/bytecode/d3d12_5_1/guest_output_ffx_fsr_rcas_dither_ps.h"
#include "../../../rexglue-sdk/src/ui/shaders/bytecode/d3d12_5_1/guest_output_bilinear_dither_ps.h"
}
namespace legodimensions::gpu_native {
class Fsr1Presenter {
  template<class T> using ComPtr = Microsoft::WRL::ComPtr<T>;
  struct Slot {
    ComPtr<ID3D12DescriptorHeap> srv, rtv;
    std::array<ComPtr<ID3D12Resource>, 5> intermediate;
  };
  ComPtr<ID3D12Device> device_;
  ComPtr<ID3D12RootSignature> root_;
  std::map<uint32_t, ComPtr<ID3D12PipelineState>> pipelines_;
  std::array<Slot, 12> slots_;
  UINT srv_stride_ = 0, rtv_stride_ = 0;

  ComPtr<ID3D12PipelineState> Pipeline(const BYTE* ps, size_t bytes, DXGI_FORMAT format) {
    D3D12_GRAPHICS_PIPELINE_STATE_DESC p{};
    p.pRootSignature = root_.Get();
    p.VS = {fsr1_shaders::guest_output_triangle_strip_rect_vs,
            sizeof(fsr1_shaders::guest_output_triangle_strip_rect_vs)};
    p.PS = {ps, bytes};
    p.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    p.SampleMask = UINT_MAX;
    p.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    p.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    p.RasterizerState.DepthClipEnable = TRUE;
    p.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    p.NumRenderTargets = 1; p.RTVFormats[0] = format; p.SampleDesc.Count = 1;
    ComPtr<ID3D12PipelineState> result;
    device_->CreateGraphicsPipelineState(&p, IID_PPV_ARGS(&result));
    return result;
  }
  bool Initialize(ID3D12Device* device) {
    if (root_) return device_.Get() == device;
    device_ = device;
    D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0};
    D3D12_ROOT_PARAMETER params[3]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable = {1, &range};
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[1].ParameterType = params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[1].Constants = params[2].Constants = {0, 0, 4};
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MaxAnisotropy = 1; sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC desc{3, params, 1, &sampler, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ComPtr<ID3DBlob> blob, error;
    if (FAILED(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1,
        &blob, &error)) || FAILED(device->CreateRootSignature(0, blob->GetBufferPointer(),
        blob->GetBufferSize(), IID_PPV_ARGS(&root_)))) return false;
    srv_stride_ = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    rtv_stride_ = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    return true;
  }
 public:
  // The host has waited for this command slot's fence before reusing its SRVs,
  // RTVs or intermediate storage. No descriptor used by an in-flight list moves.
  // Input is already SHADER_READ and destination is already COLOR_WRITE in Plume.
  bool Draw(ID3D12Device* device, ID3D12GraphicsCommandList* commands,
      ID3D12Resource* source, ID3D12Resource* destination, uint32_t slot,
      PresentRect rect, float sharpness_reduction, uint32_t max_easu_passes = 4,
      bool dither = false) {
    if (!device || !commands || !source || !destination || source == destination ||
        slot >= slots_.size() || !rect.width || !rect.height || !Initialize(device)) return false;
    const auto input = source->GetDesc(), output = destination->GetDesc();
    if (input.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || input.DepthOrArraySize != 1 ||
        input.SampleDesc.Count != 1 || output.SampleDesc.Count != 1 ||
        uint64_t(rect.x) + rect.width > output.Width || uint64_t(rect.y) + rect.height > output.Height)
      return false;
    // ReXGlue's spatial flow: up to four 2x EASU steps, 1:1 RCAS,
    // followed by bilinear only when a downscale or a remaining stretch is needed.
    struct Pass { uint32_t kind, width, height; ID3D12PipelineState* pipeline; };
    std::array<Pass, 6> passes{};
    uint32_t count = 0, width = uint32_t(input.Width), height = input.Height;
    while (count < std::clamp(max_easu_passes, 1u, 4u) && (width < rect.width || height < rect.height)) {
      width = std::min(width * 2, rect.width); height = std::min(height * 2, rect.height);
      passes[count++] = {0, width, height};
    }
    passes[count++] = {1, width, height};
    if (width != rect.width || height != rect.height) passes[count++] = {2, rect.width, rect.height};
    if (dither) passes[count - 1].kind += 2;
    const BYTE* programs[]{fsr1_shaders::guest_output_ffx_fsr_easu_ps,
        fsr1_shaders::guest_output_ffx_fsr_rcas_ps, fsr1_shaders::guest_output_bilinear_ps,
        fsr1_shaders::guest_output_ffx_fsr_rcas_dither_ps, fsr1_shaders::guest_output_bilinear_dither_ps};
    const size_t sizes[]{sizeof(fsr1_shaders::guest_output_ffx_fsr_easu_ps),
        sizeof(fsr1_shaders::guest_output_ffx_fsr_rcas_ps), sizeof(fsr1_shaders::guest_output_bilinear_ps),
        sizeof(fsr1_shaders::guest_output_ffx_fsr_rcas_dither_ps), sizeof(fsr1_shaders::guest_output_bilinear_dither_ps)};
    for (uint32_t i = 0; i < count; ++i) {
      auto& pass = passes[i];
      const auto format = i + 1 == count ? output.Format : DXGI_FORMAT_R10G10B10A2_UNORM;
      auto& pipeline = pipelines_[(uint32_t(format) << 3) | pass.kind];
      if (!pipeline) pipeline = Pipeline(programs[pass.kind], sizes[pass.kind], format);
      if (!pipeline) return false;
      pass.pipeline = pipeline.Get();
    }
    auto& frame = slots_[slot];
    if (!frame.srv) {
      D3D12_DESCRIPTOR_HEAP_DESC heap{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 6,
          D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
      if (FAILED(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&frame.srv)))) return false;
    }
    if (!frame.rtv) {
      D3D12_DESCRIPTOR_HEAP_DESC heap{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 6};
      if (FAILED(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&frame.rtv)))) return false;
    }
    for (uint32_t i = 0; i + 1 < count; ++i) {
      auto& old = frame.intermediate[i];
      if (old && old->GetDesc().Width == passes[i].width && old->GetDesc().Height == passes[i].height) continue;
      D3D12_RESOURCE_DESC texture{};
      texture.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
      texture.Width = passes[i].width; texture.Height = passes[i].height;
      texture.DepthOrArraySize = texture.MipLevels = 1; texture.SampleDesc.Count = 1;
      texture.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
      texture.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
      D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_DEFAULT;
      ComPtr<ID3D12Resource> intermediate;
      if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &texture,
          D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&intermediate)))) return false;
      old = std::move(intermediate);
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC view{};
    view.Format = input.Format;
    if (view.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS) view.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    if (view.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS) view.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    view.Texture2D.MipLevels = 1;
    auto cpu = frame.srv->GetCPUDescriptorHandleForHeapStart();
    device->CreateShaderResourceView(source, &view, cpu);
    auto rtv = frame.rtv->GetCPUDescriptorHandleForHeapStart();
    for (uint32_t i = 0; i < count; ++i) {
      auto target = i + 1 == count ? destination : frame.intermediate[i].Get();
      device->CreateRenderTargetView(target, nullptr, rtv); rtv.ptr += rtv_stride_;
      if (i + 1 < count) {
        cpu.ptr += srv_stride_; view.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
        device->CreateShaderResourceView(target, &view, cpu);
      }
    }
    ID3D12DescriptorHeap* heaps[]{frame.srv.Get()}; commands->SetDescriptorHeaps(1, heaps);
    commands->SetGraphicsRootSignature(root_.Get());
    commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    const float rectangle[]{-1, 1, 2, -2};
    commands->SetGraphicsRoot32BitConstants(1, 4, rectangle, 0);
    auto gpu = frame.srv->GetGPUDescriptorHandleForHeapStart();
    rtv = frame.rtv->GetCPUDescriptorHandleForHeapStart();
    width = uint32_t(input.Width); height = input.Height;
    for (uint32_t i = 0; i < count; ++i, gpu.ptr += srv_stride_, rtv.ptr += rtv_stride_) {
      const auto& pass = passes[i]; const bool final = i + 1 == count;
      D3D12_RESOURCE_BARRIER barrier{};
      barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
      if (!final) {
        barrier.Transition = {frame.intermediate[i].Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET};
        commands->ResourceBarrier(1, &barrier);
      }
      const int32_t x = final ? int32_t(rect.x) : 0, y = final ? int32_t(rect.y) : 0;
      D3D12_VIEWPORT viewport{float(x), float(y), float(pass.width), float(pass.height), 0, 1};
      D3D12_RECT scissor{x, y, x + LONG(pass.width), y + LONG(pass.height)};
      commands->RSSetViewports(1, &viewport); commands->RSSetScissorRects(1, &scissor);
      commands->SetGraphicsRootDescriptorTable(0, gpu);
      commands->OMSetRenderTargets(1, &rtv, FALSE, nullptr); commands->SetPipelineState(pass.pipeline);
      uint32_t constants[4]{};
      const auto bits = [](float f) { return std::bit_cast<uint32_t>(f); };
      if (pass.kind == 0) {
        constants[0] = bits(float(width) / pass.width); constants[1] = bits(float(height) / pass.height);
        constants[2] = bits(1.0f / width); constants[3] = bits(1.0f / height);
      } else {
        constants[0] = uint32_t(x); constants[1] = uint32_t(y);
        constants[2] = bits(pass.kind == 1 || pass.kind == 3 ? std::exp2(-sharpness_reduction) : 1.0f / pass.width);
        constants[3] = bits(1.0f / pass.height);
      }
      commands->SetGraphicsRoot32BitConstants(2, 4, constants, 0); commands->DrawInstanced(4, 1, 0, 0);
      if (!final) {
        std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
        commands->ResourceBarrier(1, &barrier);
      }
      width = pass.width; height = pass.height;
    }
    return true;
  }
};
}  // namespace legodimensions::gpu_native
