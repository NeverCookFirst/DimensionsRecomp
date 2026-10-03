#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <chrono>
#include <iostream>
#include <vector>
#include <cassert>
#include "gpu_native/vertex_upload.h"
using Microsoft::WRL::ComPtr;
using namespace legodimensions::gpu_native;
int main() {
  ComPtr<IDXGIFactory4> factory; assert(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))));
  ComPtr<IDXGIAdapter1> adapter; assert(SUCCEEDED(factory->EnumAdapters1(0, &adapter)));
  DXGI_ADAPTER_DESC1 desc; adapter->GetDesc1(&desc); std::wcout << desc.Description << L"\n";
  ComPtr<ID3D12Device> device; assert(SUCCEEDED(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device))));
  constexpr size_t bytes = 6 * 1024 * 1024;
  D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_UPLOAD;
  D3D12_RESOURCE_DESC resource{};resource.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;
  resource.Width=bytes;resource.Height=1;resource.DepthOrArraySize=1;resource.MipLevels=1;
  resource.SampleDesc.Count=1;resource.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  ComPtr<ID3D12Resource> buffer;
  assert(SUCCEEDED(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&resource,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&buffer))));
  void* raw=nullptr;D3D12_RANGE no_read{0,0};assert(SUCCEEDED(buffer->Map(0,&no_read,&raw)));
  auto* mapped=static_cast<uint8_t*>(raw);
  std::vector<uint8_t> src(bytes), scratch(bytes);
  for(size_t i=0;i<bytes;++i)src[i]=uint8_t(i*73+(i>>8));
  const std::array<uint32_t,4> fields{8,12,16,20};const VertexByteOrder order{24,0,fields};
  auto old=[&]{
    std::vector<uint8_t> converted(bytes);
    for(size_t i=0;i<bytes;i+=4)for(size_t j=0;j<4;++j)converted[i+j]=src[i+3-j];
    assert(ApplyVertexByteOrder(converted.data(),bytes,order));
    std::memcpy(mapped,converted.data(),bytes);
  };
  old();std::memcpy(scratch.data(),mapped,bytes);
  assert(WriteAlignedVertexUpload(mapped,src.data(),bytes,order));
  assert(std::memcmp(mapped,scratch.data(),bytes)==0);
  auto time=[&](auto fn){
    auto start=std::chrono::steady_clock::now();
    for(unsigned i=0;i<40;++i)fn();
    return std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count()/40;
  };
  // Alternate the order over three trials; no window, present, GPU readback,
  // game process or user input. This measures only CPU writes/conversion.
  for(unsigned trial=0;trial<3;++trial){
    double legacy,fused;
    if(trial%2){fused=time([&]{assert(WriteAlignedVertexUpload(mapped,src.data(),bytes,order));});legacy=time(old);}
    else{legacy=time(old);fused=time([&]{assert(WriteAlignedVertexUpload(mapped,src.data(),bytes,order));});}
    std::cout << "trial="<<trial<<" bytes="<<bytes<<" legacy_ms="<<legacy<<" fused_ms="<<fused<<"\n";
  }
  buffer->Unmap(0,nullptr);
}
