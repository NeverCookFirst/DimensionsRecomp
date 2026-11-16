// Windowless FSR 3.1 upscaler fixture. No game, swap chain or network.
// Constant or moving scene with distinct color/depth/velocity and persistent history.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_6.h>
#include "test_d3d12_adapter.h"
#include <wrl/client.h>
#include <bit>
#ifdef LEGO_FSR_TEST_NATIVE_MODULE
#include "gpu_native/fsr3_upscaler.h"
#endif
#include <FidelityFX/host/ffx_fsr3upscaler.h>
#include <FidelityFX/host/backends/dx12/ffx_dx12.h>
#include <ffx_api/ffx_api_loader.h>
#include <ffx_api/ffx_framegeneration.h>
#include <ffx_api/dx12/ffx_api_dx12.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <vector>
#include <algorithm>
using Microsoft::WRL::ComPtr;
static void hr(HRESULT code) { if(FAILED(code)) { std::printf("HRESULT %08X\n",unsigned(code)); std::exit(1); } }
static void fx(FfxErrorCode code) { if(code!=FFX_OK) { std::printf("FFX error %d\n",int(code)); std::exit(2); } }
static void api(ffxReturnCode_t code) { if(code!=FFX_API_RETURN_OK) { std::printf("FFX API error %u\n",code); std::exit(9); } }
static LONG WINAPI FixtureCrashTrace(EXCEPTION_POINTERS* fault) {
  std::fprintf(stderr,"offline_fixture_exception=%08lX address=%p\n",
      fault->ExceptionRecord->ExceptionCode,fault->ExceptionRecord->ExceptionAddress);
  void* frames[48]; const auto count=CaptureStackBackTrace(0,48,frames,nullptr);
  for(unsigned i=0;i<count;++i) {
    HMODULE module=nullptr; char path[MAX_PATH]{};
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCSTR>(frames[i]),&module);
    if(module)GetModuleFileNameA(module,path,MAX_PATH);
    std::fprintf(stderr,"frame=%u module=%s offset=0x%llx\n",i,path,
        static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(frames[i])-reinterpret_cast<uintptr_t>(module)));
  }
  return EXCEPTION_EXECUTE_HANDLER;
}
int main(int argc,char** argv) {
  std::setvbuf(stdout,nullptr,_IONBF,0);
  SetUnhandledExceptionFilter(FixtureCrashTrace);
  const bool moving=std::getenv("LEGO_FSR_TEST_MOVING")!=nullptr;
  const bool resetEveryFrame=std::getenv("LEGO_FSR_TEST_RESET_EVERY_FRAME")!=nullptr;
  if(argc!=1 && argc!=5 && argc!=6)return 7;
#ifdef LEGO_FSR_TEST_NATIVE_MODULE
  if(argc==6)return 7; // This path deliberately has no frame generation.
#endif
  const unsigned rw=argc>=5?std::strtoul(argv[1],nullptr,10):128;
  const unsigned rh=argc>=5?std::strtoul(argv[2],nullptr,10):72;
  const unsigned ow=argc>=5?std::strtoul(argv[3],nullptr,10):192;
  const unsigned oh=argc>=5?std::strtoul(argv[4],nullptr,10):108;
  if(!rw||!rh||ow<rw||oh<rh||ow>4096||oh>4096)return 7;
  const auto version=ffxFsr3UpscalerGetEffectVersion();
  if(version!=FFX_SDK_MAKE_VERSION(3,1,4))return 8;
  std::printf("effect_version_hex=%08X\n",unsigned(version));
  ComPtr<ID3D12Debug> debug;
  const bool debugEnabled=SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)));
  if(debugEnabled)debug->EnableDebugLayer();
  ComPtr<IDXGIFactory4> factory; hr(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
  ComPtr<IDXGIAdapter1> adapter; hr(TestD3D12Adapter(factory.Get(),&adapter));
  DXGI_ADAPTER_DESC1 ad{};hr(adapter->GetDesc1(&ad));
  std::printf("adapter=%ls\n",ad.Description);
  ComPtr<ID3D12Device> dev;hr(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&dev)));
  ComPtr<ID3D12InfoQueue> messages;dev.As(&messages);
  D3D12_COMMAND_QUEUE_DESC qd{};ComPtr<ID3D12CommandQueue> queue;hr(dev->CreateCommandQueue(&qd,IID_PPV_ARGS(&queue)));
  ComPtr<ID3D12CommandAllocator> alloc;hr(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&alloc)));
  ComPtr<ID3D12GraphicsCommandList> list;hr(dev->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,alloc.Get(),nullptr,IID_PPV_ARGS(&list)));hr(list->Close());
  ComPtr<ID3D12Fence> fence;hr(dev->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)));
  HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);if(!event)return 3;UINT64 serial=0;
  auto submit=[&]{hr(list->Close());ID3D12CommandList* lists[]={list.Get()};queue->ExecuteCommandLists(1,lists);hr(queue->Signal(fence.Get(),++serial));hr(fence->SetEventOnCompletion(serial,event));if(WaitForSingleObject(event,30000)!=WAIT_OBJECT_0)std::exit(4);};
  auto begin=[&]{hr(alloc->Reset());hr(list->Reset(alloc.Get(),nullptr));};
  auto tex=[&](DXGI_FORMAT fmt,unsigned w,unsigned h,D3D12_RESOURCE_STATES state){
    D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;d.Width=w;d.Height=h;d.DepthOrArraySize=1;d.MipLevels=1;d.Format=fmt;d.SampleDesc.Count=1;d.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ComPtr<ID3D12Resource> r;hr(dev->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&d,state,nullptr,IID_PPV_ARGS(&r)));return r;
  };
  auto buffer=[&](UINT64 bytes,D3D12_HEAP_TYPE type,D3D12_RESOURCE_STATES state){
    D3D12_HEAP_PROPERTIES hp{};hp.Type=type;D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;d.Width=bytes;d.Height=1;d.DepthOrArraySize=1;d.MipLevels=1;d.SampleDesc.Count=1;d.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> r;hr(dev->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&d,state,nullptr,IID_PPV_ARGS(&r)));return r;
  };
  auto transition=[&](ID3D12Resource* r,D3D12_RESOURCE_STATES before,D3D12_RESOURCE_STATES after){D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;b.Transition={r,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,before,after};list->ResourceBarrier(1,&b);};
  auto color=tex(DXGI_FORMAT_R32G32B32A32_FLOAT,rw,rh,D3D12_RESOURCE_STATE_COPY_DEST);
  auto depth=tex(DXGI_FORMAT_R32_FLOAT,rw,rh,D3D12_RESOURCE_STATE_COPY_DEST);
  auto motion=tex(DXGI_FORMAT_R32G32_FLOAT,rw,rh,D3D12_RESOURCE_STATE_COPY_DEST);
  std::vector<ComPtr<ID3D12Resource>> uploads;
  unsigned sceneFrame=0;float sceneJitterX=0,sceneJitterY=0;
  begin();
  auto fill=[&](ID3D12Resource* r,unsigned channels,const float* value){
    auto d=r->GetDesc();D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};UINT64 bytes;dev->GetCopyableFootprints(&d,0,1,0,&fp,nullptr,nullptr,&bytes);
    auto up=buffer(bytes,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);void* p;D3D12_RANGE empty{0,0};hr(up->Map(0,&empty,&p));
    for(unsigned y=0;y<rh;++y)for(unsigned x=0;x<rw;++x){
      const float px=(x+0.5f+sceneJitterX)*ow/rw,py=(y+0.5f+sceneJitterY)*oh/rh;
      const float center=ow*0.4f+sceneFrame*4.0f;
      const bool inside=std::abs(px-center)<ow/16.0f && py>oh/3.0f && py<oh*2.0f/3.0f;
      for(unsigned c=0;c<channels;++c){
        float v=value[c];
        if(moving){
          if(channels==4)v=c==3?1.0f:inside?(c==0?0.8f:c==1?0.3f:0.1f):0.1f;
          else if(channels==1)v=inside?0.25f:0.75f;
          else v=inside&&c==0?-4.0f/ow:0.0f;
        }
        reinterpret_cast<float*>(static_cast<char*>(p)+fp.Offset+y*fp.Footprint.RowPitch)[x*channels+c]=v;
      }
    }
    up->Unmap(0,nullptr);D3D12_TEXTURE_COPY_LOCATION src{};src.pResource=up.Get();src.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;src.PlacedFootprint=fp;
    D3D12_TEXTURE_COPY_LOCATION dst{};dst.pResource=r;dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
    transition(r,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);uploads.push_back(up);
  };
  float cv[]={0.2f,0.4f,0.6f,1.0f},dv[]={0.5f},mv[]={0.0f,0.0f};fill(color.Get(),4,cv);fill(depth.Get(),1,dv);fill(motion.Get(),2,mv);submit();uploads.clear();
