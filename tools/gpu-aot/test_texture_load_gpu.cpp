// Standalone D3D12 numeric oracle. No game, window, swap chain or user input.
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <cassert>
#include <cstring>
#include <iostream>
#include <vector>
#include "../../rexlego/src/gpu_native/texture_alpha4_upload.h"
#include "../../rexglue-sdk/src/graphics/shaders/bytecode/d3d12_5_1/texture_load_dxt3a_cs.h"
#include "../../rexglue-sdk/src/graphics/shaders/bytecode/d3d12_5_1/texture_load_64bpb_cs.h"
#include "oracle_tiled_offsets.h"
using Microsoft::WRL::ComPtr;
using namespace legodimensions::gpu_native;

int main() {
  ComPtr<IDXGIFactory4> factory; assert(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))));
  ComPtr<IDXGIAdapter1> adapter; assert(SUCCEEDED(factory->EnumAdapters1(0,&adapter)));
  DXGI_ADAPTER_DESC1 adapter_desc; assert(SUCCEEDED(adapter->GetDesc1(&adapter_desc)));
  std::wcout<<adapter_desc.Description<<L"\n";
  ComPtr<ID3D12Device> device;
  assert(SUCCEEDED(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device))));
  D3D12_DESCRIPTOR_RANGE ranges[2]{};
  ranges[0].RangeType=D3D12_DESCRIPTOR_RANGE_TYPE_SRV; ranges[0].NumDescriptors=1;
  ranges[1].RangeType=D3D12_DESCRIPTOR_RANGE_TYPE_UAV; ranges[1].NumDescriptors=1;
  D3D12_ROOT_PARAMETER params[3]{};
  params[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  params[0].Constants.Num32BitValues=10;
  for(int i=0;i<2;++i){
    params[i+1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[i+1].DescriptorTable={1,&ranges[i]};
  }
  D3D12_ROOT_SIGNATURE_DESC desc{3,params,0,nullptr,D3D12_ROOT_SIGNATURE_FLAG_NONE};
  ComPtr<ID3DBlob> serialized,errors;
  assert(SUCCEEDED(D3D12SerializeRootSignature(&desc,D3D_ROOT_SIGNATURE_VERSION_1,&serialized,&errors)));
  ComPtr<ID3D12RootSignature> root;
  assert(SUCCEEDED(device->CreateRootSignature(0,serialized->GetBufferPointer(),serialized->GetBufferSize(),IID_PPV_ARGS(&root))));
  D3D12_COMMAND_QUEUE_DESC queue_desc{};
  ComPtr<ID3D12CommandQueue> queue; assert(SUCCEEDED(device->CreateCommandQueue(&queue_desc,IID_PPV_ARGS(&queue))));
  ComPtr<ID3D12Fence> fence; assert(SUCCEEDED(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence))));
  HANDLE event=CreateEvent(nullptr,FALSE,FALSE,nullptr); assert(event);
  uint64_t fence_value=0;
  auto buffer=[&](size_t size,D3D12_HEAP_TYPE type,D3D12_RESOURCE_STATES state,bool uav){
    D3D12_HEAP_PROPERTIES heap{};heap.Type=type;
    D3D12_RESOURCE_DESC r{};r.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;
    r.Width=size;r.Height=1;r.DepthOrArraySize=1;r.MipLevels=1;r.SampleDesc.Count=1;
    r.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;if(uav)r.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ComPtr<ID3D12Resource> result;
    assert(SUCCEEDED(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&r,state,nullptr,IID_PPV_ARGS(&result))));
    return result;
  };
  uint32_t tested=0;
  for(const auto dimensions : {std::pair{32u,32u},std::pair{128u,64u},std::pair{1024u,512u}})
  for(bool alpha : {false,true})
  for(uint32_t tiled=0;tiled<2;++tiled)
  for(uint32_t endian=0;endian<4;++endian){
    const uint32_t width=dimensions.first,height=dimensions.second;
    const uint32_t pitch=((alpha?width:(width/4)*8)+255)&~255u;
    const uint32_t guest_pitch_blocks=((width/4)+31)&~31u;
    const uint32_t source_size=2*1024*1024,output_size=pitch*height;
    const uint32_t xor_value=endian==1?1:endian==2?3:endian==3?2:0;
    const uint32_t packed_x=4,packed_y=4;
    auto offset=[=](uint32_t x,uint32_t y,uint32_t)->int64_t{
      return tiled?GetTiledOffset2D(x+packed_x,y+packed_y,guest_pitch_blocks,3)
          : (y+packed_y)*guest_pitch_blocks*8+(x+packed_x)*8;
    };
    std::vector<uint8_t> source(source_size,0),expected(output_size,0);
    for(uint32_t y=0;y<height/4;++y)
    for(uint32_t x=0;x<width/4;++x)
    for(uint32_t b=0;b<8;++b)
      source[(size_t(offset(x,y,0))+b)^xor_value]=uint8_t(b*31+x*13+y*7);
    if(alpha){
      assert(CopyTextureAlpha4Blocks(source,expected,width,height,1,pitch,xor_value,offset));
    }else{
      for(uint32_t y=0;y<height/4;++y)
      for(uint32_t x=0;x<width/4;++x)
      for(uint32_t b=0;b<8;++b)
        expected[y*pitch+x*8+b]=source[(size_t(offset(x,y,0))+b)^xor_value];
    }
    // GPU loader starts at packed-mip offsets via the guest byte offset. A
    // tiled offset isn't linear in X/Y, so cover packed positions separately
    // in CPU tests and compare a zero-origin tiled texture here.
    if(tiled){
      std::vector<uint8_t> origin(source_size,0);
      for(uint32_t y=0;y<height/4;++y)
      for(uint32_t x=0;x<width/4;++x)
      for(uint32_t b=0;b<8;++b)
        origin[(size_t(GetTiledOffset2D(x,y,guest_pitch_blocks,3))+b)^xor_value]=source[(size_t(offset(x,y,0))+b)^xor_value];
      source=std::move(origin);
    }
    auto input=buffer(source_size,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ,false);
    auto output=buffer(output_size,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,true);
    auto readback=buffer(output_size,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST,false);
    void* mapped;D3D12_RANGE no_read{0,0};assert(SUCCEEDED(input->Map(0,&no_read,&mapped)));
    std::memcpy(mapped,source.data(),source.size());input->Unmap(0,nullptr);
    D3D12_DESCRIPTOR_HEAP_DESC heap_desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,2,D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,0};
    ComPtr<ID3D12DescriptorHeap> heap;assert(SUCCEEDED(device->CreateDescriptorHeap(&heap_desc,IID_PPV_ARGS(&heap))));
    const auto step=device->GetDescriptorHandleIncrementSize(heap_desc.Type);
    auto cpu=heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};srv.Format=DXGI_FORMAT_R32G32B32A32_UINT;
    srv.ViewDimension=D3D12_SRV_DIMENSION_BUFFER;srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Buffer.NumElements=source_size/16;device->CreateShaderResourceView(input.Get(),&srv,cpu);
    cpu.ptr+=step;
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};uav.Format=srv.Format;uav.ViewDimension=D3D12_UAV_DIMENSION_BUFFER;
    uav.Buffer.NumElements=output_size/16;device->CreateUnorderedAccessView(output.Get(),nullptr,&uav,cpu);
    D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline_desc{};pipeline_desc.pRootSignature=root.Get();
    pipeline_desc.CS=alpha?D3D12_SHADER_BYTECODE{texture_load_dxt3a_cs,sizeof(texture_load_dxt3a_cs)}
        :D3D12_SHADER_BYTECODE{texture_load_64bpb_cs,sizeof(texture_load_64bpb_cs)};
    ComPtr<ID3D12PipelineState> pipeline;assert(SUCCEEDED(device->CreateComputePipelineState(&pipeline_desc,IID_PPV_ARGS(&pipeline))));
    ComPtr<ID3D12CommandAllocator> allocator;assert(SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator))));
    ComPtr<ID3D12GraphicsCommandList> commands;
    assert(SUCCEEDED(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),pipeline.Get(),IID_PPV_ARGS(&commands))));
    commands->SetComputeRootSignature(root.Get());ID3D12DescriptorHeap* heaps[]={heap.Get()};commands->SetDescriptorHeaps(1,heaps);
    uint32_t constants[]={tiled|(endian<<2),tiled?0u:uint32_t(offset(0,0,0)),
        tiled?guest_pitch_blocks:guest_pitch_blocks*8,32,width/4,height/4,1,0,pitch,height};
    commands->SetComputeRoot32BitConstants(0,10,constants,0);
    auto gpu=heap->GetGPUDescriptorHandleForHeapStart();commands->SetComputeRootDescriptorTable(1,gpu);
    gpu.ptr+=step;commands->SetComputeRootDescriptorTable(2,gpu);
    commands->Dispatch(((width/4)+15)/16,((height/4)+31)/32,1);
    D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition={output.Get(),D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE};
    commands->ResourceBarrier(1,&barrier);commands->CopyBufferRegion(readback.Get(),0,output.Get(),0,output_size);
    assert(SUCCEEDED(commands->Close()));ID3D12CommandList* lists[]={commands.Get()};queue->ExecuteCommandLists(1,lists);
    assert(SUCCEEDED(queue->Signal(fence.Get(),++fence_value)));
    assert(SUCCEEDED(fence->SetEventOnCompletion(fence_value,event)));
    assert(WaitForSingleObject(event,30000)==WAIT_OBJECT_0);
    D3D12_RANGE read_range{0,output_size};assert(SUCCEEDED(readback->Map(0,&read_range,&mapped)));
    const uint32_t rows=alpha?height:height/4,bytes=alpha?width:(width/4)*8;
    for(uint32_t y=0;y<rows;++y)for(uint32_t x=0;x<bytes;++x){
      const auto actual=static_cast<uint8_t*>(mapped)[y*pitch+x];
      if(actual!=expected[y*pitch+x]){
        std::cerr<<"mismatch alpha="<<alpha<<" tiled="<<tiled<<" endian="<<endian<<" x="<<x<<" y="<<y
            <<" expected="<<unsigned(expected[y*pitch+x])<<" got="<<unsigned(actual)<<"\n";
        return 1;
      }
    }
    readback->Unmap(0,&no_read);++tested;
  }
  CloseHandle(event);
  std::cout<<"PASS: "<<tested<<" actual SDK D3D12 texture-loader comparisons; no game launched\n";
}
