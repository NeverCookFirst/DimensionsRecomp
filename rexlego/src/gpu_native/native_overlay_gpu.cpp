#include "gpu_native/native_overlay.h"
#include "gpu_native/overlay_packet.h"
#include "gpu_native/pipeline_library.h"
#include "native_overlay_vs.h"
#include "native_overlay_ps.h"
#include <d3d12.h>
#include <wrl/client.h>
#include <array>
#include <cstring>
#include <limits>
#include <unordered_map>
#include <rex/logging.h>

namespace legodimensions::gpu_native {
namespace {
using Microsoft::WRL::ComPtr;
struct Upload {
  ComPtr<ID3D12Resource> resource;
  uint8_t* mapped=nullptr;
  uint64_t capacity=0;
  ~Upload(){if(resource&&mapped)resource->Unmap(0,nullptr);}
};
struct Slot {
  uint64_t serial=0,used=0;
  std::shared_ptr<Upload> upload;
  std::vector<std::shared_ptr<Upload>> retired;
};
struct Image {
  std::shared_ptr<const OverlayTexture> pixels;
  ComPtr<ID3D12Resource> texture,upload;
  uint32_t descriptor=0;
};
struct Renderer {
  ComPtr<ID3D12RootSignature> root;
  ComPtr<ID3DBlob> root_data;
  ComPtr<ID3D12DescriptorHeap> heap;
  std::unordered_map<uint32_t,ComPtr<ID3D12PipelineState>> pipelines;
  std::unordered_map<const OverlayTexture*,Image> images;
  std::array<Slot,12> slots;
  uint32_t increment=0;
  uint64_t texture_bytes=0;
};
Renderer renderer;
constexpr uint32_t kMaxImages=64;
ComPtr<ID3D12Resource> Resource(ID3D12Device* device,D3D12_HEAP_TYPE type,
    const D3D12_RESOURCE_DESC& desc,D3D12_RESOURCE_STATES state){
  D3D12_HEAP_PROPERTIES heap{};heap.Type=type;
  ComPtr<ID3D12Resource> p;
  device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,state,nullptr,IID_PPV_ARGS(&p));return p;
}
D3D12_RESOURCE_DESC BufferDesc(uint64_t bytes){
  D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;d.Width=bytes;
  d.Height=d.DepthOrArraySize=d.MipLevels=d.SampleDesc.Count=1;d.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;return d;
}
bool Initialize(ID3D12Device* device){
  if(renderer.root&&renderer.heap)return true;
  D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,1,0,0,0};
  D3D12_ROOT_PARAMETER params[2]{};
  params[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;params[0].Constants={0,0,5};
  params[0].ShaderVisibility=D3D12_SHADER_VISIBILITY_ALL;
  params[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  params[1].DescriptorTable={1,&range};params[1].ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;
  D3D12_STATIC_SAMPLER_DESC samplers[4]{};
  for(uint32_t i=0;i<4;++i){
    auto& s=samplers[i];s.Filter=i%2?D3D12_FILTER_MIN_MAG_MIP_POINT:D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    s.AddressU=s.AddressV=s.AddressW=i>=2?D3D12_TEXTURE_ADDRESS_MODE_WRAP:D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    s.ShaderRegister=i;s.ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;
    s.ComparisonFunc=D3D12_COMPARISON_FUNC_ALWAYS;s.MaxLOD=std::numeric_limits<float>::max();s.MaxAnisotropy=1;
  }
  D3D12_ROOT_SIGNATURE_DESC desc{2,params,4,samplers,D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT};
  ComPtr<ID3DBlob> blob,error;
  if(FAILED(D3D12SerializeRootSignature(&desc,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&error)))return false;
  ComPtr<ID3D12RootSignature> root;
  if(FAILED(device->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&root))))return false;
  D3D12_DESCRIPTOR_HEAP_DESC hd{};hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  hd.NumDescriptors=kMaxImages;hd.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  ComPtr<ID3D12DescriptorHeap> heap;
  if(FAILED(device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&heap))))return false;
  renderer.root=std::move(root);renderer.heap=std::move(heap);
  renderer.root_data = std::move(blob);
  renderer.increment=device->GetDescriptorHandleIncrementSize(hd.Type);return true;
}
ID3D12PipelineState* Pipeline(ID3D12Device* device,DXGI_FORMAT format,bool lines){
  const uint32_t key=uint32_t(format)|(lines?0x10000:0);
  if(auto found=renderer.pipelines.find(key);found!=renderer.pipelines.end())return found->second.Get();
  D3D12_GRAPHICS_PIPELINE_STATE_DESC p{};p.pRootSignature=renderer.root.Get();
  p.VS={g_native_overlay_vs,sizeof(g_native_overlay_vs)};p.PS={g_native_overlay_ps,sizeof(g_native_overlay_ps)};
  const D3D12_INPUT_ELEMENT_DESC elements[]{
      {"POSITION",0,DXGI_FORMAT_R32G32_FLOAT,0,0,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
      {"TEXCOORD",0,DXGI_FORMAT_R32G32_FLOAT,0,8,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
      {"COLOR",0,DXGI_FORMAT_R8G8B8A8_UNORM,0,16,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0}};
  p.InputLayout={elements,3};p.SampleMask=UINT_MAX;p.SampleDesc.Count=1;
  p.PrimitiveTopologyType=lines?D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE:D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  p.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID;p.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;
  p.RasterizerState.DepthClipEnable=TRUE;
  p.DepthStencilState.DepthFunc=D3D12_COMPARISON_FUNC_ALWAYS;
  p.DepthStencilState.FrontFace=p.DepthStencilState.BackFace={D3D12_STENCIL_OP_KEEP,D3D12_STENCIL_OP_KEEP,D3D12_STENCIL_OP_KEEP,D3D12_COMPARISON_FUNC_ALWAYS};
  auto& blend=p.BlendState.RenderTarget[0];blend.BlendEnable=TRUE;
  blend.SrcBlend=D3D12_BLEND_SRC_ALPHA;blend.DestBlend=D3D12_BLEND_INV_SRC_ALPHA;blend.BlendOp=D3D12_BLEND_OP_ADD;
  blend.SrcBlendAlpha=D3D12_BLEND_ONE;blend.DestBlendAlpha=D3D12_BLEND_INV_SRC_ALPHA;blend.BlendOpAlpha=D3D12_BLEND_OP_ADD;
  blend.RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_ALL;p.NumRenderTargets=1;p.RTVFormats[0]=format;
  ComPtr<ID3D12PipelineState> result;
  if (!CreateAuxiliaryPipeline(device, p, renderer.root_data->GetBufferPointer(),
                               renderer.root_data->GetBufferSize(), result.GetAddressOf()))
    return nullptr;
  auto* value=result.Get();renderer.pipelines.emplace(key,std::move(result));return value;
}
Image* Texture(ID3D12Device* device,ID3D12GraphicsCommandList* commands,
    const std::shared_ptr<const OverlayTexture>& pixels){
  if(auto found=renderer.images.find(pixels.get());found!=renderer.images.end())return &found->second;
  if(!pixels||!pixels->width||!pixels->height||renderer.images.size()>=kMaxImages||
      pixels->rgba.size()!=uint64_t(pixels->width)*pixels->height*4||
      renderer.texture_bytes+pixels->rgba.size()>64*1024*1024)return nullptr;
  D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  d.Width=pixels->width;d.Height=pixels->height;d.DepthOrArraySize=d.MipLevels=d.SampleDesc.Count=1;d.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
  Image image;image.pixels=pixels;image.descriptor=uint32_t(renderer.images.size());
  image.texture=Resource(device,D3D12_HEAP_TYPE_DEFAULT,d,D3D12_RESOURCE_STATE_COPY_DEST);
  if(!image.texture)return nullptr;
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};UINT rows;UINT64 row_size,total;
  device->GetCopyableFootprints(&d,0,1,0,&fp,&rows,&row_size,&total);
  image.upload=Resource(device,D3D12_HEAP_TYPE_UPLOAD,BufferDesc(total),D3D12_RESOURCE_STATE_GENERIC_READ);
  if(!image.upload)return nullptr;
  void* mapped=nullptr;D3D12_RANGE read{0,0};if(FAILED(image.upload->Map(0,&read,&mapped)))return nullptr;
  for(uint32_t y=0;y<rows;++y)std::memcpy(static_cast<uint8_t*>(mapped)+fp.Offset+uint64_t(y)*fp.Footprint.RowPitch,
      pixels->rgba.data()+size_t(y)*pixels->width*4,pixels->width*4);
  image.upload->Unmap(0,nullptr);
  D3D12_TEXTURE_COPY_LOCATION dst{},src{};dst.pResource=image.texture.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  src.pResource=image.upload.Get();src.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;src.PlacedFootprint=fp;
  commands->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
  D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Transition={image.texture.Get(),0,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE};commands->ResourceBarrier(1,&barrier);
  D3D12_SHADER_RESOURCE_VIEW_DESC view{};view.Format=d.Format;view.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
  view.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;view.Texture2D.MipLevels=1;
  auto handle=renderer.heap->GetCPUDescriptorHandleForHeapStart();handle.ptr+=uint64_t(image.descriptor)*renderer.increment;
  device->CreateShaderResourceView(image.texture.Get(),&view,handle);
  renderer.texture_bytes+=pixels->rgba.size();auto [it,added]=renderer.images.emplace(pixels.get(),std::move(image));return &it->second;
}
}
unsigned PrepareNativeOverlayPipelines(ID3D12Device* device, uint32_t format) {
  if (!Initialize(device)) return 0;
  unsigned prepared = 0;
  for (bool lines : {false, true})
    prepared += Pipeline(device, static_cast<DXGI_FORMAT>(format), lines) != nullptr;
  return prepared;
}

