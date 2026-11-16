// Windowless execution of the production recorder and both stencil strategies.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "gpu_native/depth_resample.h"
#include <dxgi1_6.h>
#include <d3d12sdklayers.h>
#include <array>
#include <vector>
#include <fstream>
#include <cstring>
#include <cstdio>
#include <cstdlib>
using Microsoft::WRL::ComPtr;
using namespace legodimensions::gpu_native;
static void hr(HRESULT h){if(FAILED(h)){printf("HRESULT=%08X\n",unsigned(h));std::exit(2);}}
static std::vector<unsigned char> read(const char* path){
  std::ifstream f(path,std::ios::binary|std::ios::ate);if(!f)std::exit(3);
  std::vector<unsigned char>b(size_t(f.tellg()));f.seekg(0);f.read(reinterpret_cast<char*>(b.data()),b.size());return b;
}
int main(int argc,char**argv){
  if(argc!=6 && argc!=7)return 1;setvbuf(stdout,nullptr,_IONBF,0);
  ComPtr<ID3D12Debug> debug;
  if(SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))debug->EnableDebugLayer();
  ComPtr<IDXGIFactory4> factory;hr(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
  ComPtr<IDXGIAdapter1> adapter;
  if(argc==7)hr(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)));
  else {
    ComPtr<IDXGIFactory6> preference;hr(factory.As(&preference));
    hr(preference->EnumAdapterByGpuPreference(0,DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,IID_PPV_ARGS(&adapter)));
  }
  DXGI_ADAPTER_DESC1 adapter_desc{};hr(adapter->GetDesc1(&adapter_desc));
  printf("ADAPTER %ls dedicated_bytes=%llu software=%u\n",adapter_desc.Description,
    static_cast<unsigned long long>(adapter_desc.DedicatedVideoMemory),bool(adapter_desc.Flags&DXGI_ADAPTER_FLAG_SOFTWARE));
  ComPtr<ID3D12Device> device;hr(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_12_0,IID_PPV_ARGS(&device)));
  ComPtr<ID3D12InfoQueue> info;device.As(&info);
  D3D12_COMMAND_QUEUE_DESC q{};ComPtr<ID3D12CommandQueue> queue;hr(device->CreateCommandQueue(&q,IID_PPV_ARGS(&queue)));
  ComPtr<ID3D12CommandAllocator> allocator;hr(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)));
  ComPtr<ID3D12GraphicsCommandList> list;hr(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&list)));hr(list->Close());
  ComPtr<ID3D12Fence> fence;hr(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)));
  HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);UINT64 serial=0;
  auto begin=[&]{hr(allocator->Reset());hr(list->Reset(allocator.Get(),nullptr));};
  auto submit=[&]{hr(list->Close());ID3D12CommandList* lists[]{list.Get()};queue->ExecuteCommandLists(1,lists);
    hr(queue->Signal(fence.Get(),++serial));hr(fence->SetEventOnCompletion(serial,event));
    if(WaitForSingleObject(event,30000)!=WAIT_OBJECT_0)std::exit(4);};
  auto transition=[&](ID3D12Resource*r,D3D12_RESOURCE_STATES before,D3D12_RESOURCE_STATES after){
    D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition={r,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,before,after};list->ResourceBarrier(1,&b);};
  auto buffer=[&](UINT64 size,D3D12_HEAP_TYPE type){
    D3D12_HEAP_PROPERTIES hp{};hp.Type=type;D3D12_RESOURCE_DESC d{};
    d.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;d.Width=size;d.Height=d.DepthOrArraySize=d.MipLevels=1;
    d.SampleDesc.Count=1;d.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;ComPtr<ID3D12Resource> r;
    hr(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&d,type==D3D12_HEAP_TYPE_READBACK?
      D3D12_RESOURCE_STATE_COPY_DEST:D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&r)));return r;};
  auto texture=[&](UINT w,UINT h,D3D12_RESOURCE_STATES state){
    D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_DEFAULT;D3D12_RESOURCE_DESC d{};
    d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;d.Width=w;d.Height=h;d.DepthOrArraySize=d.MipLevels=1;
    d.SampleDesc.Count=1;d.Format=DXGI_FORMAT_R32G8X24_TYPELESS;d.Flags=D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    ComPtr<ID3D12Resource>r;hr(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&d,state,nullptr,IID_PPV_ARGS(&r)));return r;};
  auto vs=read(argv[1]),both=read(argv[2]),depth=read(argv[3]),stencil=read(argv[4]),captured=read(argv[5]);
  if(captured.size()!=1129u*635u*8u)return 5;
  struct Shape{UINT sw,sh,dw,dh;bool captured;};
  const Shape cases[]{{63,37,97,53,false},{17,11,17,11,false},{640,360,1920,1080,false},
    {1129,635,1920,1080,false},{1280,720,1920,1080,false},{1920,1080,1129,635,false},
    {1129,635,1920,1080,true}};
  unsigned checked=0;
  for(bool portable:{false,true}){
    auto recorder=DepthResampler::Create(device.Get(),vs,both,depth,stencil,portable);if(!recorder)return 6;
    printf("MODE portable_requested=%u pixel_stencil_reference=%u\n",portable,recorder->UsesPixelStencilReference());
    for(const auto& c:cases){
      auto src=texture(c.sw,c.sh,D3D12_RESOURCE_STATE_COPY_DEST);
      auto dst=texture(c.dw,c.dh,D3D12_RESOURCE_STATE_DEPTH_WRITE);
      auto sd=src->GetDesc(),dd=dst->GetDesc();
      std::array<D3D12_PLACED_SUBRESOURCE_FOOTPRINT,2> in{},out{};
      UINT in_rows[2],out_rows[2];UINT64 in_row[2],out_row[2],in_bytes,out_bytes;
      device->GetCopyableFootprints(&sd,0,2,0,in.data(),in_rows,in_row,&in_bytes);
      device->GetCopyableFootprints(&dd,0,2,0,out.data(),out_rows,out_row,&out_bytes);
      if(in_row[0]!=c.sw*4u||in_row[1]!=c.sw||out_row[0]!=c.dw*4u||out_row[1]!=c.dw)return 7;
      auto upload=buffer(in_bytes,D3D12_HEAP_TYPE_UPLOAD),download=buffer(out_bytes,D3D12_HEAP_TYPE_READBACK);
      auto src_check=buffer(in_bytes,D3D12_HEAP_TYPE_READBACK);
      std::vector<float> depths(c.sw*c.sh);std::vector<unsigned char> stencils(c.sw*c.sh);
      for(UINT y=0;y<c.sh;++y)for(UINT x=0;x<c.sw;++x){
        const UINT i=y*c.sw+x;
        if(c.captured){
          std::memcpy(&depths[i],captured.data()+i*8,4);stencils[i]=captured[i*8+4];
        }else{
          depths[i]=float((x*7919u+y*104729u)%0xFFFFFFu)/float(0xFFFFFFu);
          stencils[i]=static_cast<unsigned char>(x+y*17u);
        }
      }
      void* mapped;D3D12_RANGE empty{0,0};hr(upload->Map(0,&empty,&mapped));std::memset(mapped,0,in_bytes);
      for(UINT y=0;y<c.sh;++y){
        std::memcpy(static_cast<char*>(mapped)+in[0].Offset+y*in[0].Footprint.RowPitch,depths.data()+y*c.sw,c.sw*4);
        std::memcpy(static_cast<char*>(mapped)+in[1].Offset+y*in[1].Footprint.RowPitch,stencils.data()+y*c.sw,c.sw);
      }upload->Unmap(0,nullptr);
      D3D12_DESCRIPTOR_HEAP_DESC hd{};hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_DSV;hd.NumDescriptors=1;
      ComPtr<ID3D12DescriptorHeap>dsv_heap;hr(device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&dsv_heap)));
      D3D12_DEPTH_STENCIL_VIEW_DESC dsv{};dsv.Format=DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
      dsv.ViewDimension=D3D12_DSV_DIMENSION_TEXTURE2D;
      device->CreateDepthStencilView(dst.Get(),&dsv,dsv_heap->GetCPUDescriptorHandleForHeapStart());
      begin();
      for(UINT plane=0;plane<2;++plane){
        D3D12_TEXTURE_COPY_LOCATION from{},to{};from.pResource=upload.Get();
        from.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;from.PlacedFootprint=in[plane];
        to.pResource=src.Get();to.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;to.SubresourceIndex=plane;
        list->CopyTextureRegion(&to,0,0,0,&from,nullptr);
      }
      transition(src.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
      list->ClearDepthStencilView(dsv_heap->GetCPUDescriptorHandleForHeapStart(),
        D3D12_CLEAR_FLAG_DEPTH|D3D12_CLEAR_FLAG_STENCIL,.123f,0xA5,0,nullptr);
      // Failed requests must reject before recording any copy.
      if(recorder->Record(list.Get(),src.Get(),src.Get()) || recorder->Record(list.Get(),nullptr,dst.Get()))return 8;
      auto lease=recorder->Record(list.Get(),src.Get(),dst.Get());if(!lease)return 9;
      transition(dst.Get(),D3D12_RESOURCE_STATE_DEPTH_WRITE,D3D12_RESOURCE_STATE_COPY_SOURCE);
      transition(src.Get(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_SOURCE);
      for(UINT plane=0;plane<2;++plane)for(UINT target=0;target<2;++target){
        D3D12_TEXTURE_COPY_LOCATION from{},to{};from.pResource=target?src.Get():dst.Get();
        from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;from.SubresourceIndex=plane;
        to.pResource=target?src_check.Get():download.Get();to.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        to.PlacedFootprint=target?in[plane]:out[plane];list->CopyTextureRegion(&to,0,0,0,&from,nullptr);
      }
      // The final case also releases the recorder and original owners before
      // submission. The returned lease must retain resources/root/PSOs through
      // the fence, rather than depending on the caller's local variables.
      if(c.captured){recorder.reset();src.Reset();dst.Reset();}
      submit();lease.reset();hr(download->Map(0,nullptr,&mapped));
      for(UINT y=0;y<c.dh;++y)for(UINT x=0;x<c.dw;++x){
        const UINT sx=UINT((uint64_t(2*x+1)*c.sw)/(2*c.dw)),sy=UINT((uint64_t(2*y+1)*c.sh)/(2*c.dh));
        float actual;std::memcpy(&actual,static_cast<char*>(mapped)+out[0].Offset+y*out[0].Footprint.RowPitch+x*4,4);
        const auto mask=static_cast<unsigned char*>(mapped)[out[1].Offset+y*out[1].Footprint.RowPitch+x];
        if(std::memcmp(&actual,&depths[sy*c.sw+sx],4)){
          printf("depth mismatch portable=%u %ux%u->%ux%u at%u,%u got=%.9g expected=%.9g\n",portable,c.sw,c.sh,c.dw,c.dh,x,y,actual,depths[sy*c.sw+sx]);return 23;}
        if(mask!=stencils[sy*c.sw+sx]){
          printf("stencil mismatch portable=%u at%u,%u got=%u expected=%u\n",portable,x,y,mask,stencils[sy*c.sw+sx]);return 23;}
      }download->Unmap(0,nullptr);
      hr(src_check->Map(0,nullptr,&mapped));
      for(UINT y=0;y<c.sh;++y){
        if(std::memcmp(static_cast<char*>(mapped)+in[0].Offset+y*in[0].Footprint.RowPitch,depths.data()+y*c.sw,c.sw*4) ||
           std::memcmp(static_cast<char*>(mapped)+in[1].Offset+y*in[1].Footprint.RowPitch,stencils.data()+y*c.sw,c.sw))return 24;
      }src_check->Unmap(0,nullptr);
      printf("PASS %ux%u->%ux%u captured=%u depth/stencil exact, source unchanged\n",c.sw,c.sh,c.dw,c.dh,c.captured);++checked;
    }
  }
  unsigned errors=0,warnings=0;
  if(info)for(UINT64 i=0;i<info->GetNumStoredMessages();++i){
    SIZE_T bytes=0;info->GetMessage(i,nullptr,&bytes);std::vector<char> data(bytes);
    auto* m=reinterpret_cast<D3D12_MESSAGE*>(data.data());hr(info->GetMessage(i,m,&bytes));
    if(m->Severity<=D3D12_MESSAGE_SEVERITY_ERROR){++errors;printf("DEBUG ERROR %s\n",m->pDescription);}
    else if(m->Severity==D3D12_MESSAGE_SEVERITY_WARNING){++warnings;printf("DEBUG WARNING %s\n",m->pDescription);}
  }
  CloseHandle(event);printf("PASS cases=%u debug_layer=%u errors=%u warnings=%u\n",checked,debug!=nullptr,errors,warnings);
  return errors||warnings?25:0;
}
