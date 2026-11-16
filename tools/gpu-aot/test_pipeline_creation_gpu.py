"""Exercise both production PSO builders on windowless D3D12, without game code."""
import argparse
import json
import os
from pathlib import Path
import subprocess


def body(text, signature):
    start = text.index(signature)
    end = text.index('{', start) + 1
    depth = 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


p = argparse.ArgumentParser(description=__doc__)
p.add_argument('output', type=Path)
p.add_argument('--plume-lib', required=True, type=Path)
p.add_argument('--dxc', required=True, type=Path)
p.add_argument('--compiler', default='clang-cl')
p.add_argument('--previous-source', type=Path)
a = p.parse_args()
root = Path(__file__).resolve().parents[2]
a.output = a.output.resolve()
a.output.mkdir(parents=True, exist_ok=True)
source = (root / 'rexlego/src/gpu_native/draw.cpp').read_text()
declaration_type = body((root / 'rexlego/src/gpu_native/vertex_declarations.h').read_text(), 'struct VertexDeclarationView {') + ';'
types = source[source.index('struct PipelineKey {'):source.index('bool RetainedDrawPreparationEnabled()')]
support = '\n'.join(body(source, s) for s in (
    'void ApplyCullPipeline(', 'void SetNativePipelineStencilReference('))
builders = '\n'.join(body(source, s) for s in (
    'plume::RenderPipeline* GetPipeline(', 'plume::RenderPipeline* GetTemporalPipeline('))
hlsl = a.output / 'fixture.hlsl'
hlsl.write_text('''float4 vs(uint id:SV_VertexID):SV_Position {
 float2 p=float2((id<<1)&2,id&2);return float4(p*float2(2,-2)+float2(-1,1),0,1);}
float4 ps():SV_Target0{return float4(64.0/255.0,128.0/255.0,192.0/255.0,1);}
''')
for stage in ('vs', 'ps'):
    subprocess.run([str(a.dxc), '-T', stage + '_6_0', '-E', stage,
                    str(hlsl), '-Fo', str(a.output / (stage + '.dxil'))], check=True, timeout=30)