#ifdef LEGO_FSR_TEST_NATIVE_MODULE
  using namespace legodimensions::gpu_native;
  auto nativeContext=Fsr3Upscaler::Create(dev.Get(),{rw,rh},{ow,oh});
  if(!nativeContext)return 12;
  unsigned historyResets=0;
#else
  std::vector<unsigned char> scratch(ffxGetScratchMemorySizeDX12(FFX_FSR3UPSCALER_CONTEXT_COUNT));
  FfxFsr3UpscalerContextDescription cd{};cd.flags=FFX_FSR3UPSCALER_ENABLE_AUTO_EXPOSURE;cd.maxRenderSize={rw,rh};cd.maxUpscaleSize={ow,oh};
  fx(ffxGetInterfaceDX12(&cd.backendInterface,ffxGetDeviceDX12(dev.Get()),scratch.data(),scratch.size(),FFX_FSR3UPSCALER_CONTEXT_COUNT));
  auto context=std::make_unique<FfxFsr3UpscalerContext>();fx(ffxFsr3UpscalerContextCreate(context.get(),&cd));
  FfxFsr3UpscalerSharedResourceDescriptions shared{};fx(ffxFsr3UpscalerGetSharedResourceDescriptions(context.get(),&shared));
  // Formats/dimensions checked against the SDK's own returned descriptions.
  if(shared.dilatedDepth.resourceDescription.format!=FFX_SURFACE_FORMAT_R32_FLOAT || shared.dilatedMotionVectors.resourceDescription.format!=FFX_SURFACE_FORMAT_R16G16_FLOAT || shared.reconstructedPrevNearestDepth.resourceDescription.format!=FFX_SURFACE_FORMAT_R32_UINT)return 5;
  auto dilDepth=tex(DXGI_FORMAT_R32_FLOAT,rw,rh,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  auto dilMotion=tex(DXGI_FORMAT_R16G16_FLOAT,rw,rh,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  auto prevDepth=tex(DXGI_FORMAT_R32_UINT,rw,rh,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
#endif
  auto output=tex(DXGI_FORMAT_R32G32B32A32_FLOAT,ow,oh,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  HMODULE module=nullptr;ffxFunctions functions{};ffxContext fg=nullptr;
  ComPtr<ID3D12Resource> generated;
  if(argc==6){
    module=LoadLibraryExA(argv[5],nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if(!module){std::printf("Provider load error %lu\n",GetLastError());return 10;}
    ffxLoadFunctions(&functions,module);
    if(!functions.CreateContext||!functions.Configure||!functions.Dispatch||!functions.DestroyContext||!functions.Query)return 11;
    ffxCreateBackendDX12Desc backend{};backend.header.type=FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;backend.device=dev.Get();
    ffxCreateContextDescFrameGeneration create{};create.header.type=FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
    create.header.pNext=&backend.header;create.displaySize={ow,oh};create.maxRenderSize={rw,rh};create.backBufferFormat=FFX_API_SURFACE_FORMAT_R32G32B32A32_FLOAT;
    api(functions.CreateContext(&fg,&create.header,nullptr));
    ffxQueryGetProviderVersion provider{};provider.header.type=FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
    api(functions.Query(&fg,&provider.header));std::printf("frame_generation_provider=%s\n",provider.versionName?provider.versionName:"unknown");
    generated=tex(DXGI_FORMAT_R32G32B32A32_FLOAT,ow,oh,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  }
  auto res=[](ID3D12Resource* r,const wchar_t* name,FfxResourceStates state,FfxResourceUsage usage){return ffxGetResourceDX12(r,ffxGetResourceDescriptionDX12(r,usage),name,state);};
#ifdef LEGO_FSR_TEST_NATIVE_MODULE
  // Leave a full accumulation interval after the cut/reset/gap checks.
  unsigned frames=moving?32:8;
#else
  unsigned frames=moving?16:8;
#endif
  for(unsigned frame=0;frame<frames;++frame){
    const uint64_t frameId=frame;
    begin();FfxFsr3UpscalerDispatchDescription d{};d.commandList=ffxGetCommandListDX12(list.Get());
    d.color=res(color.Get(),L"Synthetic color",FFX_RESOURCE_STATE_COMPUTE_READ,FFX_RESOURCE_USAGE_READ_ONLY);
    d.depth=res(depth.Get(),L"Synthetic depth",FFX_RESOURCE_STATE_COMPUTE_READ,FFX_RESOURCE_USAGE_DEPTHTARGET);
    d.motionVectors=res(motion.Get(),L"Static velocity",FFX_RESOURCE_STATE_COMPUTE_READ,FFX_RESOURCE_USAGE_READ_ONLY);
#ifndef LEGO_FSR_TEST_NATIVE_MODULE
    d.dilatedDepth=res(dilDepth.Get(),L"Dilated depth",FFX_RESOURCE_STATE_UNORDERED_ACCESS,FFX_RESOURCE_USAGE_UAV);
    d.dilatedMotionVectors=res(dilMotion.Get(),L"Dilated motion",FFX_RESOURCE_STATE_UNORDERED_ACCESS,FFX_RESOURCE_USAGE_UAV);
    d.reconstructedPrevNearestDepth=res(prevDepth.Get(),L"Previous depth",FFX_RESOURCE_STATE_UNORDERED_ACCESS,FFX_RESOURCE_USAGE_UAV);
    d.output=res(output.Get(),L"Upscaled",FFX_RESOURCE_STATE_UNORDERED_ACCESS,FFX_RESOURCE_USAGE_UAV);
#else
    d.output=res(output.Get(),L"Upscaled",FFX_RESOURCE_STATE_UNORDERED_ACCESS,FFX_RESOURCE_USAGE_UAV);
#endif
    d.renderSize={rw,rh};d.upscaleSize={ow,oh};d.motionVectorScale={float(rw),float(rh)};d.frameTimeDelta=1000.0f/30;d.preExposure=1;d.reset=frame==0;d.cameraNear=0.1f;d.cameraFar=1000;d.cameraFovAngleVertical=1.04719755f;d.viewSpaceToMetersFactor=1;
    // A constant scene is invariant to camera jitter; use the real FSR sequence.
#ifdef LEGO_FSR_TEST_NATIVE_MODULE
    const uint64_t nativeFrame=frame+(frame>=12?1:0);
    if(!nativeContext->Jitter(nativeFrame,d.jitterOffset.x,d.jitterOffset.y))return 13;
#else
    fx(ffxFsr3UpscalerGetJitterOffset(&d.jitterOffset.x,&d.jitterOffset.y,frame,ffxFsr3UpscalerGetJitterPhaseCount(rw,ow)));
#endif
    if(moving){
      sceneFrame=frame;sceneJitterX=d.jitterOffset.x;sceneJitterY=d.jitterOffset.y;
      for(auto* r:{color.Get(),depth.Get(),motion.Get()})transition(r,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_DEST);
      fill(color.Get(),4,cv);fill(depth.Get(),1,dv);fill(motion.Get(),2,mv);
    }
#ifdef LEGO_FSR_TEST_NATIVE_MODULE
    Fsr3FrameInputs inputs;
    inputs.color=color.Get();inputs.depth=depth.Get();inputs.motion=motion.Get();inputs.output=output.Get();
    inputs.frame=nativeFrame;inputs.scene_epoch=frame>=8?2:1;
    inputs.jitter_x=d.jitterOffset.x;inputs.jitter_y=d.jitterOffset.y;
    inputs.frame_time_ms=d.frameTimeDelta;inputs.pre_exposure=1;
    inputs.camera_near=d.cameraNear;inputs.camera_far=d.cameraFar;inputs.camera_fov=d.cameraFovAngleVertical;
    inputs.sharpen=true;inputs.sharpness=0.2f;inputs.camera_cut=frame==4;
    if(frame==0){
      auto bad=inputs;bad.depth=bad.color;if(nativeContext->Dispatch(list.Get(),bad))return 14;
      bad=inputs;bad.motion=bad.depth;if(nativeContext->Dispatch(list.Get(),bad))return 14;
      bad=inputs;bad.camera_near=0;if(nativeContext->Dispatch(list.Get(),bad))return 14;
      bad=inputs;bad.jitter_x+=1;if(nativeContext->Dispatch(list.Get(),bad))return 14;
      bad=inputs;bad.sharpness=2;if(nativeContext->Dispatch(list.Get(),bad))return 14;
      bad=inputs;bad.output=bad.color;if(nativeContext->Dispatch(list.Get(),bad))return 14;
    }
    if(frame==14)nativeContext->InvalidateHistory();
    if(!nativeContext->Dispatch(list.Get(),inputs))return 15;
    const bool expectedReset=frame==0||frame==4||frame==8||frame==12||frame==14;
    if(nativeContext->LastDispatchResetHistory()!=expectedReset)return 16;
    historyResets+=nativeContext->LastDispatchResetHistory();
    if(frame==frames-1){
      if(nativeContext->Dispatch(list.Get(),inputs))return 17;
      nativeContext->InvalidateHistory();
      if(nativeContext->Dispatch(list.Get(),inputs))return 17;
    }
#else
    fx(ffxFsr3UpscalerContextDispatch(context.get(),&d));
#endif
    if(fg){
      transition(output.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
      ffxConfigureDescFrameGeneration config{};config.header.type=FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
      config.frameGenerationEnabled=true;config.flags=FFX_FRAMEGENERATION_FLAG_NO_SWAPCHAIN_CONTEXT_NOTIFY;
      config.generationRect={0,0,int32_t(ow),int32_t(oh)};config.frameID=frameId;
      api(functions.Configure(&fg,&config.header));
      ffxDispatchDescFrameGenerationPrepareCameraInfo camera{};camera.header.type=FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_CAMERAINFO;
      camera.cameraUp[1]=1;camera.cameraRight[0]=1;camera.cameraForward[2]=1;
      ffxDispatchDescFrameGenerationPrepare prepare{};prepare.header.type=FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE;
      prepare.header.pNext=&camera.header;prepare.frameID=frameId;prepare.commandList=list.Get();prepare.renderSize={rw,rh};
      prepare.jitterOffset={d.jitterOffset.x,d.jitterOffset.y};prepare.motionVectorScale={float(rw),float(rh)};
      prepare.frameTimeDelta=d.frameTimeDelta;prepare.cameraNear=d.cameraNear;prepare.cameraFar=d.cameraFar;
      prepare.cameraFovAngleVertical=d.cameraFovAngleVertical;prepare.viewSpaceToMetersFactor=1;
      prepare.depth=ffxApiGetResourceDX12(depth.Get());prepare.motionVectors=ffxApiGetResourceDX12(motion.Get());
      api(functions.Dispatch(&fg,&prepare.header));
      ffxDispatchDescFrameGeneration generate{};generate.header.type=FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION;
      generate.commandList=list.Get();generate.presentColor=ffxApiGetResourceDX12(output.Get());
      generate.outputs[0]=ffxApiGetResourceDX12(generated.Get(),FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
      generate.numGeneratedFrames=1;generate.reset=frame==0||resetEveryFrame;generate.frameID=frameId;generate.generationRect=config.generationRect;
      generate.backbufferTransferFunction=FFX_API_BACKBUFFER_TRANSFER_FUNCTION_SCRGB;generate.minMaxLuminance[1]=1000;
      api(functions.Dispatch(&fg,&generate.header));
      transition(output.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
    submit();
    uploads.clear();
  }
  auto od=output->GetDesc();D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};UINT64 bytes;dev->GetCopyableFootprints(&od,0,1,0,&fp,nullptr,nullptr,&bytes);
  auto readback=buffer(bytes,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);begin();transition(output.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);
  D3D12_TEXTURE_COPY_LOCATION src{};src.pResource=output.Get();src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  D3D12_TEXTURE_COPY_LOCATION dst{};dst.pResource=readback.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;dst.PlacedFootprint=fp;list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);submit();
  void* mapped;D3D12_RANGE range{0,SIZE_T(bytes)};hr(readback->Map(0,&range,&mapped));double maxError=0;unsigned invalid=0;
  auto centroid=[&](const void* data){double mass=0,weighted=0;
    for(unsigned y=0;y<oh;++y)for(unsigned x=0;x<ow;++x){
      const auto* pixel=reinterpret_cast<const float*>(static_cast<const char*>(data)+fp.Offset+y*fp.Footprint.RowPitch)+4*x;
      const double weight=std::max(0.0,double(pixel[0])-double(pixel[1]));mass+=weight;weighted+=weight*(x+0.5);
    }
    return mass>1?weighted/mass:-1.0;
  };
  const double upscaledCenter=moving?centroid(mapped):0;
  for(unsigned y=0;y<oh;++y)for(unsigned x=0;x<ow;++x)for(unsigned c=0;c<3;++c){float v=reinterpret_cast<float*>(static_cast<char*>(mapped)+fp.Offset+y*fp.Footprint.RowPitch)[x*4+c];if(!std::isfinite(v))++invalid;else maxError=std::fmax(maxError,std::fabs(v-cv[c]));}
  readback->Unmap(0,nullptr);
  double generatedError=0;unsigned generatedInvalid=0;
  double generatedCenter=0;
  if(fg){
    begin();transition(generated.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);
    src.pResource=generated.Get();list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);submit();
    hr(readback->Map(0,&range,&mapped));
    if(moving)generatedCenter=centroid(mapped);
    for(unsigned y=0;y<oh;++y)for(unsigned x=0;x<ow;++x)for(unsigned c=0;c<3;++c){
      float v=reinterpret_cast<float*>(static_cast<char*>(mapped)+fp.Offset+y*fp.Footprint.RowPitch)[x*4+c];
      if(!std::isfinite(v))++generatedInvalid;else generatedError=std::fmax(generatedError,std::fabs(v-cv[c]));
    }
    readback->Unmap(0,nullptr);api(functions.DestroyContext(&fg,nullptr));FreeLibrary(module);
    std::printf("generated_frames=%u nonfinite=%u manual_dispatch=1 swapchain_pacing_tested=0\n",frames,generatedInvalid);
    if(!moving)std::printf("generated_static_rgb_error=%.9f\n",generatedError);
  }
#ifdef LEGO_FSR_TEST_NATIVE_MODULE
  nativeContext.reset(); // All submissions have completed, including readback.
  std::printf("production_upscaler=1 history_resets=%u input_rejections_checked=1 frame_generation=0\n",historyResets);
#else
  fx(ffxFsr3UpscalerContextDestroy(context.get()));
#endif
  CloseHandle(event);
  unsigned gpuErrors=0;
  if(messages)for(UINT64 i=0;i<messages->GetNumStoredMessages();++i){
    SIZE_T length=0;hr(messages->GetMessage(i,nullptr,&length));std::vector<unsigned char> storage(length);
    auto* message=reinterpret_cast<D3D12_MESSAGE*>(storage.data());hr(messages->GetMessage(i,message,&length));
    if(message->Severity<=D3D12_MESSAGE_SEVERITY_ERROR){++gpuErrors;std::printf("D3D12 error: %s\n",message->pDescription);}
  }
  std::printf("debug_layer=%u gpu_errors=%u\n",debugEnabled,gpuErrors);
  std::printf("fsr=3.1.4 frames=%u render=%ux%u output=%ux%u nonfinite=%u game_launched=0\n",frames,rw,rh,ow,oh,invalid);
  if(!moving)std::printf("upscaled_static_rgb_error=%.9f\n",maxError);
  if(moving){
    const double expected=ow*0.4+4.0*(frames-1),interpolated=expected-2;
    std::printf("moving_object upscaled_center=%.6f expected=%.6f generated_center=%.6f expected=%.6f\n",upscaledCenter,expected,generatedCenter,interpolated);
    return gpuErrors||invalid||generatedInvalid||std::abs(upscaledCenter-expected)>1.0||(argc==6&&std::abs(generatedCenter-interpolated)>1.0)?12:0;
  }
  return gpuErrors || invalid || maxError>0.03 || generatedInvalid || generatedError>0.03 ? 6 : 0;
}
