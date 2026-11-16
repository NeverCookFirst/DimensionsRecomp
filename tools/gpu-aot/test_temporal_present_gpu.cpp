// Windowless production presentation test. Host ownership is a fence-aware
// fixture; Plume, DX12, the temporal presenter and FSR dispatch are production.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <plume_d3d12.h>
#include "gpu_native/temporal_present.h"
#include "gpu_native/fsr3_settings.h"
#include "gpu_native/device.h"
#include <cassert>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <vector>
namespace plume {std::unique_ptr<RenderInterface> CreateD3D12Interface();}
using namespace legodimensions::gpu_native;
#undef assert
#define assert(value) do {if(!(value)){printf("FAIL assertion line=%d: %s\n",__LINE__,#value);std::exit(23);}} while(0)
namespace {
plume::RenderDevice* device;
plume::RenderCommandList* commands;
plume::RenderDescriptorSet* descriptors;
std::vector<std::shared_ptr<void>> retired;
uint32_t next_descriptor = 1;
Fsr3Size render;
bool sharpening = true;
bool enabled = true;
float sharpness = .2f;
std::vector<char> Read(const char* path) {
  std::ifstream f(path,std::ios::binary|std::ios::ate); assert(f);
  std::vector<char> b(size_t(f.tellg())); f.seekg(0);f.read(b.data(),b.size());return b;
}
std::shared_ptr<plume::RenderTexture> Texture(uint32_t w,uint32_t h,plume::RenderFormat format) {
  plume::RenderTextureDesc desc;desc.dimension=plume::RenderTextureDimension::TEXTURE_2D;
  desc.width=w;desc.height=h;desc.depth=desc.arraySize=desc.mipLevels=1;
  desc.format=format;desc.flags=plume::RenderTextureFlag::RENDER_TARGET;desc.committed=true;
  desc.multisampling.sampleCount=plume::RenderSampleCount::COUNT_1;
  return device->createTexture(desc);
}
}
namespace legodimensions::gpu_native {
plume::RenderDevice* HostDevice::Device(){return device;}
plume::RenderCommandList* HostDevice::BeginFrameCommands(){return commands;}
u32 HostDevice::RegisterTexture(plume::RenderTexture* t,plume::RenderTextureView* v){
  const auto index=next_descriptor++;assert(index<64);descriptors->setTexture(index,t,plume::RenderTextureLayout::SHADER_READ,v);return index;
}
void HostDevice::UnregisterTexture(u32){}
void HostDevice::RetireResource(std::shared_ptr<void> r){if(r)retired.push_back(std::move(r));}
void HostDevice::SnapshotTexture(plume::RenderTexture*,std::string_view,bool){}
bool HostDevice::SnapshotFrameActive(){return false;}
bool NativeFsr3Enabled(){return enabled;}
Fsr3Size NativeFsr3RenderSize(){return render;}
bool NativeFsr3Sharpening(){return sharpening;}
float NativeFsr3Sharpness(){return sharpness;}
}
int main(int argc,char** argv) {
  assert(argc>=5 && argc<=9);render={uint32_t(std::atoi(argv[1])),uint32_t(std::atoi(argv[2]))};
  const bool original_warp=argc==9;
  const bool switch_presets=std::getenv("LEGO_TEST_SWITCH_PRESETS")!=nullptr;
  const bool toggle_mode=std::getenv("LEGO_TEST_TOGGLE_MODE")!=nullptr;
  assert(!switch_presets || original_warp || toggle_mode);
  const std::filesystem::path original_inputs=original_warp?argv[8]:"";
  const bool patterned=argc>=6,warp_geometry=argc>=7;
  const bool invert_jitter=patterned && std::strcmp(argv[5],"invert-jitter")==0;
  const bool force_fallback=patterned && std::strcmp(argv[5],"fallback")==0;
  const bool late_camera=patterned && std::strcmp(argv[5],"late-camera")==0;
  const bool composition=patterned && std::strcmp(argv[5],"compose")==0;
  const auto scene=[](float x,float y,int channel) {
    const float base[]{.125f,.25f,.5f};
    return base[channel]+.09f*std::sin(x*180.f+channel)*std::cos(y*155.f-channel);
  };
  auto render_interface=plume::CreateD3D12Interface();assert(render_interface);
  auto owner=render_interface->createDevice();assert(owner);device=owner.get();
  auto queue=device->createCommandQueue(plume::RenderCommandListType::DIRECT);
  auto list=queue->createCommandList();commands=list.get();
  auto fence=device->createCommandFence();
  plume::RenderDescriptorSetBuilder tables;tables.begin();tables.addTexture(0,64);tables.end(true,64);
  auto table=tables.create(device);descriptors=table.get();
  plume::RenderDescriptorSetBuilder samplers;samplers.begin();samplers.addSampler(0,1);samplers.end();
  auto sampler_table=samplers.create(device);
  plume::RenderSamplerDesc sampler_desc;sampler_desc.minFilter=sampler_desc.magFilter=plume::RenderFilter::LINEAR;
  sampler_desc.addressU=sampler_desc.addressV=plume::RenderTextureAddressMode::CLAMP;
  if(original_warp){
    sampler_desc.addressU=sampler_desc.addressV=plume::RenderTextureAddressMode::WRAP;
    sampler_desc.addressW=plume::RenderTextureAddressMode::CLAMP;
    sampler_desc.mipmapMode=plume::RenderMipmapMode::NEAREST;sampler_desc.maxLOD=.25f;
    sampler_desc.borderColor=plume::RenderBorderColor::TRANSPARENT_BLACK;
  }
  auto sampler=device->createSampler(sampler_desc);sampler_table->setSampler(0,sampler.get());
  plume::RenderPipelineLayoutBuilder builder;builder.begin(false,true);
  builder.addDescriptorSet(tables);builder.addDescriptorSet(tables);builder.addDescriptorSet(tables);builder.addDescriptorSet(samplers);
  if(original_warp)for(uint32_t i=0;i<3;++i)
    builder.addRootDescriptor(i,4,plume::RenderRootDescriptorType::CONSTANT_BUFFER);
  builder.addPushConstant(3,4,16,plume::RenderShaderStageFlag::PIXEL);builder.end();
  auto layout=builder.create(device);
  auto vs_bytes=Read(argv[3]),ps_bytes=Read(argv[4]);
  auto vs=device->createShader(vs_bytes.data(),vs_bytes.size(),"main",plume::RenderShaderFormat::DXIL);
  auto ps=device->createShader(ps_bytes.data(),ps_bytes.size(),"main",plume::RenderShaderFormat::DXIL);
  struct WarpVertex{float clip[4],uv[2];};
  std::vector<WarpVertex> warp,oracle;
  std::unique_ptr<plume::RenderBuffer> warp_buffer;
  const plume::RenderInputSlot warp_slot(0,original_warp?36:sizeof(WarpVertex));
  const plume::RenderInputElement warp_elements[]{
      {"POSITION",0,0,plume::RenderFormat::R32G32B32A32_FLOAT,0,0},
      {"TEXCOORD",0,1,plume::RenderFormat::R32G32_FLOAT,0,16}};
  if(warp_geometry){
    const auto unpack=[&](const char* path){
      auto raw=Read(path);assert(raw.size()==1536*304);std::vector<WarpVertex> result(1536);
      for(size_t i=0;i<result.size();++i){
        std::memcpy(result[i].clip,raw.data()+i*304,16);
        std::memcpy(result[i].uv,raw.data()+i*304+16,8);
        for(float x:result[i].clip)assert(std::isfinite(x));
        assert(result[i].clip[3]>0);
      }return result;
    };
    warp=unpack(argv[6]);oracle=unpack(argc>=8?argv[7]:argv[6]);
    std::vector<char> raw;
    if(original_warp){raw=Read((original_inputs/"vb-0.bin").string().c_str());assert(raw.size()==1536*36);}
    const size_t size=original_warp?raw.size():warp.size()*sizeof(WarpVertex);
    warp_buffer=device->createBuffer(plume::RenderBufferDesc::VertexBuffer(size,plume::RenderHeapType::UPLOAD));
    assert(warp_buffer);std::memcpy(warp_buffer->map(),original_warp?static_cast<const void*>(raw.data()):warp.data(),size);warp_buffer->unmap();
  }
  std::array<std::vector<char>,3> original_constants;
  std::array<std::unique_ptr<plume::RenderBuffer>,3> original_cb;
  if(original_warp)for(uint32_t i=0;i<3;++i){
    original_constants[i]=Read((original_inputs/("cb"+std::to_string(i)+".bin")).string().c_str());
    assert(original_constants[i].size()==(i==2?624:4096));
    original_cb[i]=device->createBuffer(plume::RenderBufferDesc::UploadBuffer((original_constants[i].size()+255)&~size_t(255)));
    assert(original_cb[i]);
  }
  const auto source_uv=[&](double px,double py){
    if(!warp_geometry)return std::array<float,2>{float(px),float(py)};
    for(size_t i=0;i<oracle.size();i+=3){
      double x[3],y[3];for(int v=0;v<3;++v){
        x[v]=double(oracle[i+v].clip[0])/oracle[i+v].clip[3]*.5+.5;
        y[v]=.5-double(oracle[i+v].clip[1])/oracle[i+v].clip[3]*.5;
      }
      const double denominator=(y[1]-y[2])*(x[0]-x[2])+(x[2]-x[1])*(y[0]-y[2]);
      if(std::abs(denominator)<1e-12)continue;
      const double a=((y[1]-y[2])*(px-x[2])+(x[2]-x[1])*(py-y[2]))/denominator;
      const double b=((y[2]-y[0])*(px-x[2])+(x[0]-x[2])*(py-y[2]))/denominator;
      const double c=1-a-b;if(a<-.000001 || b<-.000001 || c<-.000001)continue;
      const double weight[]{a/oracle[i].clip[3],b/oracle[i+1].clip[3],c/oracle[i+2].clip[3]};
      const double divisor=weight[0]+weight[1]+weight[2];std::array<float,2> uv{};
      for(int channel=0;channel<2;++channel)uv[channel]=float((weight[0]*oracle[i].uv[channel]+
          weight[1]*oracle[i+1].uv[channel]+weight[2]*oracle[i+2].uv[channel])/divisor);
      return uv;
    }
    assert(false && "reference sample outside captured triangle mesh");return std::array<float,2>{};
  };
  plume::RenderGraphicsPipelineDesc p;p.pipelineLayout=layout.get();p.vertexShader=vs.get();p.pixelShader=ps.get();
  p.primitiveTopology=plume::RenderPrimitiveTopology::TRIANGLE_LIST;p.renderTargetCount=1;
  p.renderTargetFormat[0]=plume::RenderFormat::R32G32B32A32_FLOAT;p.renderTargetBlend[0]=plume::RenderBlendDesc::Copy();
  p.cullMode=plume::RenderCullMode::NONE;p.fillMode=plume::RenderFillMode::SOLID;
  if(warp_geometry){p.inputSlots=&warp_slot;p.inputSlotsCount=1;p.inputElements=warp_elements;p.inputElementsCount=2;}
  const plume::RenderInputElement original_position{"POSITION",0,0,plume::RenderFormat::R32G32B32_FLOAT,0,0};
  if(original_warp){p.inputElements=&original_position;p.inputElementsCount=1;}
  auto pipeline=device->createGraphicsPipeline(p);assert(pipeline && static_cast<plume::D3D12GraphicsPipeline*>(pipeline.get())->d3d);
  auto color=Texture(render.width,render.height,plume::RenderFormat::R32G32B32A32_FLOAT);
  auto color_view=color->createTextureView(plume::RenderTextureViewDesc::Texture2D(plume::RenderFormat::R32G32B32A32_FLOAT));
  const auto color_descriptor=HostDevice::RegisterTexture(color.get(),color_view.get());
  auto depth=Texture(render.width,render.height,plume::RenderFormat::R32_FLOAT);
  auto copied=Texture(1920,1080,plume::RenderFormat::R32G32B32A32_FLOAT);
  const plume::RenderTexture* source_attachments[]{color.get(),depth.get()};
  auto source_fb=device->createFramebuffer(plume::RenderFramebufferDesc(source_attachments,2));
  const plume::RenderTexture* target_attachment[]{copied.get()};
  auto target_fb=device->createFramebuffer(plume::RenderFramebufferDesc(target_attachment,1));
  bool marker_present=false;
  std::array<float,2> raw_warp_sample_shift{};
  const auto submit=[&]{commands->end();queue->executeCommandLists(commands,fence.get());queue->waitForCommandFence(fence.get());retired.clear();};
  const auto readback=[&] {
    auto* raw=static_cast<plume::D3D12Texture*>(copied.get())->d3d;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};UINT64 bytes;auto desc=raw->GetDesc();
    static_cast<plume::D3D12Device*>(device)->d3d->GetCopyableFootprints(&desc,0,1,0,&footprint,nullptr,nullptr,&bytes);
    auto buffer=device->createBuffer(plume::RenderBufferDesc::ReadbackBuffer(bytes));
    commands->begin();commands->barriers(plume::RenderBarrierStage::COPY,{copied.get(),plume::RenderTextureLayout::COPY_SOURCE});
    D3D12_TEXTURE_COPY_LOCATION src{};src.pResource=raw;src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION dst{};dst.pResource=static_cast<plume::D3D12Buffer*>(buffer.get())->d3d;
    dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;dst.PlacedFootprint=footprint;
    static_cast<plume::D3D12CommandList*>(commands)->d3d->CopyTextureRegion(&dst,0,0,0,&src,nullptr);submit();
    auto* data=static_cast<const uint8_t*>(buffer->map());assert(data);float max_error=0;
    for(uint32_t y=16;y<1064;y+=73)for(uint32_t x=16;x<1904;x+=89){
      auto* pixel=reinterpret_cast<const float*>(data+size_t(y)*footprint.Footprint.RowPitch+x*16);
      const float expected[]{.125f,.25f,.5f};
      for(int c=0;c<3;++c){assert(std::isfinite(pixel[c]));
        const auto uv=source_uv((x+.5)/1920,(y+.5)/1080);
        const float reference=patterned?scene(uv[0]-raw_warp_sample_shift[0],uv[1]-raw_warp_sample_shift[1],c):expected[c];
        max_error=std::max(max_error,std::abs(pixel[c]-reference));}
    }
    if(marker_present)for(uint32_t py=0;py<1080;++py){
      auto* pixel=reinterpret_cast<const float*>(data+size_t(py)*footprint.Footprint.RowPitch+4*16);
      assert(pixel[0]==1 && pixel[1]==0 && pixel[2]==1 && pixel[3]==1);
    }
    buffer->unmap();return max_error;
  };
  float max_error=0;
  for(uint64_t frame=0;frame<24;++frame){
    if(toggle_mode)enabled=(frame/4)%2!=0;
    if(switch_presets && frame && frame%4==0){
      const Fsr3Size sizes[]{{1920,1080},{1280,720},{1129,635},{960,540},{640,360},{1920,1080}};
      render=sizes[frame/4];
      // Previous work is already fenced. Change the scene resources without
      // resetting the production presenter so its first-frame resize path runs.
      source_fb.reset();color_view.reset();color.reset();depth.reset();
      color=Texture(render.width,render.height,plume::RenderFormat::R32G32B32A32_FLOAT);
      color_view=color->createTextureView(plume::RenderTextureViewDesc::Texture2D(plume::RenderFormat::R32G32B32A32_FLOAT));
      descriptors->setTexture(color_descriptor,color.get(),plume::RenderTextureLayout::SHADER_READ,color_view.get());
      depth=Texture(render.width,render.height,plume::RenderFormat::R32_FLOAT);
      const plume::RenderTexture* attachments[]{color.get(),depth.get()};
      source_fb=device->createFramebuffer(plume::RenderFramebufferDesc(attachments,2));assert(source_fb);
      printf("preset changed before frame=%llu to %ux%u; presenter retained\n",static_cast<unsigned long long>(frame),render.width,render.height);
    }
    marker_present=false;
    commands->begin();
    const TemporalCamera camera{.1f,1000.f,1.f,frame>=16};float x,y;
    if(late_camera) {
      assert(PrepareTemporalDraw(commands,frame,render.width,render.height,{},x,y));
      assert(x!=0 || y!=0);
    }
    assert(PrepareTemporalDraw(commands,frame,render.width,render.height,camera,x,y)==enabled);
    float x2,y2;assert(PrepareTemporalDraw(commands,frame,render.width,render.height,camera,x2,y2)==enabled);
    assert(x==x2&&y==y2);
    commands->barriers(plume::RenderBarrierStage::GRAPHICS,
      std::vector<plume::RenderTextureBarrier>{{color.get(),plume::RenderTextureLayout::COLOR_WRITE},{depth.get(),plume::RenderTextureLayout::COLOR_WRITE}});
    commands->setFramebuffer(source_fb.get());commands->clearColor(0,plume::RenderColor(.125f,.25f,.5f,1));commands->clearColor(1,plume::RenderColor(.5f,0,0,0));
    if(patterned) {
      // Raster-equivalent static detail sampled at the actual jittered pixel
      // centers. Flat clears cannot expose a jitter sign/phase mismatch.
      auto* raw=static_cast<plume::D3D12Texture*>(color.get())->d3d;
      auto desc=raw->GetDesc();D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};UINT64 bytes;
      static_cast<plume::D3D12Device*>(device)->d3d->GetCopyableFootprints(&desc,0,1,0,&fp,nullptr,nullptr,&bytes);
      std::shared_ptr<plume::RenderBuffer> upload=device->createBuffer(plume::RenderBufferDesc::UploadBuffer(bytes));
      auto* mapped=static_cast<uint8_t*>(upload->map());assert(mapped);
      const float sign=invert_jitter?-1.f:1.f;
      for(uint32_t py=0;py<render.height;++py)for(uint32_t px=0;px<render.width;++px) {
        auto* pixel=reinterpret_cast<float*>(mapped+size_t(py)*fp.Footprint.RowPitch+px*16);
        for(int c=0;c<3;++c)pixel[c]=scene((px+.5f-sign*x)/render.width,(py+.5f-sign*y)/render.height,c);
        pixel[3]=1;
      }
      upload->unmap();commands->setFramebuffer(nullptr);
      commands->barriers(plume::RenderBarrierStage::COPY,{color.get(),plume::RenderTextureLayout::COPY_DEST});
      D3D12_TEXTURE_COPY_LOCATION src{};src.pResource=static_cast<plume::D3D12Buffer*>(upload.get())->d3d;
      src.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;src.PlacedFootprint=fp;
      D3D12_TEXTURE_COPY_LOCATION dst{};dst.pResource=raw;dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
      static_cast<plume::D3D12CommandList*>(commands)->d3d->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
      retired.push_back(std::move(upload));
    }
    if(enabled) {
    auto* motion_fb=TemporalFramebuffer(nullptr);assert(motion_fb);commands->setFramebuffer(motion_fb);
    commands->clearColor(0,plume::RenderColor(0,0,0,0));commands->clearColor(1,plume::RenderColor(frame<8?1.f:0.f,0,0,0));
    if(frame==12)RejectTemporalMotion(commands);
    }
    sharpening=frame<20;sharpness=frame>=8?.6f:.2f;
    TextureResourceView input;input.texture=color.get();input.width=render.width;input.height=render.height;input.descriptor_index=color_descriptor;
    const bool missing_depth=force_fallback || frame==9 || frame==19;
    auto output=PresentTemporalFrame(input,{depth,render.width,render.height,missing_depth?~uint64_t{0}:frame},frame);
    const auto offset=TemporalPresentationSampleOffset();
    if(missing_depth || !enabled){assert(output.texture==input.texture);assert(offset[0]==x/render.width && offset[1]==y/render.height);}
    else {assert(output.texture && output.texture!=input.texture && output.width==1920 && output.height==1080);assert(offset[0]==0 && offset[1]==0);}
    if(switch_presets && frame%4==0 && enabled){
      assert(!missing_depth && output.texture!=input.texture);
      printf("first frame=%llu at %ux%u reconstructed immediately\n",static_cast<unsigned long long>(frame),render.width,render.height);
    }
    // Original warp PS has no spatial-copy offset parameter. On a failed FSR
    // attempt this fixture checks its raw jittered-source sampling. Production
    // keeps the low-resolution target and compensates at final presentation;
    // that final fallback copy is covered by the separate copy-PS fixture.
    raw_warp_sample_shift=original_warp && missing_depth?offset:std::array<float,2>{};
    if(composition && !missing_depth){
      assert(!TemporalSceneComposed(frame));assert(!CompleteTemporalComposition(frame+1));
      assert(CompleteTemporalComposition(frame));assert(TemporalSceneComposed(frame));
      float ui_x=-9,ui_y=-9;
      assert(!PrepareTemporalDraw(commands,frame,render.width,render.height,TemporalCamera{.2f,500,.75f,false},ui_x,ui_y));
      assert(ui_x==0 && ui_y==0 && TemporalFramebuffer(nullptr)==nullptr);
    }
    // Exercise the actual external-dispatch -> Plume descriptor/state recovery.
    commands->barriers(plume::RenderBarrierStage::GRAPHICS,
      std::vector<plume::RenderTextureBarrier>{{output.texture,plume::RenderTextureLayout::SHADER_READ},{copied.get(),plume::RenderTextureLayout::COLOR_WRITE}});
    commands->setFramebuffer(target_fb.get());commands->setGraphicsPipelineLayout(layout.get());commands->setGraphicsDescriptorSet(table.get(),0);
    if(original_warp){
      commands->setGraphicsDescriptorSet(table.get(),1);commands->setGraphicsDescriptorSet(table.get(),2);
      // Descriptor relocation only; retain captured shader/transform/gain data.
      std::memcpy(original_constants[2].data(),&output.descriptor_index,4);
      const uint32_t sampler_zero=0;std::memcpy(original_constants[2].data()+384,&sampler_zero,4);
      for(uint32_t i=0;i<3;++i){
        std::memcpy(original_cb[i]->map(),original_constants[i].data(),original_constants[i].size());original_cb[i]->unmap();
        commands->setGraphicsRootDescriptor(original_cb[i]->at(0),i);
      }
    }
    commands->setGraphicsDescriptorSet(sampler_table.get(),3);
    commands->setPipeline(pipeline.get());commands->setViewports(plume::RenderViewport(0,0,1920,1080));commands->setScissors(plume::RenderRect(0,0,1920,1080));
    const uint32_t indices[]{output.descriptor_index,std::bit_cast<uint32_t>(offset[0]),std::bit_cast<uint32_t>(1.f),std::bit_cast<uint32_t>(offset[1])};commands->setGraphicsPushConstants(0,indices);
    if(warp_geometry){const plume::RenderVertexBufferView view(warp_buffer->at(0),uint32_t(warp.size()*(original_warp?36:sizeof(WarpVertex))));
      commands->setVertexBuffers(0,&view,1,&warp_slot);}
    commands->drawInstanced(warp_geometry?uint32_t(warp.size()):3,1,0,0);
    if(composition && !missing_depth){
      // Native-resolution one-pixel UI follows reconstruction and must not be
      // filtered or reconstructed a second time during final presentation.
      const plume::RenderRect marker(4,0,5,1080);
      commands->clearColor(0,plume::RenderColor(1,0,1,1),&marker,1);marker_present=true;
      TextureResourceView final;final.texture=copied.get();final.width=1920;final.height=1080;
      assert(PresentTemporalFrame(final,{},frame).texture==copied.get());
      assert(std::strcmp(NativeTemporalPresentationStatus(),"FSR 3 temporal reconstruction")==0);
      assert((TemporalPresentationSampleOffset()==std::array<float,2>{0,0}));
    }
    EndTemporalFrame(frame!=10);submit();const float frame_error=readback();max_error=std::max(max_error,frame_error);
    if(original_warp && std::getenv("LEGO_TEST_FRAME_ERRORS"))
      printf("frame=%llu missing_depth=%u jitter=(%g,%g) error=%g\n",static_cast<unsigned long long>(frame),missing_depth,x,y,frame_error);
  }
  // Camera discovery can be late; all draws keep the first draw's jitter.
  commands->begin();float x,y;assert(PrepareTemporalDraw(commands,24,render.width,render.height,{},x,y));assert(x!=0||y!=0);
  const auto first_jitter=std::array{x,y};
  PrepareTemporalDraw(commands,24,render.width,render.height,TemporalCamera{.1f,1000,1,true},x,y);
  assert((std::array{x,y}==first_jitter));
  TextureResourceView input;input.texture=color.get();input.width=render.width;input.height=render.height;
  assert(PresentTemporalFrame(input,{depth,render.width,render.height,24},24).texture!=color.get());
  assert((TemporalPresentationSampleOffset()==std::array<float,2>{0,0}));
  if(composition){
    assert(CompleteTemporalComposition(24));FailTemporalComposition(24);
    assert(!TemporalSceneComposed(24));assert(PresentTemporalFrame(input,{depth,render.width,render.height,24},24).texture==color.get());
  }
  EndTemporalFrame(true);submit();
  // Ownership filtering happens in draw.cpp. If two authorized scene cameras
  // really conflict, the presenter must still reject the frame, not silently
  // choose one projection for unrelated depth.
  commands->begin();
  assert(PrepareTemporalDraw(commands,25,render.width,render.height,TemporalCamera{.05f,1000,.66f,false},x,y));
  if(composition)assert(!CompleteTemporalComposition(25)); // Stale successful status cannot authorize a new frame.
  assert(!PrepareTemporalDraw(commands,25,render.width,render.height,TemporalCamera{.2f,500,.75f,false},x,y));
  assert(PresentTemporalFrame(input,{depth,render.width,render.height,25},25).texture==color.get());
  EndTemporalFrame(false);submit();ResetTemporalPresentation();
  printf("production presentation %ux%u -> 1920x1080, 24 frames, patterned=%d inverted_jitter=%d error %.6f; no game or frame generation\n",render.width,render.height,patterned,invert_jitter,max_error);
  if(warp_geometry)printf("captured warp geometry:1536 vertices, independent perspective-interpolated reference\n");
  if(original_warp)printf("original executed VS/PS, raw IA and captured constants; descriptor indices relocated; controlled scene/depth/motion\n");
  if(switch_presets)printf("six preset phases in one renderer; each first frame reconstructed and pixel-checked\n");
  // A spatial fallback has no temporal settling: require approximately one
  // 8-bit code value of error on this smooth, band-limited reference.
  return max_error < (force_fallback ? .004f : .01f) ? 0 : 24;
}