fixture = r'''
#define NOMINMAX
#include <plume_d3d12.h>
#include "gpu_native/pipeline_creation_gate.h"
#include "gpu_native/texture_readback.h"
#include "gpu_native/blend_state.h"
#include "gpu_native/depth_state.h"
#include "gpu_native/stencil_state.h"
#include "gpu_native/cull_state.h"
#include <array>
#include <atomic>
#include <barrier>
#include <bit>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <future>
#include <memory>
#include <mutex>
#include <span>
#include <unordered_map>
#include <vector>
using namespace legodimensions::gpu_native;
using u32=uint32_t;using u64=uint64_t;using i32=int32_t;
constexpr size_t kNativeRenderTargets=4,kNativeVertexStreams=16;
#define REXLOG_INFO(...) ((void)0)
#define REXLOG_WARN(...) ((void)0)
#define REXLOG_ERROR(...) ((void)0)
#define CHECK(c) do{if(!(c)){std::fprintf(stderr,"line %d: %s\n",__LINE__,#c);std::exit(23);}}while(0)
namespace plume {std::unique_ptr<RenderInterface> CreateD3D12Interface();}
''' + declaration_type + '\n' + types + r'''
std::mutex g_pipeline_mutex;
PipelineCreationGate<PipelineKey,PipelineKeyHash> g_pipeline_creation,g_temporal_pipeline_creation;
std::unordered_map<PipelineKey,std::unique_ptr<plume::RenderPipeline>,PipelineKeyHash>g_pipelines,g_temporal_pipelines;
struct Timing{u32 pipeline_creations=0,temporal_pipeline_creations=0;double pipeline_creation_ms=0,temporal_pipeline_creation_ms=0;};
thread_local Timing g_draw_timing;
plume::RenderShader *vs=nullptr,*ps=nullptr;
enum class ShaderStage{kVertex,kPixel};
bool FloatBlendIndicesInput(std::span<const plume::RenderInputElement>){return false;}
bool FindFloatBlendIndicesDxil(auto...){return false;}
bool FindPositionPrecisionDxil(auto...){return false;}
struct Selection{plume::RenderShader* shader=nullptr;const plume::RenderInputElement*elements=nullptr;};
Selection SelectIntegerInputShader(auto...){return {};}
plume::RenderShader* ResolveInstancedConstantsShader(auto...){return vs;}
plume::RenderShader* ResolveFloatBlendIndicesShader(auto...){return vs;}
plume::RenderShader* ResolvePositionPrecisionShader(auto...){return vs;}
plume::RenderShader* ResolveBoundShader(ShaderStage stage,u32){return stage==ShaderStage::kVertex?vs:ps;}
int TemporalCoverageForBlend(auto...){return 0;}
bool TemporalMaterialReactiveEnabled(){return false;}
bool HasTemporalCoverageShader(auto...){return false;}
bool HasIntegerInputShader(auto...){return false;}
bool HasVerifiedTemporalCamera(auto...){return false;}
bool HasTemporalCameraConstants(auto...){return false;}
bool HasTemporalShader(auto...){return false;}
plume::RenderShader* ResolveTemporalShader(u64,bool pixel,u32){return pixel?ps:vs;}
plume::RenderShader* ResolveTemporalCoverageShader(auto...){return ps;}
struct Driver {
 plume::RenderDevice* real=nullptr;
 std::atomic<u32>calls{0},active{0},peak{0};
 std::barrier<>*rendezvous=nullptr;
 std::atomic<bool>fail{false};
 std::unique_ptr<plume::RenderPipeline> createGraphicsPipeline(const plume::RenderGraphicsPipelineDesc& desc){
  ++calls;const auto n=++active;auto before=peak.load();while(before<n&&!peak.compare_exchange_weak(before,n)){}
  if(rendezvous)rendezvous->arrive_and_wait();
  if(fail.exchange(false)){--active;return {};}
  auto result=real->createGraphicsPipeline(desc);--active;return result;
 }
}driver;
plume::RenderPipelineLayout* layout=nullptr;
struct HostDevice {static Driver*Device(){return &driver;}static auto PipelineLayout(){return layout;}};
''' + support + '\n' + builders + r'''
std::vector<char> read(const char*path){std::ifstream f(path,std::ios::binary);return {std::istreambuf_iterator<char>(f),{}};}
int main(){
 auto api=plume::CreateD3D12Interface();auto device=api->createDevice();CHECK(device);driver.real=device.get();
 auto vb=read("vs.dxil"),pb=read("ps.dxil");
 auto vertex=device->createShader(vb.data(),vb.size(),"vs",plume::RenderShaderFormat::DXIL);
 auto pixel=device->createShader(pb.data(),pb.size(),"ps",plume::RenderShaderFormat::DXIL);
 CHECK(vertex&&pixel);vs=vertex.get();ps=pixel.get();
 plume::RenderPipelineLayoutBuilder lb;lb.begin(false,true);lb.end();auto owner=lb.create(device.get());CHECK(owner);layout=owner.get();
 VertexDeclarationView declaration{};
 PipelineKey base;base.vertex_shader=1;base.pixel_shader=2;base.vertex_shader_hash=3;base.pixel_shader_hash=4;
 base.render_target_formats[0]=u32(plume::RenderFormat::R8G8B8A8_UNORM);base.color_write_mask=15;
 const auto topology=plume::RenderPrimitiveTopology::TRIANGLE_LIST;
 const auto unknown=plume::RenderFormat::UNKNOWN;
 // Execute both complete production builders. These calls really enter the
 // D3D12 driver, with a rendezvous proving the global lock is not held there.
 for(bool temporal:{false,true}){
  std::barrier rendezvous(3);driver.rendezvous=&rendezvous;
  std::array<std::future<plume::RenderPipeline*>,3>jobs;
  const auto before=driver.calls.load();
  for(u32 i=0;i<3;++i)jobs[i]=std::async(std::launch::async,[&,i]{auto key=base;key.color_write_mask=1u<<i;
   return temporal?GetTemporalPipeline(key,declaration,topology,unknown):GetPipeline(key,declaration,topology,unknown);});
  for(auto&job:jobs)CHECK(job.get());CHECK(driver.calls==before+3);driver.rendezvous=nullptr;
 }
 CHECK(driver.peak==3);
 // Same-key contention returns one actual driver PSO, including cache hits.
 std::vector<std::future<plume::RenderPipeline*>>jobs;const auto before=driver.calls.load();
 for(u32 i=0;i<24;++i)jobs.push_back(std::async(std::launch::async,[&]{return GetPipeline(base,declaration,topology,unknown);}));
 auto*published=jobs[0].get();CHECK(published);
 for(size_t i=1;i<jobs.size();++i)CHECK(jobs[i].get()==published);CHECK(driver.calls==before+1);
 auto failed=base;failed.clip_disable=1;driver.fail=true;
 CHECK(!GetPipeline(failed,declaration,topology,unknown));CHECK(GetPipeline(failed,declaration,topology,unknown));
 // Render through the published production PSO and check the resulting bytes.
 auto texture=device->createTexture(plume::RenderTextureDesc::ColorTarget(1,1,plume::RenderFormat::R8G8B8A8_UNORM));CHECK(texture);
 const plume::RenderTexture* target=texture.get();plume::RenderFramebufferDesc fd;fd.colorAttachments=&target;fd.colorAttachmentsCount=1;
 auto fb=device->createFramebuffer(fd);auto queue=device->createCommandQueue(plume::RenderCommandListType::DIRECT);
 auto commands=queue->createCommandList();auto fence=device->createCommandFence();commands->begin();
 commands->barriers(plume::RenderBarrierStage::GRAPHICS,plume::RenderTextureBarrier(texture.get(),plume::RenderTextureLayout::COLOR_WRITE));
 commands->setFramebuffer(fb.get());commands->clearColor();commands->setViewports(plume::RenderViewport(0,0,1,1));commands->setScissors(plume::RenderRect(0,0,1,1));
 commands->setGraphicsPipelineLayout(layout);commands->setPipeline(published);commands->drawInstanced(3,1,0,0);
 std::vector<u8> rgba;
 CHECK(ReadbackTextureRgba8(device.get(),commands.get(),texture.get(),1,1,rgba,[&]{commands->end();queue->executeCommandLists(commands.get(),fence.get());queue->waitForCommandFence(fence.get());return true;},[](auto){}));
 CHECK(rgba==std::vector<u8>({64,128,192,255}));
 g_pipelines.clear();g_temporal_pipelines.clear();g_pipeline_creation.ClearAfterDrain();g_temporal_pipeline_creation.ClearAfterDrain();
 std::printf("PASS production normal+temporal builders: driver concurrency=%u, same-key publication, failure retry, rendered RGBA=64,128,192,255\n",driver.peak.load());
}
'''
fixture = fixture.replace('std::vector<u8>', 'std::vector<uint8_t>')


