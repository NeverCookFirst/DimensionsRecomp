#pragma once
#include "display_gamma_lut.h"
#include <d3d12.h>
#include <wrl/client.h>
#include <cstring>

namespace legodimensions::gpu_native::gamma_shaders {
// Use the reference renderer's exact table lookup program. Its t0 is an
// R10G10B10A2 buffer in Xenos BGR order, t1 the already resolved scene.
#include "../../../rexglue-sdk/src/graphics/shaders/bytecode/d3d12_5_1/apply_gamma_table_cs.h"
}
namespace legodimensions::gpu_native {
class DisplayGammaGpu {
  template<class T> using ComPtr=Microsoft::WRL::ComPtr<T>;
  struct Slot {ComPtr<ID3D12Resource> ramp;ComPtr<ID3D12DescriptorHeap> heap;};
  std::array<Slot,12> slots_;
  ComPtr<ID3D12Device> device_;
  ComPtr<ID3D12RootSignature> root_;
  ComPtr<ID3D12PipelineState> pipeline_;
  UINT stride_=0;
  bool Initialize(ID3D12Device* device) {
    if(pipeline_)return device_.Get()==device;
    device_=device;
    D3D12_DESCRIPTOR_RANGE ranges[2]{{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,2,0,0,0},
                                   {D3D12_DESCRIPTOR_RANGE_TYPE_UAV,1,0,0,2}};
    D3D12_ROOT_PARAMETER parameters[2]{};
    parameters[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[0].DescriptorTable={2,ranges};
    parameters[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[1].Constants={0,0,2};
    D3D12_ROOT_SIGNATURE_DESC desc{2,parameters,0,nullptr,D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ComPtr<ID3DBlob> blob,error;
    if(FAILED(D3D12SerializeRootSignature(&desc,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&error))||
       FAILED(device->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&root_))))return false;
    D3D12_COMPUTE_PIPELINE_STATE_DESC p{};p.pRootSignature=root_.Get();
    p.CS={gamma_shaders::apply_gamma_table_cs,sizeof(gamma_shaders::apply_gamma_table_cs)};
    if(FAILED(device->CreateComputePipelineState(&p,IID_PPV_ARGS(&pipeline_))))return false;
    stride_=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    return true;
  }
 public:
  // The caller owns input/output lifetime and state transitions and has
  // waited for this command slot's fence before reusing its upload/SRVs.
  // Input: NON_PIXEL_SHADER_RESOURCE; output: UNORDERED_ACCESS. No barriers
  // or commands are emitted on a validation/allocation failure.
  bool Apply(ID3D12Device* device,ID3D12GraphicsCommandList* commands,
      ID3D12Resource* input,ID3D12Resource* output,uint32_t slot,const DisplayGammaTable& table) {
    if(!device||!commands||!input||!output||input==output||slot>=slots_.size())return false;
    const auto in=input->GetDesc(),out=output->GetDesc();
    if(in.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D||out.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D||
       in.Width!=out.Width||in.Height!=out.Height||in.DepthOrArraySize!=1||out.DepthOrArraySize!=1||
       in.SampleDesc.Count!=1||out.SampleDesc.Count!=1||in.MipLevels!=1||out.MipLevels!=1||
       (in.Format!=DXGI_FORMAT_R8G8B8A8_UNORM&&in.Format!=DXGI_FORMAT_B8G8R8A8_UNORM)||
       out.Format!=DXGI_FORMAT_R8G8B8A8_UNORM||!(out.Flags&D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)||
       !Initialize(device))return false;
    auto& s=slots_[slot];
    if(!s.ramp) {
      D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_UPLOAD;
      D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;d.Width=1024;
      d.Height=d.DepthOrArraySize=d.MipLevels=1;d.SampleDesc.Count=1;d.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
      if(FAILED(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&d,D3D12_RESOURCE_STATE_GENERIC_READ,
          nullptr,IID_PPV_ARGS(&s.ramp))))return false;
    }
    if(!s.heap) {
      D3D12_DESCRIPTOR_HEAP_DESC d{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,3,D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,0};
      if(FAILED(device->CreateDescriptorHeap(&d,IID_PPV_ARGS(&s.heap))))return false;
    }
    void* mapped=nullptr;const D3D12_RANGE no_read{0,0};
    if(FAILED(s.ramp->Map(0,&no_read,&mapped)))return false;
    std::memcpy(mapped,table.words.data(),1024);const D3D12_RANGE written{0,1024};s.ramp->Unmap(0,&written);
    auto cpu=s.heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_SHADER_RESOURCE_VIEW_DESC ramp{};ramp.Format=DXGI_FORMAT_R10G10B10A2_UNORM;
    ramp.ViewDimension=D3D12_SRV_DIMENSION_BUFFER;ramp.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    ramp.Buffer.NumElements=256;device->CreateShaderResourceView(s.ramp.Get(),&ramp,cpu);cpu.ptr+=stride_;
    D3D12_SHADER_RESOURCE_VIEW_DESC scene{};scene.Format=in.Format;scene.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
    scene.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;scene.Texture2D.MipLevels=1;
    device->CreateShaderResourceView(input,&scene,cpu);cpu.ptr+=stride_;
    D3D12_UNORDERED_ACCESS_VIEW_DESC target{};target.Format=out.Format;target.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;
    device->CreateUnorderedAccessView(output,nullptr,&target,cpu);
    ID3D12DescriptorHeap* heaps[]{s.heap.Get()};commands->SetDescriptorHeaps(1,heaps);
    commands->SetComputeRootSignature(root_.Get());commands->SetPipelineState(pipeline_.Get());
    commands->SetComputeRootDescriptorTable(0,s.heap->GetGPUDescriptorHandleForHeapStart());
    const uint32_t size[]{uint32_t(in.Width),in.Height};commands->SetComputeRoot32BitConstants(1,2,size,0);
    commands->Dispatch((size[0]+15)/16,(size[1]+7)/8,1);
    return true;
  }
};
} // namespace legodimensions::gpu_native
