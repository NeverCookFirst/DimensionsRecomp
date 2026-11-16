// Windowless production FSR 1 presenter, including native RCAS, EASU chains,
// downscaling, repeated slot reuse and letterbox offsets. No game or swap chain.
#define NOMINMAX
#include "gpu_native/fsr1_present.h"
#include <d3d12sdklayers.h>
#include <dxgi1_6.h>
#include "test_d3d12_adapter.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
using Microsoft::WRL::ComPtr;
using namespace legodimensions::gpu_native;
static void Hr(HRESULT h) { if (FAILED(h)) { std::printf("HRESULT %08X\n",unsigned(h));std::exit(2); } }
static void Check(bool b) { if (!b) std::exit(23); }
int main() {
  ComPtr<ID3D12Debug> debug;
  if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) debug->EnableDebugLayer();
  ComPtr<IDXGIFactory6> factory; Hr(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
  ComPtr<IDXGIAdapter1> adapter; Hr(TestD3D12Adapter(factory.Get(),&adapter));
  DXGI_ADAPTER_DESC1 adapter_desc{};Hr(adapter->GetDesc1(&adapter_desc));std::printf("adapter=%ls\n",adapter_desc.Description);
  ComPtr<ID3D12Device> device;Hr(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device)));
  ComPtr<ID3D12InfoQueue> info;device.As(&info);
  D3D12_COMMAND_QUEUE_DESC q{};ComPtr<ID3D12CommandQueue> queue;Hr(device->CreateCommandQueue(&q,IID_PPV_ARGS(&queue)));
  ComPtr<ID3D12CommandAllocator> allocator;Hr(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)));
  ComPtr<ID3D12GraphicsCommandList> list;Hr(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&list)));Hr(list->Close());
  ComPtr<ID3D12Fence> fence;Hr(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)));
  HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);UINT64 serial=0;
  auto begin=[&]{Hr(allocator->Reset());Hr(list->Reset(allocator.Get(),nullptr));};
  auto submit=[&]{Hr(list->Close());ID3D12CommandList* lists[]{list.Get()};queue->ExecuteCommandLists(1,lists);
    Hr(queue->Signal(fence.Get(),++serial));Hr(fence->SetEventOnCompletion(serial,event));
    Check(WaitForSingleObject(event,30000)==WAIT_OBJECT_0);};
  auto transition=[&](ID3D12Resource* r,D3D12_RESOURCE_STATES before,D3D12_RESOURCE_STATES after){
    D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition={r,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,before,after};list->ResourceBarrier(1,&b);};
  auto texture=[&](UINT w,UINT h,D3D12_RESOURCE_STATES state){
    D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;D3D12_RESOURCE_DESC d{};
    d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;d.Width=w;d.Height=h;d.DepthOrArraySize=d.MipLevels=1;
    d.SampleDesc.Count=1;d.Format=DXGI_FORMAT_R8G8B8A8_UNORM;d.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    ComPtr<ID3D12Resource> r;Hr(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&d,state,nullptr,IID_PPV_ARGS(&r)));return r;};
  auto buffer=[&](UINT64 bytes,D3D12_HEAP_TYPE type){
    D3D12_HEAP_PROPERTIES heap{};heap.Type=type;D3D12_RESOURCE_DESC d{};
    d.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;d.Width=bytes;d.Height=d.DepthOrArraySize=d.MipLevels=1;
    d.SampleDesc.Count=1;d.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;ComPtr<ID3D12Resource> r;
    Hr(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&d,type==D3D12_HEAP_TYPE_UPLOAD?
        D3D12_RESOURCE_STATE_GENERIC_READ:D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&r)));return r;};
  D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_RTV,1};ComPtr<ID3D12DescriptorHeap> rtv;
  Hr(device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&rtv)));const auto target_handle=rtv->GetCPUDescriptorHandleForHeapStart();
  const auto pattern=[](int x,int y,int c){return uint8_t(40+(x*13+y*7+c*31)%150);};
  struct Shape{UINT sw,sh,dw,dh;unsigned pattern;};
  const Shape cases[]{{64,64,64,64,true},{1280,720,1920,1080,false},{1129,635,1920,1080,false},
      {17,13,97,61,false},{8,8,257,257,false},{128,72,64,36,false},{64,64,64,64,true},
      {64,36,128,72,2},{17,13,97,61,2}};
  Fsr1Presenter presenter;unsigned checked=0;int maximum_error=0;
  for (unsigned reuse=0;reuse<2;++reuse) for(unsigned k=0;k<std::size(cases);++k) {
    const auto s=cases[k];const PresentRect rect{7,9,s.dw,s.dh};const UINT ow=s.dw+19,oh=s.dh+23;
    auto source=texture(s.sw,s.sh,D3D12_RESOURCE_STATE_COPY_DEST),target=texture(ow,oh,D3D12_RESOURCE_STATE_RENDER_TARGET);
    device->CreateRenderTargetView(target.Get(),nullptr,target_handle);
    auto desc=source->GetDesc();D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};UINT64 bytes;
    device->GetCopyableFootprints(&desc,0,1,0,&fp,nullptr,nullptr,&bytes);auto upload=buffer(bytes,D3D12_HEAP_TYPE_UPLOAD);
    uint8_t* mapped;Hr(upload->Map(0,nullptr,reinterpret_cast<void**>(&mapped)));
    for(UINT y=0;y<s.sh;++y)for(UINT x=0;x<s.sw;++x)for(int c=0;c<4;++c)
      mapped[y*size_t(fp.Footprint.RowPitch)+x*4+c]=c==3?255:s.pattern==1?pattern(x,y,c):
          s.pattern==2?uint8_t(std::lround(25.5f+150.f*(c%2?(y+.5f)/s.sh:(x+.5f)/s.sw))):uint8_t(32u<<c);
    upload->Unmap(0,nullptr);begin();
    D3D12_TEXTURE_COPY_LOCATION src{},dst{};src.pResource=upload.Get();src.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;src.PlacedFootprint=fp;
    dst.pResource=source.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
    transition(source.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    const float sentinel[]{1,0,1,1};list->ClearRenderTargetView(target_handle,sentinel,0,nullptr);
    const float reduction=reuse?1.5f:.2f;
    Check(presenter.Draw(device.Get(),list.Get(),source.Get(),target.Get(),k%3,rect,reduction,4,reuse!=0));
    transition(target.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_SOURCE);
    desc=target->GetDesc();device->GetCopyableFootprints(&desc,0,1,0,&fp,nullptr,nullptr,&bytes);auto readback=buffer(bytes,D3D12_HEAP_TYPE_READBACK);
    src={};src.pResource=target.Get();src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst={};dst.pResource=readback.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;dst.PlacedFootprint=fp;
    list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);submit();
    Hr(readback->Map(0,nullptr,reinterpret_cast<void**>(&mapped)));
    for(UINT y=0;y<oh;++y)for(UINT x=0;x<ow;++x) {
      auto* pixel=mapped+y*size_t(fp.Footprint.RowPitch)+x*4;
      if(x<rect.x || y<rect.y || x>=rect.x+s.dw || y>=rect.y+s.dh) {
        Check(pixel[0]==255 && pixel[1]==0 && pixel[2]==255 && pixel[3]==255);continue;
      }
      const int px=int(x-rect.x),py=int(y-rect.y);
      float expected[3]{};
      if(!s.pattern)for(int c=0;c<3;++c)expected[c]=float(32u<<c);
      else if(s.pattern==2) {
        // EASU must preserve the coordinates of a smooth signal. Exclude its
        // clamped border footprint when comparing the analytical ramp.
        if(px<8 || py<8 || px>=int(s.dw)-8 || py>=int(s.dh)-8)continue;
        for(int c=0;c<3;++c)expected[c]=25.5f+150.f*(c%2?(py+.5f)/s.dh:(px+.5f)/s.dw);
      }
      else {
        // Independent RCAS scalar equation; test native 1:1 without EASU.
        float neighbors[4][3],center[3],lobe=-.1875f;
        const int nx[]{px,px-1,px+1,px},ny[]{py-1,py,py,py+1};
        for(int c=0;c<3;++c) {
          center[c]=pattern(px,py,c)/255.f;float lo=1,hi=0;
          for(int n=0;n<4;++n){neighbors[n][c]=nx[n]<0 || ny[n]<0 || nx[n]>=int(s.sw) || ny[n]>=int(s.sh)?0.f:pattern(nx[n],ny[n],c)/255.f;
            lo=std::min(lo,neighbors[n][c]);hi=std::max(hi,neighbors[n][c]);}
          lobe=std::max(lobe,std::min(0.f,std::max(-std::min(center[c],lo)/(4*hi),(1-std::max(center[c],hi))/(4*lo-4))));
        }
        lobe*=std::exp2(-reduction);
        for(int c=0;c<3;++c)expected[c]=std::clamp((center[c]+lobe*(neighbors[0][c]+neighbors[1][c]+neighbors[2][c]+neighbors[3][c]))/(1+4*lobe),0.f,1.f)*255.f;
      }
      for(int c=0;c<3;++c){const int error=std::abs(int(pixel[c])-int(std::lround(expected[c])));maximum_error=std::max(maximum_error,error);
        // The ReXGlue EASU program uses approximate reciprocal/normalization;
        // this ramp checks spatial mapping, not bit-exact linear filtering.
        if(error>(s.pattern==2?4:1)){std::printf("FAIL case=%u reuse=%u pixel=%u,%u channel=%d got=%u expected=%g\n",k,reuse,x,y,c,pixel[c],expected[c]);return 24;}}
      Check(pixel[3]==255);++checked;
    }
    readback->Unmap(0,nullptr);std::printf("PASS %ux%u -> %ux%u slot=%u pattern=%u\n",s.sw,s.sh,s.dw,s.dh,k%3,s.pattern);
  }
  if(info)for(UINT64 i=0;i<info->GetNumStoredMessages();++i){SIZE_T size=0;info->GetMessage(i,nullptr,&size);std::vector<uint8_t> data(size);
    auto* msg=reinterpret_cast<D3D12_MESSAGE*>(data.data());info->GetMessage(i,msg,&size);
    if(msg->Severity<=D3D12_MESSAGE_SEVERITY_ERROR){std::printf("D3D12: %s\n",msg->pDescription);return 25;}}
  std::printf("PASS FSR1 pixels=%u max_error=%d debug_layer=%u; no game\n",checked,maximum_error,bool(debug));CloseHandle(event);
}
