// Windowless D3D12 test: actual IA conversion into float/uint VS signatures.
#define NOMINMAX
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <array>
#include <vector>
#include <fstream>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include "gpu_native/float_blend_indices.h"
using Microsoft::WRL::ComPtr;
using namespace legodimensions::gpu_native;
void checkAt(HRESULT h,int line) { if (FAILED(h)) { std::printf("HRESULT %08X line %d\n",unsigned(h),line); std::exit(2); } }
#define check(x) checkAt((x),__LINE__)
std::vector<char> read(const char* path) {
 std::ifstream f(path,std::ios::binary|std::ios::ate); if(!f)std::exit(3);
 std::vector<char>b(size_t(f.tellg())); f.seekg(0); f.read(b.data(),b.size()); return b;
}
int main(int argc,char** argv) {
 if(argc!=4)return 1;
 const auto floatVS=read(argv[1]),uintVS=read(argv[2]),ps=read(argv[3]);
 ComPtr<ID3D12Device>d;check(D3D12CreateDevice(nullptr,D3D_FEATURE_LEVEL_12_0,IID_PPV_ARGS(&d)));
 ComPtr<ID3D12CommandQueue>q;D3D12_COMMAND_QUEUE_DESC qd{};check(d->CreateCommandQueue(&qd,IID_PPV_ARGS(&q)));
 ComPtr<ID3D12CommandAllocator>a;check(d->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&a)));
 ComPtr<ID3D12GraphicsCommandList>l;check(d->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,a.Get(),nullptr,IID_PPV_ARGS(&l)));check(l->Close());
 D3D12_ROOT_SIGNATURE_DESC rd{};rd.Flags=D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
 ComPtr<ID3DBlob>blob,err;check(D3D12SerializeRootSignature(&rd,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&err));
 ComPtr<ID3D12RootSignature>root;check(d->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&root)));
 auto buffer=[&](UINT64 size,D3D12_HEAP_TYPE type,D3D12_RESOURCE_STATES state){
  D3D12_HEAP_PROPERTIES hp{};hp.Type=type;D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;
  desc.Width=size;desc.Height=1;desc.DepthOrArraySize=desc.MipLevels=1;desc.SampleDesc.Count=1;desc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  ComPtr<ID3D12Resource>r;check(d->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&desc,state,nullptr,IID_PPV_ARGS(&r)));return r;};
 const std::array<uint8_t,12>values={64,128,192,255,64,128,192,255,64,128,192,255};
 auto vb=buffer(values.size(),D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
 void*m=nullptr;check(vb->Map(0,nullptr,&m));std::memcpy(m,values.data(),values.size());vb->Unmap(0,nullptr);
 D3D12_VERTEX_BUFFER_VIEW view{vb->GetGPUVirtualAddress(),UINT(values.size()),4};
 D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_DEFAULT;D3D12_RESOURCE_DESC td{};
 td.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;td.Width=td.Height=4;td.DepthOrArraySize=td.MipLevels=1;
 td.SampleDesc.Count=1;td.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;td.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
 ComPtr<ID3D12Resource>rt;check(d->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&td,D3D12_RESOURCE_STATE_RENDER_TARGET,nullptr,IID_PPV_ARGS(&rt)));
 D3D12_DESCRIPTOR_HEAP_DESC hd{};hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;hd.NumDescriptors=1;
 ComPtr<ID3D12DescriptorHeap>heap;check(d->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&heap)));
 auto handle=heap->GetCPUDescriptorHandleForHeapStart();d->CreateRenderTargetView(rt.Get(),nullptr,handle);
 UINT64 bytes;D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp;d->GetCopyableFootprints(&td,0,1,0,&fp,nullptr,nullptr,&bytes);
 auto rb=buffer(bytes,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
 ComPtr<ID3D12Fence>f;check(d->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&f)));
 HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);if(!event)return 4;UINT64 serial=0;
 for(bool integer:{false,true})for(bool negative:{false,true}) {
  plume::RenderInputElement input;input.semanticName="BLENDINDICES";input.semanticIndex=0;
  input.format=integer?plume::RenderFormat::R8G8B8A8_UINT:plume::RenderFormat::R8G8B8A8_UNORM;
  bool useFloat=FloatBlendIndicesInput({&input,1});if(negative)useFloat=!useFloat;
  const auto&vs=useFloat?floatVS:uintVS;
  D3D12_INPUT_ELEMENT_DESC ie{"BLENDINDICES",0,integer?DXGI_FORMAT_R8G8B8A8_UINT:DXGI_FORMAT_R8G8B8A8_UNORM,0,0,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0};
  D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};pd.pRootSignature=root.Get();pd.VS={vs.data(),vs.size()};pd.PS={ps.data(),ps.size()};
  pd.InputLayout={&ie,1};pd.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;pd.NumRenderTargets=1;
  pd.RTVFormats[0]=td.Format;pd.SampleDesc.Count=1;pd.SampleMask=UINT_MAX;
  pd.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID;pd.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;
  pd.RasterizerState.DepthClipEnable=TRUE;pd.DepthStencilState.DepthFunc=D3D12_COMPARISON_FUNC_ALWAYS;
  pd.BlendState.RenderTarget[0].SrcBlend=pd.BlendState.RenderTarget[0].SrcBlendAlpha=D3D12_BLEND_ONE;
  pd.BlendState.RenderTarget[0].DestBlend=pd.BlendState.RenderTarget[0].DestBlendAlpha=D3D12_BLEND_ZERO;
  pd.BlendState.RenderTarget[0].BlendOp=pd.BlendState.RenderTarget[0].BlendOpAlpha=D3D12_BLEND_OP_ADD;
  pd.BlendState.RenderTarget[0].RenderTargetWriteMask=15;
  ComPtr<ID3D12PipelineState>pso;check(d->CreateGraphicsPipelineState(&pd,IID_PPV_ARGS(&pso)));
  check(a->Reset());check(l->Reset(a.Get(),pso.Get()));l->SetGraphicsRootSignature(root.Get());
  l->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);l->IASetVertexBuffers(0,1,&view);
  D3D12_VIEWPORT vp{0,0,4,4,0,1};D3D12_RECT sc{0,0,4,4};l->RSSetViewports(1,&vp);l->RSSetScissorRects(1,&sc);
  l->OMSetRenderTargets(1,&handle,FALSE,nullptr);const float clear[4]={};l->ClearRenderTargetView(handle,clear,0,nullptr);l->DrawInstanced(3,1,0,0);
  D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;b.Transition={rt.Get(),0,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_SOURCE};l->ResourceBarrier(1,&b);
  D3D12_TEXTURE_COPY_LOCATION dst{};dst.pResource=rb.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;dst.PlacedFootprint=fp;
  D3D12_TEXTURE_COPY_LOCATION src{};src.pResource=rt.Get();src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;l->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
  std::swap(b.Transition.StateBefore,b.Transition.StateAfter);l->ResourceBarrier(1,&b);check(l->Close());ID3D12CommandList*lists[]={l.Get()};q->ExecuteCommandLists(1,lists);
  check(q->Signal(f.Get(),++serial));check(f->SetEventOnCompletion(serial,event));if(WaitForSingleObject(event,30000)!=WAIT_OBJECT_0)return 5;
  check(rb->Map(0,nullptr,&m));const float*pixel=reinterpret_cast<float*>(static_cast<char*>(m)+fp.Offset+fp.Footprint.RowPitch+16);
  bool matches=true;for(int i=0;i<4;++i){const float expected=integer?values[i]:values[i]/255.0f;matches&=std::isfinite(pixel[i])&&std::abs(pixel[i]-expected)<0.000001f;}
  std::printf("%s %s: %.8g %.8g %.8g %.8g\n",integer?"UINT":"UNORM",negative?"negative control":"production selection",pixel[0],pixel[1],pixel[2],pixel[3]);
  rb->Unmap(0,nullptr);if(matches==negative)return 6;
 }
 CloseHandle(event);std::puts("PASS: normalized material values and integer indices; both wrong-signature controls rejected");return 0;
}