void RenderNativeOverlays(ID3D12Device* device,ID3D12GraphicsCommandList* commands,
    uint32_t format,uint32_t slot,uint64_t serial,uint32_t w,uint32_t h){
  auto packet=NativeOverlayPacket();
  if(!device||!commands||!packet||packet->draws.empty()||slot>=renderer.slots.size()||!serial||
      packet->width!=w||packet->height!=h||!Initialize(device))return;
  if(!std::isfinite(packet->logical_width)||!std::isfinite(packet->logical_height)||
      packet->logical_width<=0||packet->logical_height<=0)return;
  const uint64_t vertex_bytes=packet->vertices.size()*sizeof(OverlayVertex);
  const uint64_t index_bytes=packet->indices.size()*sizeof(uint16_t);
  const uint64_t index_at=(vertex_bytes+255)&~uint64_t{255};
  const uint64_t bytes=(index_at+index_bytes+255)&~uint64_t{255};
  if(!vertex_bytes||!index_bytes||bytes>32*1024*1024)return;
  auto& arena=renderer.slots[slot];
  // HostDevice has waited for this ring slot before advancing its serial.
  if(arena.serial!=serial){arena.serial=serial;arena.used=0;arena.retired.clear();}
  if(!arena.upload||arena.used+bytes>arena.upload->capacity){
    auto upload=std::make_shared<Upload>();upload->capacity=std::max(uint64_t{256*1024},bytes);
    upload->resource=Resource(device,D3D12_HEAP_TYPE_UPLOAD,BufferDesc(upload->capacity),D3D12_RESOURCE_STATE_GENERIC_READ);
    D3D12_RANGE read{0,0};if(!upload->resource||FAILED(upload->resource->Map(0,&read,reinterpret_cast<void**>(&upload->mapped))))return;
    if(arena.upload)arena.retired.push_back(std::move(arena.upload));arena.upload=std::move(upload);arena.used=0;
  }
  const auto base=arena.used;arena.used+=bytes;
  std::memcpy(arena.upload->mapped+base,packet->vertices.data(),vertex_bytes);
  std::memcpy(arena.upload->mapped+base+index_at,packet->indices.data(),index_bytes);
  const auto address=arena.upload->resource->GetGPUVirtualAddress()+base;
  D3D12_VERTEX_BUFFER_VIEW vb{address,UINT(vertex_bytes),sizeof(OverlayVertex)};
  D3D12_INDEX_BUFFER_VIEW ib{address+index_at,UINT(index_bytes),DXGI_FORMAT_R16_UINT};
  commands->SetGraphicsRootSignature(renderer.root.Get());
  auto* heap=renderer.heap.Get();commands->SetDescriptorHeaps(1,&heap);
  commands->IASetVertexBuffers(0,1,&vb);commands->IASetIndexBuffer(&ib);
  const D3D12_VIEWPORT viewport{0,0,float(w),float(h),0,1};commands->RSSetViewports(1,&viewport);
  static const auto white=std::make_shared<OverlayTexture>(OverlayTexture{1,1,true,false,{255,255,255,255}});
  for(const auto& draw:packet->draws){
    if(!OverlayDrawRange(*packet,draw))continue;
    const auto clip=OverlayScissor(draw,*packet,w,h);if(clip[2]<=clip[0]||clip[3]<=clip[1])continue;
    auto* image=Texture(device,commands,draw.texture?draw.texture:white);
    auto* pipeline=Pipeline(device,DXGI_FORMAT(format),draw.lines);if(!image||!pipeline)continue;
    const D3D12_RECT rect{clip[0],clip[1],clip[2],clip[3]};commands->RSSetScissorRects(1,&rect);
    struct {float sx,sy,x,y;uint32_t filter;} transform{2/packet->logical_width,-2/packet->logical_height,-1,1,
        (image->pixels->linear?0u:1u)+(image->pixels->repeat?2u:0u)};
    commands->SetGraphicsRoot32BitConstants(0,5,&transform,0);
    auto gpu=renderer.heap->GetGPUDescriptorHandleForHeapStart();gpu.ptr+=uint64_t(image->descriptor)*renderer.increment;
    commands->SetGraphicsRootDescriptorTable(1,gpu);commands->SetPipelineState(pipeline);
    commands->IASetPrimitiveTopology(draw.lines?D3D_PRIMITIVE_TOPOLOGY_LINELIST:D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commands->DrawIndexedInstanced(draw.count,1,draw.first,draw.base,0);
  }
}
void ResetNativeOverlayGpu(){SetNativeOverlayDeviceReady(false);renderer={};}
} // namespace legodimensions::gpu_native
