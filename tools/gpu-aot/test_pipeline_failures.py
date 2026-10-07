"""Exercise native pipeline bootstrap and resolve retries with real function bodies."""
import argparse
from pathlib import Path
import subprocess


def function(source, signature):
    start = source.index(signature)
    end = source.index("{", start) + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    parser.add_argument("--compiler", default="clang++")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    production = (root / "rexlego/src/gpu_native/device.cpp").read_text()
    bodies = "\n".join(function(production, signature) for signature in (
        "bool CreatePipelineLayout(", "bool CreatePresentPipeline(",
        "bool HostDevice::ResolveHdrColor(", "bool HostDevice::TransferDepthAlias(",
    ))
    harness = r'''
#include <array>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <memory>
#include <mutex>
#include <vector>
using u32=std::uint32_t;using i32=std::int32_t;
#define REXLOG_INFO(...) ((void)0)
#define REXLOG_ERROR(...) ((void)0)
enum class Failure {None,NullWrapper,NullHandle};
namespace plume {
enum class RenderShaderFormat {DXIL};
enum class RenderComparisonFunction {ALWAYS};
enum class RenderPrimitiveTopology {TRIANGLE_LIST};
enum class RenderCullMode {NONE};
enum class RenderFillMode {SOLID};
enum class RenderRootDescriptorType {CONSTANT_BUFFER};
enum class RenderShaderStageFlag {PIXEL};
enum class RenderFilter {LINEAR};
enum class RenderMipmapMode {LINEAR};
enum class RenderTextureAddressMode {CLAMP};
enum class RenderFormat {UNKNOWN,B8G8R8A8_UNORM,R16G16B16A16_UNORM,
 R32G32B32A32_FLOAT,R16G16_UNORM,R32_FLOAT,D32_FLOAT_S8_UINT,R8G8B8A8_UNORM};
enum class RenderTextureLayout {SHADER_READ,COLOR_WRITE,DEPTH_WRITE};
enum class RenderBarrierStage {GRAPHICS};
struct RenderPipelineLayout {virtual ~RenderPipelineLayout()=default;};
struct D3D12PipelineLayout:RenderPipelineLayout {
 int handle=1;int* rootSignature=nullptr;
 explicit D3D12PipelineLayout(bool valid){if(valid)rootSignature=&handle;}
};
struct RenderPipeline {virtual ~RenderPipeline()=default;};
struct D3D12GraphicsPipeline:RenderPipeline {
 static inline int live=0;int handle=1;int* d3d=nullptr;
 explicit D3D12GraphicsPipeline(bool valid){++live;if(valid)d3d=&handle;}
 ~D3D12GraphicsPipeline(){--live;}
};
struct RenderShader {};
struct RenderSampler {};
struct RenderTexture {};
struct RenderFramebuffer {};
struct RenderSamplerDesc {
 RenderFilter minFilter{},magFilter{};RenderMipmapMode mipmapMode{};
 RenderTextureAddressMode addressU{},addressV{},addressW{};
};
struct RenderBlendDesc {static RenderBlendDesc Copy(){return {};}};
struct RenderGraphicsPipelineDesc {
 RenderPipelineLayout* pipelineLayout=nullptr;RenderShader* vertexShader=nullptr;
 RenderShader* pixelShader=nullptr;RenderComparisonFunction depthFunction{};
 bool depthEnabled=false,depthWriteEnabled=false;RenderPrimitiveTopology primitiveTopology{};
 RenderCullMode cullMode{};RenderFillMode fillMode{};u32 renderTargetCount=0;
 std::array<RenderFormat,4> renderTargetFormat{};std::array<RenderBlendDesc,4> renderTargetBlend{};
 RenderFormat depthTargetFormat{};
};
struct RenderFramebufferDesc {
 const RenderTexture** colorAttachments=nullptr;u32 colorAttachmentsCount=0;
 RenderTexture* depthAttachment=nullptr;
};
struct RenderTextureBarrier {
 RenderTextureBarrier(RenderTexture*,RenderTextureLayout){}
};
struct RenderViewport {RenderViewport(float,float,float,float){}};
struct RenderRect {RenderRect(u32,u32,u32,u32){}};
struct RenderDescriptorSet {
 int sampler_sets=0;
 void setSampler(u32,RenderSampler* sampler){assert(sampler);++sampler_sets;}
};
struct Device {
 Failure layout_failure=Failure::None,pipeline_failure=Failure::None;
 int layouts=0,pipelines=0,shaders=0,samplers=0,framebuffers=0,fail_shader_at=0;
 std::unique_ptr<RenderPipelineLayout> createPipelineLayout(){
  ++layouts;if(layout_failure==Failure::NullWrapper)return {};
  return std::make_unique<D3D12PipelineLayout>(layout_failure!=Failure::NullHandle);
 }
 std::unique_ptr<RenderPipeline> createGraphicsPipeline(const RenderGraphicsPipelineDesc& desc){
  assert(desc.pipelineLayout&&static_cast<D3D12PipelineLayout*>(desc.pipelineLayout)->rootSignature);
  assert(desc.vertexShader&&desc.pixelShader);++pipelines;
  if(pipeline_failure==Failure::NullWrapper)return {};
  return std::make_unique<D3D12GraphicsPipeline>(pipeline_failure!=Failure::NullHandle);
 }
 auto createShader(const void*,std::size_t,const char*,RenderShaderFormat){
  ++shaders;return shaders==fail_shader_at?std::unique_ptr<RenderShader>{}:
   std::make_unique<RenderShader>();
 }
 auto createSampler(const RenderSamplerDesc&){++samplers;return std::make_unique<RenderSampler>();}
 auto createFramebuffer(const RenderFramebufferDesc&){++framebuffers;return std::make_unique<RenderFramebuffer>();}
};
struct RenderDescriptorSetBuilder {
 void begin(){}void addTexture(u32,u32){}void addSampler(u32,u32){}void end(bool,u32){}
 auto create(Device*){return std::make_unique<RenderDescriptorSet>();}
};
struct RenderPipelineLayoutBuilder {
 void begin(bool,bool){}void addDescriptorSet(const RenderDescriptorSetBuilder&){}
 void addRootDescriptor(u32,u32,RenderRootDescriptorType){}
 void addPushConstant(u32,u32,u32,RenderShaderStageFlag){}void end(){}
 auto create(Device* device){return device->createPipelineLayout();}
};
struct Commands {
 int mutations=0,draws=0,pipeline_binds=0;
 void barriers(RenderBarrierStage,const RenderTextureBarrier*,u32){++mutations;}
 void barriers(RenderBarrierStage,const RenderTextureBarrier&){++mutations;}
 void setFramebuffer(RenderFramebuffer*){++mutations;}
 void setViewports(RenderViewport){++mutations;}void setScissors(RenderRect){++mutations;}
 void setGraphicsPipelineLayout(RenderPipelineLayout* layout){
  assert(layout&&static_cast<D3D12PipelineLayout*>(layout)->rootSignature);++mutations;
 }
 void setGraphicsDescriptorSet(RenderDescriptorSet*,u32){++mutations;}
 void setPipeline(RenderPipeline* pipeline){
  assert(pipeline&&static_cast<D3D12GraphicsPipeline*>(pipeline)->d3d);
  ++pipeline_binds;++mutations;
 }
 void setGraphicsPushConstants(u32,const void*){++mutations;}
 void drawInstanced(u32,u32,u32,u32){++draws;++mutations;}
};
}
constexpr u32 kBindlessTextureCount=65536,kBindlessSamplerCount=1024,kFirstTextureSlot=3;
constexpr unsigned char g_copy_vs_dxil[]{1},g_copy_color_ps_dxil[]{2},
 g_resolve_color_ps_dxil[]{3},g_depth_pack_ps_dxil[]{4},g_depth_restore_ps_dxil[]{5};
struct ResolveRegion {u32 x,y,width,height,left,top;};
enum class ColorResolveDestination {Unorm16,Float32,Unorm16x2,kDepthFloat32};
struct State {
 std::unique_ptr<plume::Device> device=std::make_unique<plume::Device>();
 std::unique_ptr<plume::RenderPipelineLayout> pipeline_layout;
 std::unique_ptr<plume::RenderDescriptorSet> texture_descriptors,sampler_descriptors;
 std::unique_ptr<plume::RenderSampler> default_sampler;
 std::unique_ptr<plume::RenderShader> copy_vertex_shader,copy_pixel_shader,resolve_pixel_shader;
 std::unique_ptr<plume::RenderPipeline> copy_pipeline;
 std::array<std::unique_ptr<plume::RenderPipeline>,4> resolve_pipelines;
 std::array<std::unique_ptr<plume::RenderShader>,2> depth_alias_shaders;
 std::array<std::unique_ptr<plume::RenderPipeline>,2> depth_alias_pipelines;
 std::vector<bool> texture_slots;
};
std::unique_ptr<State> g_state;
std::mutex g_mutex;
plume::Commands commands;
int retired=0;
struct HostDevice {
 static auto LockRecording(){static std::recursive_mutex mutex;return std::unique_lock(mutex);}
 static plume::Commands* BeginFrameCommands(){return &commands;}
 static void RetireResource(std::shared_ptr<void> resource){assert(resource);++retired;}
 static bool ResolveHdrColor(plume::RenderTexture*,u32,plume::RenderTexture*,u32,u32,float,
  ColorResolveDestination,bool,const ResolveRegion*);
 static bool TransferDepthAlias(plume::RenderTexture*,u32,plume::RenderTexture*,u32,u32,bool);
};
'''
    tests = r'''
void Prepare(){
 g_state=std::make_unique<State>();commands={};retired=0;
 assert(CreatePipelineLayout(*g_state));assert(CreatePresentPipeline(*g_state));
}
int main(){
 for(auto failure:{Failure::NullWrapper,Failure::NullHandle}){
  State state;state.device->layout_failure=failure;
  assert(!CreatePipelineLayout(state));assert(!state.pipeline_layout);
  assert(state.device->samplers==0&&state.device->pipelines==0);
  state.device->layout_failure=Failure::None;
  assert(CreatePipelineLayout(state));assert(state.default_sampler);
  state.device->pipeline_failure=failure;
  assert(!CreatePresentPipeline(state));assert(!state.copy_pipeline);
  assert(plume::D3D12GraphicsPipeline::live==0);
  state.device->pipeline_failure=Failure::None;
  assert(CreatePresentPipeline(state));assert(state.copy_pipeline);
 }
 assert(plume::D3D12GraphicsPipeline::live==0);
 for(int shader_failure:{1,2}){
  State state;assert(CreatePipelineLayout(state));state.device->fail_shader_at=shader_failure;
  assert(!CreatePresentPipeline(state));assert(!state.copy_pipeline&&state.device->pipelines==0);
 }
 plume::RenderTexture source,destination;
 for(auto failure:{Failure::NullWrapper,Failure::NullHandle}){
  for(u32 format=0;format<4;++format){
   Prepare();auto& state=*g_state;state.device->pipeline_failure=failure;
   const auto call=[&]{return HostDevice::ResolveHdrColor(&source,1,&destination,128,64,1,
    static_cast<ColorResolveDestination>(format),true,nullptr);};
   for(int attempt=0;attempt<2;++attempt){
    assert(!call());assert(!state.resolve_pipelines[format]);
    assert(commands.mutations==0&&commands.draws==0&&retired==0);
    assert(state.device->pipelines==2+attempt&&state.device->framebuffers==0);
    assert(plume::D3D12GraphicsPipeline::live==1); // Only the healthy present PSO.
   }
   state.device->pipeline_failure=Failure::None;
   assert(call());assert(commands.draws==1&&commands.pipeline_binds==1&&retired==1);
   assert(state.device->pipelines==4&&plume::D3D12GraphicsPipeline::live==2);
   assert(call());assert(commands.draws==2&&state.device->pipelines==4&&retired==2);
   g_state.reset();assert(plume::D3D12GraphicsPipeline::live==0);
  }
  for(bool restore:{false,true}){
   Prepare();auto& state=*g_state;state.device->pipeline_failure=failure;
   const auto call=[&]{return HostDevice::TransferDepthAlias(&source,1,&destination,128,64,restore);};
   for(int attempt=0;attempt<2;++attempt){
    assert(!call());assert(!state.depth_alias_pipelines[restore]);
    assert(commands.mutations==0&&commands.draws==0&&retired==0);
    assert(state.device->pipelines==2+attempt&&state.device->framebuffers==0);
    assert(plume::D3D12GraphicsPipeline::live==1);
   }
   state.device->pipeline_failure=Failure::None;
   assert(call());assert(commands.draws==1&&commands.pipeline_binds==1&&retired==1);
   assert(state.device->pipelines==4&&plume::D3D12GraphicsPipeline::live==2);
   assert(call());assert(commands.draws==2&&state.device->pipelines==4&&retired==2);
   g_state.reset();assert(plume::D3D12GraphicsPipeline::live==0);
  }
 }
 std::cout<<"PASS: real pipeline bootstrap bodies reject failed handles; color/depth resolve failures retry without binding or drawing.\n";
}
'''
    args.output.mkdir(parents=True, exist_ok=True)
    source = args.output.resolve() / "pipeline-failures.cpp"
    exe = args.output.resolve() / "pipeline-failures.exe"
    source.write_text(harness + bodies + tests)
    subprocess.run([args.compiler, "-std=c++20", "-UNDEBUG", str(source), "-o", str(exe)],
                   check=True, timeout=45)
    subprocess.run([str(exe)], check=True, timeout=10)


if __name__ == "__main__":
    main()