def compile_run(name, code, timeout):
    cpp = a.output / (name + '.cpp')
    exe = cpp.with_suffix('.exe')
    cpp.write_text(code)
    cmd = [a.compiler, str(cpp), '/std:c++20', '/O2', '/MD', '/EHsc', '/DNOMINMAX',
           '/I' + str(root / 'rexlego/src'), '/I' + str(root / 'thirdparty/plume'),
           '/I' + str(root / 'thirdparty/plume/contrib/D3D12MemoryAllocator/include'),
           '/Fo' + str(cpp.with_suffix('.obj')), '/Fe' + str(exe), '/link',
           str(a.plume_lib.resolve()), 'd3d12.lib', 'dxgi.lib', 'dxguid.lib', 'user32.lib']
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=120)
    (a.output / (name + '-compile.log')).write_text(r.stdout + r.stderr)
    r.check_returncode()
    return subprocess.run([str(exe)], cwd=a.output, env=dict(os.environ, PLUME_D3D12_STATE_CACHE='0'),
                          capture_output=True, text=True, timeout=timeout)


r = compile_run('pipeline-creation-gpu', fixture, 45)
(a.output / 'positive.log').write_text(r.stdout + r.stderr)
if r.returncode:
    raise RuntimeError(r.stdout + r.stderr)
print(r.stdout.strip())
r = subprocess.run([str(a.output / 'pipeline-creation-gpu.exe')], cwd=a.output,
                   env=dict(os.environ, PLUME_D3D12_STATE_CACHE='1'),
                   capture_output=True, text=True, timeout=45)
(a.output / 'state-cache-on.log').write_text(r.stdout + r.stderr)
if r.returncode:
    raise RuntimeError(r.stdout + r.stderr)
print('PASS with Plume command state cache enabled')
negative = None
if a.previous_source:
    previous = a.previous_source.read_text()
    old = '\n'.join(body(previous, s) for s in (
        'plume::RenderPipeline* GetPipeline(', 'plume::RenderPipeline* GetTemporalPipeline('))
    try:
        compile_run('previous-source', fixture.replace(builders, old), 5)
    except subprocess.TimeoutExpired:
        negative = 'previous global lock cannot reach concurrent driver rendezvous'
    else:
        raise RuntimeError('Previous implementation escaped the concurrency regression')
    print('PASS negative control: ' + negative)
(a.output / 'verification.json').write_text(json.dumps(dict(game_launched=False,
    window_opened=False, production_builders=True, actual_d3d12=True,
    concurrent_driver_calls=3, state_cache_modes=[0, 1],
    rendered_rgba=[64, 128, 192, 255], negative=negative), indent=2))
