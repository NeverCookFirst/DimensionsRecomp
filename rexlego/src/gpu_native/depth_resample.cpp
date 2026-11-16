#include "gpu_native/depth_resample.h"
#include "gpu_native/pipeline_library.h"
#include <d3dcompiler.h>

namespace legodimensions::gpu_native {
namespace {
struct Bindings {
  Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> resources, depth;
  Microsoft::WRL::ComPtr<ID3D12Resource> source, destination;
  Microsoft::WRL::ComPtr<ID3D12RootSignature> root;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> pipelines[9];
};
bool Supported(const D3D12_RESOURCE_DESC& d) {
  return d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
      d.Format == DXGI_FORMAT_R32G8X24_TYPELESS && d.Width && d.Height &&
      d.Width <= 16384 && d.Height <= 16384 && d.MipLevels == 1 &&
      d.DepthOrArraySize == 1 && d.SampleDesc.Count == 1 &&
      !(d.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE);
}
}
std::unique_ptr<DepthResampler> DepthResampler::Create(ID3D12Device* device,
    std::span<const unsigned char> vertex, std::span<const unsigned char> combined,
    std::span<const unsigned char> depth, std::span<const unsigned char> stencil,
    bool force_portable) {
  if (!device || vertex.empty() || combined.empty() || depth.empty() || stencil.empty()) return {};
  auto result = std::unique_ptr<DepthResampler>(new DepthResampler);
  result->device_ = device;
  D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
  result->pixel_stencil_reference_ = !force_portable && SUCCEEDED(device->CheckFeatureSupport(
      D3D12_FEATURE_D3D12_OPTIONS,&options,sizeof(options))) && options.PSSpecifiedStencilRefSupported;
  D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,2,0,0,0};
  D3D12_ROOT_PARAMETER parameters[2]{};
  parameters[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  parameters[0].DescriptorTable={1,&range};
  parameters[0].ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;
  parameters[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  parameters[1].Constants={0,0,5};
  parameters[1].ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;
  D3D12_ROOT_SIGNATURE_DESC root{};root.NumParameters=2;root.pParameters=parameters;
  Microsoft::WRL::ComPtr<ID3DBlob> blob,error;
  if (FAILED(D3D12SerializeRootSignature(&root,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&error)) ||
      FAILED(device->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),
          IID_PPV_ARGS(&result->root_)))) return {};
  const auto pipeline = [&](std::span<const unsigned char> pixel,bool write_depth,
                            unsigned stencil_mask,auto& output) {
    D3D12_GRAPHICS_PIPELINE_STATE_DESC p{};
    p.pRootSignature=result->root_.Get();p.VS={vertex.data(),vertex.size()};
    p.PS={pixel.data(),pixel.size()};
    p.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID;
    p.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;p.RasterizerState.DepthClipEnable=TRUE;
    p.DepthStencilState.DepthEnable=write_depth;
    p.DepthStencilState.DepthWriteMask=write_depth?D3D12_DEPTH_WRITE_MASK_ALL:D3D12_DEPTH_WRITE_MASK_ZERO;
    p.DepthStencilState.DepthFunc=D3D12_COMPARISON_FUNC_ALWAYS;
    p.DepthStencilState.StencilEnable=stencil_mask!=0;
    p.DepthStencilState.StencilReadMask=0xFF;
    p.DepthStencilState.StencilWriteMask=static_cast<UINT8>(stencil_mask);
    p.DepthStencilState.FrontFace={D3D12_STENCIL_OP_KEEP,D3D12_STENCIL_OP_KEEP,
        D3D12_STENCIL_OP_REPLACE,D3D12_COMPARISON_FUNC_ALWAYS};
    p.DepthStencilState.BackFace=p.DepthStencilState.FrontFace;
    p.SampleMask=UINT_MAX;p.SampleDesc.Count=1;
    p.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    p.DSVFormat=DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    return CreateAuxiliaryPipeline(device, p, blob->GetBufferPointer(),
                                   blob->GetBufferSize(), output.GetAddressOf());
  };
  if (result->pixel_stencil_reference_) {
    if (!pipeline(combined,true,0xFF,result->combined_)) return {};
  } else {
    if (!pipeline(depth,true,0,result->depth_)) return {};
    for (unsigned bit=0;bit<8;++bit)
      if (!pipeline(stencil,false,1u<<bit,result->stencil_[bit])) return {};
  }
  return result;
}
bool DepthResampler::Supports(ID3D12Resource* source,ID3D12Resource* destination) {
  if (!source || !destination || source==destination) return false;
  const auto src=source->GetDesc(),dst=destination->GetDesc();
  return Supported(src) && Supported(dst) &&
      (dst.Flags&D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
}
std::shared_ptr<void> DepthResampler::Record(ID3D12GraphicsCommandList* list,
    ID3D12Resource* source,ID3D12Resource* destination) {
  if (!list || !Supports(source,destination)) return {};
  const auto src=source->GetDesc(),dst=destination->GetDesc();
  auto bindings=std::make_shared<Bindings>();
  bindings->source=source;bindings->destination=destination;bindings->root=root_;
  bindings->pipelines[0]=pixel_stencil_reference_?combined_:depth_;
  if(!pixel_stencil_reference_)for(unsigned bit=0;bit<8;++bit)
    bindings->pipelines[bit+1]=stencil_[bit];
  D3D12_DESCRIPTOR_HEAP_DESC heap{};heap.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  heap.NumDescriptors=2;heap.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  if (FAILED(device_->CreateDescriptorHeap(&heap,IID_PPV_ARGS(&bindings->resources)))) return {};
  heap.Type=D3D12_DESCRIPTOR_HEAP_TYPE_DSV;heap.NumDescriptors=1;heap.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
  if (FAILED(device_->CreateDescriptorHeap(&heap,IID_PPV_ARGS(&bindings->depth)))) return {};
  auto handle=bindings->resources->GetCPUDescriptorHandleForHeapStart();
  D3D12_SHADER_RESOURCE_VIEW_DESC srv{};srv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
  srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  srv.Texture2D.MipLevels=1;srv.Format=DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
  device_->CreateShaderResourceView(source,&srv,handle);
  handle.ptr+=device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  srv.Format=DXGI_FORMAT_X32_TYPELESS_G8X24_UINT;srv.Texture2D.PlaneSlice=1;
  device_->CreateShaderResourceView(source,&srv,handle);
  const auto dsv=bindings->depth->GetCPUDescriptorHandleForHeapStart();
  D3D12_DEPTH_STENCIL_VIEW_DESC view{};view.Format=DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
  view.ViewDimension=D3D12_DSV_DIMENSION_TEXTURE2D;
  device_->CreateDepthStencilView(destination,&view,dsv);
  // Everything that can fail is prepared before modifying command state.
  ID3D12DescriptorHeap* heaps[]{bindings->resources.Get()};list->SetDescriptorHeaps(1,heaps);
  list->SetGraphicsRootSignature(root_.Get());
  list->SetGraphicsRootDescriptorTable(0,bindings->resources->GetGPUDescriptorHandleForHeapStart());
  UINT constants[]{UINT(src.Width),src.Height,UINT(dst.Width),dst.Height,0};
  list->SetGraphicsRoot32BitConstants(1,5,constants,0);
  const D3D12_VIEWPORT viewport{0,0,float(dst.Width),float(dst.Height),0,1};
  const D3D12_RECT scissor{0,0,LONG(dst.Width),LONG(dst.Height)};
  list->RSSetViewports(1,&viewport);list->RSSetScissorRects(1,&scissor);
  list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  list->OMSetRenderTargets(0,nullptr,FALSE,&dsv);
  if (pixel_stencil_reference_) {
    list->SetPipelineState(combined_.Get());list->DrawInstanced(3,1,0,0);
  } else {
    list->SetPipelineState(depth_.Get());list->DrawInstanced(3,1,0,0);
    list->ClearDepthStencilView(dsv,D3D12_CLEAR_FLAG_STENCIL,0,0,0,nullptr);
    list->OMSetStencilRef(0xFF);
    for (unsigned bit=0;bit<8;++bit) {
      list->SetGraphicsRoot32BitConstant(1,1u<<bit,4);
      list->SetPipelineState(stencil_[bit].Get());list->DrawInstanced(3,1,0,0);
    }
  }
  list->OMSetRenderTargets(0,nullptr,FALSE,nullptr);
  return bindings;
}
}
