#define NOMINMAX
#include "gpu_native/native_overlay.h"
#include "gpu_native/overlay_packet.h"
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <wrl/client.h>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <fstream>
using Microsoft::WRL::ComPtr;
using namespace legodimensions::gpu_native;
std::shared_ptr<OverlayPacket> packet;
namespace legodimensions::gpu_native {
std::shared_ptr<const OverlayPacket> NativeOverlayPacket(){return packet;}
void SetNativeOverlayDeviceReady(bool){}
}
int main(int argc,char**argv){
  ComPtr<ID3D12Debug> debug;
  if(SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))debug->EnableDebugLayer();
  ComPtr<ID3D12Device> device;assert(SUCCEEDED(D3D12CreateDevice(nullptr,D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device))));
  ComPtr<ID3D12InfoQueue> info;device.As(&info);
  D3D12_COMMAND_QUEUE_DESC qd{};ComPtr<ID3D12CommandQueue> queue;assert(SUCCEEDED(device->CreateCommandQueue(&qd,IID_PPV_ARGS(&queue))));
  ComPtr<ID3D12CommandAllocator> allocator;assert(SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator))));
  ComPtr<ID3D12GraphicsCommandList> commands;assert(SUCCEEDED(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&commands))));commands->Close();
  ComPtr<ID3D12Fence> fence;device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence));
  HANDLE event=CreateEvent(nullptr,FALSE,FALSE,nullptr);
  D3D12_RESOURCE_DESC td{};td.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;td.Width=td.Height=128;
  td.DepthOrArraySize=td.MipLevels=td.SampleDesc.Count=1;td.Format=DXGI_FORMAT_R8G8B8A8_UNORM;td.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
  D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;
  ComPtr<ID3D12Resource> target;assert(SUCCEEDED(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&td,D3D12_RESOURCE_STATE_RENDER_TARGET,nullptr,IID_PPV_ARGS(&target))));
  D3D12_DESCRIPTOR_HEAP_DESC hd{};hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;hd.NumDescriptors=1;
  ComPtr<ID3D12DescriptorHeap> rtv;device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&rtv));
  auto handle=rtv->GetCPUDescriptorHandleForHeapStart();device->CreateRenderTargetView(target.Get(),nullptr,handle);
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};UINT rows;UINT64 row_bytes,total;device->GetCopyableFootprints(&td,0,1,0,&fp,&rows,&row_bytes,&total);
  D3D12_RESOURCE_DESC bd{};bd.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;bd.Width=total;bd.Height=bd.DepthOrArraySize=bd.MipLevels=bd.SampleDesc.Count=1;bd.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  heap.Type=D3D12_HEAP_TYPE_READBACK;ComPtr<ID3D12Resource> readback;
  assert(SUCCEEDED(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&bd,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&readback))));
  auto image=std::make_shared<OverlayTexture>();image->width=image->height=1;image->rgba={255,0,0,128};
  packet=std::make_shared<OverlayPacket>();packet->width=packet->height=128;packet->logical_width=packet->logical_height=64;
  packet->vertices={{8,8,0,0,~0u},{24,8,1,0,~0u},{24,24,1,1,~0u},{8,24,0,1,~0u}};
  packet->indices={0,1,2,0,2,3};OverlayDraw draw;draw.texture=image;draw.count=6;draw.scissor=true;draw.clip={12,8,24,24};packet->draws={draw};
  for(uint64_t frame=1;frame<=8;++frame){
    allocator->Reset();commands->Reset(allocator.Get(),nullptr);
    if(frame>1){D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;b.Transition={target.Get(),0,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_RENDER_TARGET};commands->ResourceBarrier(1,&b);}
    commands->OMSetRenderTargets(1,&handle,FALSE,nullptr);const float clear[]{.1f,.1f,.1f,1};commands->ClearRenderTargetView(handle,clear,0,nullptr);
    RenderNativeOverlays(device.Get(),commands.Get(),td.Format,0,frame,128,128);
    D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;b.Transition={target.Get(),0,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_SOURCE};commands->ResourceBarrier(1,&b);
    D3D12_TEXTURE_COPY_LOCATION src{},dst{};src.pResource=target.Get();src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.pResource=readback.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;dst.PlacedFootprint=fp;commands->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
    commands->Close();ID3D12CommandList* lists[]{commands.Get()};queue->ExecuteCommandLists(1,lists);queue->Signal(fence.Get(),frame);fence->SetEventOnCompletion(frame,event);WaitForSingleObject(event,INFINITE);
    void* mapped;D3D12_RANGE range{0,size_t(total)};assert(SUCCEEDED(readback->Map(0,&range,&mapped)));
    auto pixel=[&](unsigned x,unsigned y,unsigned c){return static_cast<uint8_t*>(mapped)[fp.Offset+uint64_t(y)*fp.Footprint.RowPitch+4*x+c]/255.f;};
    assert(std::abs(pixel(30,24,0)-.55196f)<.012f);assert(std::abs(pixel(30,24,1)-.0498f)<.012f);
    assert(std::abs(pixel(20,24,0)-.1f)<.006f);assert(std::abs(pixel(60,60,0)-.1f)<.006f);
    if(argc>1&&frame==8){std::ofstream f(argv[1],std::ios::binary);for(unsigned y=0;y<128;++y)f.write(static_cast<char*>(mapped)+fp.Offset+y*fp.Footprint.RowPitch,128*4);}
    readback->Unmap(0,nullptr);
  }
  if(info)for(UINT64 i=0;i<info->GetNumStoredMessagesAllowedByRetrievalFilter();++i){SIZE_T size=0;info->GetMessage(i,nullptr,&size);std::vector<uint8_t> data(size);auto*m=reinterpret_cast<D3D12_MESSAGE*>(data.data());info->GetMessage(i,m,&size);
    if(m->Severity<=D3D12_MESSAGE_SEVERITY_ERROR){std::fprintf(stderr,"D3D12 error %u: %s\n",m->ID,m->pDescription);return 24;}}
  ResetNativeOverlayGpu();CloseHandle(event);
  std::puts("PASS real native overlay GPU: DPI scaling, clipping, RGBA font alpha, repeated fence-slot reuse; no window or game");
}
