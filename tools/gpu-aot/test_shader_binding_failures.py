"""Reject failed nonnull shader binds before cached PSOs, using production bodies."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess


def function(source, signature):
    start = source.index(signature)
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path)
    parser.add_argument('--compiler', default='clang++')
    parser.add_argument('--verify-regression', action='store_true',
                        help='also prove removing the production guard fails the cache-hit case')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    shaders = (root / 'rexlego/src/gpu_native/shaders.cpp').read_text()
    draw = (root / 'rexlego/src/gpu_native/draw.cpp').read_text()
    hooks = (root / 'rexlego/src/gpu_native/hooks_shader.cpp').read_text()
    header = (root / 'rexlego/src/gpu_native/shaders.h').read_text()
    address_declaration_begin = header.index('u32 BoundShaderAddress(')
    address_declaration = header[address_declaration_begin:
                                 header.index(';', address_declaration_begin) + 1]

    # Keep the registry, binding, DXIL resolution and reset implementations
    # unchanged. Only guest adoption and the host driver are test boundaries.
    registry = shaders[shaders.index('struct ShaderResource {'):
                       shaders.index('std::shared_ptr<ShaderResource> FindResource(')]
    binding_bodies = '\n'.join(function(shaders, signature) for signature in (
        'bool BindShader(', 'plume::RenderShader* ResolveBoundShader(',
        'u32 BoundShaderAddress(', 'u64 BoundShaderHash(',
        'void ResetShaderResources(',
    ))
    setter_bodies = '\n'.join(function(hooks, signature) for signature in (
        'void SetPixelShaderHook(', 'void SetVertexShaderHook(',
    ))
    key_types = draw[draw.index('struct PipelineKey {'):draw.index('u32 g_unsupported_draw_logs')]
    # The unchanged cache lookup and shader-resolution gates are the behavior
    # under test. Pipeline creation below them is supplied by the fake driver.
    pipeline_begin = draw.index('plume::RenderPipeline* GetPipeline(')
    pipeline_end = draw.index('  std::array<plume::RenderInputSlot,', pipeline_begin)
    pipeline_prefix = draw[pipeline_begin:pipeline_end]
    key_begin = draw.index('  PipelineKey key;', draw.index('bool DispatchDraw('))
    key_end = draw.index('  const auto* state_bytes', key_begin)
    key_construction = draw[key_begin:key_end]
    guards = {stage: function(key_construction, 'if (' + stage + '_binding_failed)')
              for stage in ('pixel', 'vertex')}
    index_branch_begin = draw.index('  } else if (indexed) {')
    index_branch = function(draw[index_branch_begin:], 'else if (indexed)')[len('else '):]
    nonindex_draw = draw.index('    commands->drawInstanced(count, 1, use_windows ? 0 : start, 0);')
    nonindex_begin = draw.rfind('    QueryDrawScope query_scope(commands);', 0, nonindex_draw)
    assert nonindex_begin >= 0
    nonindex_end = draw.index('\n  }', nonindex_begin)
    nonindex_submission = draw[nonindex_begin:nonindex_end]
    draw_hooks = '\n'.join(function(draw, signature) for signature in (
        'u32 DrawVerticesHook(', 'u32 DrawIndexedVerticesHook(',
    ))

    harness = r'''
#include <array>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include "gpu_native/index_draw.h"
using namespace legodimensions::gpu_native;
using u32=uint32_t;using u64=uint64_t;using i32=int32_t;
constexpr u32 kNativeRenderTargets=4,kNativeVertexStreams=16;
#define REXLOG_WARN(...) ((void)0)
#define REXLOG_ERROR(...) ((void)0)
enum class ShaderStage:u32{kVertex=6,kPixel=7};
struct ShaderCacheEntry{u32 specConstantsMask=3;};
struct ShaderBytecode{
 const void* data=nullptr;size_t size=0;
 explicit operator bool()const{return data&&size;}
};
namespace plume {
enum class RenderShaderFormat{DXIL};
enum class RenderFormat{UNKNOWN,R16_UINT,R32_UINT};
enum class RenderPrimitiveTopology{TRIANGLE_LIST};
struct RenderShader{void setName(const char*){}};
struct RenderPipeline{RenderShader* vs;RenderShader* ps;};
}
struct FakeDevice{
 u32 shader_creates=0;
 auto createShader(const void* data,size_t bytes,const char*,plume::RenderShaderFormat){
  assert(data&&bytes);++shader_creates;return std::make_unique<plume::RenderShader>();
 }
} device;
struct HostDevice{
 static inline std::recursive_mutex recording;
 static auto LockRecording(){return std::unique_lock(recording);}
 static auto Device(){return &device;}
};
struct Memory{std::vector<u32> freed;void SystemHeapFree(u32 address){freed.push_back(address);}} memory;
#define REX_KERNEL_MEMORY() (&memory)
std::vector<std::string> events;
template<class... Args>void LongProbeEvent(const char* event,bool,Args&&...){events.emplace_back(event);}
''' + registry + r'''
// A guest address may be re-registered/recycled and become unadoptable.
// These fixtures retain exactly the same address and even a zero hash to
// exercise a cache collision with a formerly valid requested shader.
std::unordered_map<u32,std::shared_ptr<ShaderResource>> adoptable;
u32 adoption_calls=0,dxil_lookups=0;
std::shared_ptr<ShaderResource> AdoptShader(u32 address,ShaderStage){
 ++adoption_calls;auto it=adoptable.find(address);
 return it==adoptable.end()?nullptr:it->second;
}
bool dxil_available=true;
ShaderBytecode FindDxil(u64,u32){
 ++dxil_lookups;static const char bytecode[]="DXIL";
 return dxil_available?ShaderBytecode{bytecode,sizeof(bytecode)}:ShaderBytecode{};
}
''' + 'void PollPrecompiledShaders() {}\n' + address_declaration + '\n' + binding_bodies + r'''
struct D3DDevice{u32 vertex_shader=0,pixel_shader=0,vertex_declaration=0x4000;};
''' + setter_bodies + key_types + r'''
struct VertexDeclarationView{u64 content_hash=0x1234;};
u32 pipeline_creates=0;
''' + pipeline_prefix + r'''
 auto pipeline=std::make_unique<plume::RenderPipeline>();
 pipeline->vs=vs;pipeline->ps=ps;++pipeline_creates;
 auto [it,inserted]=g_pipelines.emplace(key,std::move(pipeline));
 assert(inserted);return it->second.get();
}
struct Buffer{Buffer* at(u32 offset){assert(!offset);return this;}} buffer;
struct BufferResourceView{Buffer* buffer=nullptr;u32 length=0,guest_format=0;};
enum class BufferKind{kIndex};
u32 buffer_resolves=0;
BufferResourceView ResolveBufferResourceView(u32,BufferKind){++buffer_resolves;return {&buffer,256,1};}
BufferResourceView ResolveBufferResourceWindow(u32,BufferKind,u32,u32){assert(false);return {};}
namespace plume {
struct RenderIndexBufferView{
 RenderIndexBufferView(Buffer* b,u32 bytes,RenderFormat){assert(b&&bytes);}
};
}
u32 query_begins=0,query_ends=0,query_failures=0,submitted_draws=0;
struct Recorder{
 u32 index_bindings=0;
 void setIndexBuffer(const plume::RenderIndexBufferView*){++index_bindings;}
 void drawInstanced(u32 count,u32 instances,u32 start,u32 first){
  assert(count==6&&instances==1&&start==5&&!first);++submitted_draws;
 }
 void drawIndexedInstanced(u32 count,u32 instances,u32 start,i32 base,u32 first){
  assert(count==6&&instances==1&&start==5&&base==-7&&!first);++submitted_draws;
 }
} recorder;
struct QueryDrawScope{
 explicit QueryDrawScope(Recorder*){++query_begins;}
 ~QueryDrawScope(){++query_ends;}
};
void FailActiveQueries(){++query_failures;}
bool ConsumeCpuPoolCopyDraw(u32,u32,u32){return false;}
bool DispatchDraw(D3DDevice* device,u32,bool indexed,u32 start,u32 count,i32 base_vertex){
 if(!device||!count)return false;
 struct {u32 vertex_declaration;u32 index_buffer;} bindings{device->vertex_declaration,0x3000};
 const VertexDeclarationView declaration;
''' + key_construction + r'''
 auto* pipeline=GetPipeline(key,declaration,plume::RenderPrimitiveTopology::TRIANGLE_LIST,
                            plume::RenderFormat::UNKNOWN);
 if(!pipeline)return false;
 auto* commands=&recorder;
 const bool use_windows=false;
 const u32 min_index=0,index_offset=0,index_bytes=0;
 ''' + index_branch + r''' else {
''' + nonindex_submission + r'''
 }
 return true;
}
''' + draw_hooks + r'''
std::shared_ptr<ShaderResource> add_shader(u32 address,ShaderStage stage,u64 hash,
                                         bool owned=false){
 static const ShaderCacheEntry entry;
 auto resource=std::make_shared<ShaderResource>();
 resource->guest_address=address;resource->stage=stage;resource->hash=hash;
 resource->cache_entry=&entry;resource->owns_guest_memory=owned;
 g_registry[address]=resource;adoptable[address]=resource;return resource;
}
void reset_counters(){
 query_begins=query_ends=query_failures=submitted_draws=buffer_resolves=0;
 recorder={};events.clear();
}
void rejected(D3DDevice& guest,bool indexed,const char* event="draw_missing_pixel_shader"){
 reset_counters();
 if(indexed)DrawIndexedVerticesHook(&guest,4,u32(-7),5,6);
 else DrawVerticesHook(&guest,4,5,6);
 assert(submitted_draws==0 && "failed nonnull shader binding reused a cached pipeline");
 assert(query_begins==0&&query_ends==0&&query_failures==1);
 assert(!buffer_resolves&&!recorder.index_bindings);
 assert(events.size()==1&&events[0]==event);
}
void accepted(D3DDevice& guest,bool indexed){
 reset_counters();
 if(indexed)DrawIndexedVerticesHook(&guest,4,u32(-7),5,6);
 else DrawVerticesHook(&guest,4,5,6);
 assert(submitted_draws==1&&query_begins==1&&query_ends==1&&!query_failures);
 assert(buffer_resolves==u32(indexed)&&recorder.index_bindings==u32(indexed));
 assert(events.empty());
}
int main(){
 D3DDevice guest;
 auto vs=add_shader(0x1000,ShaderStage::kVertex,0);
 // A zero hash is valid at the registry boundary; addresses/hashes after a
 // failed rebind match the previously cached key if the new guard is removed.
 auto ps=add_shader(0x2000,ShaderStage::kPixel,0);
 SetVertexShaderHook(&guest,0x1000);SetPixelShaderHook(&guest,0x2000);
 assert(guest.vertex_shader==0x1000&&guest.pixel_shader==0x2000);
 accepted(guest,false);accepted(guest,true);
 assert(g_pipelines.size()==1&&pipeline_creates==1);
 assert(adoption_calls==2&&device.shader_creates==2&&dxil_lookups==2);
 const auto* cached=g_pipelines.begin()->second.get();
 const u32 original_pipeline_creates=pipeline_creates,original_dxil_lookups=dxil_lookups;

 adoptable.erase(0x2000);g_registry.erase(0x2000);
 SetPixelShaderHook(&guest,0x2000);
 bool failed=false;
 assert(guest.pixel_shader==0x2000);
 assert(BoundShaderAddress(ShaderStage::kPixel,&failed)==0x2000&&failed);
 assert(!ResolveBoundShader(ShaderStage::kPixel,0));
 rejected(guest,false);rejected(guest,true);
 assert(g_pipelines.size()==1&&g_pipelines.begin()->second.get()==cached);
 assert(pipeline_creates==original_pipeline_creates&&dxil_lookups==original_dxil_lookups);

 // Successful adoption at the same address immediately recovers; the known
 // good cached PSO is still usable, without resolving shader bytecode per draw.
 adoptable[0x2000]=ps;g_registry[0x2000]=ps;
 SetPixelShaderHook(&guest,0x2000);
 assert(BoundShaderAddress(ShaderStage::kPixel,&failed)==0x2000&&!failed);
 const auto successful_adoptions=adoption_calls;
 accepted(guest,false);accepted(guest,true);
 assert(adoption_calls==successful_adoptions&&dxil_lookups==original_dxil_lookups);
 assert(pipeline_creates==original_pipeline_creates);

 // The cache-hit bypass applies equally to a failed vertex rebind. Both
 // stages must remain unavailable even when the former cache key still exists.
 adoptable.erase(0x1000);g_registry.erase(0x1000);
 SetVertexShaderHook(&guest,0x1000);
 assert(guest.vertex_shader==0x1000);
 assert(BoundShaderAddress(ShaderStage::kVertex,&failed)==0x1000&&failed);
 rejected(guest,false,"draw_missing_vertex_shader");
 rejected(guest,true,"draw_missing_vertex_shader");
 assert(g_pipelines.size()==1&&pipeline_creates==original_pipeline_creates);
 adoptable[0x1000]=vs;g_registry[0x1000]=vs;
 SetVertexShaderHook(&guest,0x1000);
 accepted(guest,false);accepted(guest,true);
 assert(pipeline_creates==original_pipeline_creates&&dxil_lookups==original_dxil_lookups);

 // Genuine null requests intentionally create/cache the depth-only PSO.
 SetPixelShaderHook(&guest,0);
 assert(guest.pixel_shader==0&&BoundShaderAddress(ShaderStage::kPixel,&failed)==0&&!failed);
 accepted(guest,false);accepted(guest,true);
 assert(g_pipelines.size()==2&&pipeline_creates==2);
 bool found_null=false;
 for(const auto& [key,pipeline]:g_pipelines){
  if(!key.pixel_shader){assert(pipeline->vs&&!pipeline->ps);found_null=true;}
 }
 assert(found_null);

 // A wrong-stage nonnull adoption is also failure, despite a nonnull wrapper.
 adoptable[0x2000]=vs;SetPixelShaderHook(&guest,0x2000);
 assert(BoundShaderAddress(ShaderStage::kPixel,&failed)==0x2000&&failed);
 rejected(guest,false);
 SetPixelShaderHook(&guest,0);accepted(guest,false);

 // Non-adoption DXIL failures retain a nonnull address and are rejected by
 // the existing GetPipeline guard. They never become a depth-only cache key.
 auto unavailable=add_shader(0x2500,ShaderStage::kPixel,0xBAD);
 SetPixelShaderHook(&guest,0x2500);dxil_available=false;reset_counters();
 DrawVerticesHook(&guest,4,5,6);
 assert(!submitted_draws&&!query_begins&&query_failures==1&&g_pipelines.size()==2);
 dxil_available=true;accepted(guest,false);assert(g_pipelines.size()==3);

 // Reset clears both failure and successful binding records, while retaining
 // the original ownership rule for freeing guest memory.
 add_shader(0x9000,ShaderStage::kPixel,0xDEAD,true);
 adoptable.erase(0x2000);SetPixelShaderHook(&guest,0x2000);
 ResetShaderResources();
 assert(g_registry.empty()&&memory.freed==std::vector<u32>{0x9000});
 assert(!BoundShaderAddress(ShaderStage::kPixel,&failed)&&!failed);
 assert(!BoundShaderAddress(ShaderStage::kVertex)&&!ResolveBoundShader(ShaderStage::kVertex,0));
 g_pipelines.clear();
 SetVertexShaderHook(&guest,0x1000);SetPixelShaderHook(&guest,0);
 accepted(guest,false);
}
'''
    args.output.mkdir(parents=True, exist_ok=True)
    cpp = args.output / 'shader-binding-failures.cpp'
    cpp.write_text(harness)
    executable_suffix = '.exe' if os.name == 'nt' else ''
    exe = args.output / ('shader-binding-failures' + executable_suffix)
    compile_args = [args.compiler, '-std=c++20', '-O2', '-UNDEBUG', '-pthread',
                    '-I' + str(root / 'rexlego/src')]
    subprocess.run(compile_args + [str(cpp), '-o', str(exe)], check=True, timeout=45)
    subprocess.run([str(exe.resolve())], check=True, timeout=10)
    regression = None
    if args.verify_regression:
        for stage, guard in guards.items():
            mutant = args.output / ('shader-binding-without-' + stage + '-guard.cpp')
            mutant.write_text(harness.replace(guard, '', 1))
            mutant_exe = mutant.with_suffix(executable_suffix)
            subprocess.run(compile_args + [str(mutant), '-o', str(mutant_exe)],
                           check=True, timeout=45)
            result = subprocess.run([str(mutant_exe.resolve())], capture_output=True,
                                    text=True, timeout=10)
            assert result.returncode != 0 and (
                'failed nonnull shader binding reused a cached pipeline' in result.stderr)
        regression = 'removing either production stage guard submits its formerly cached failed binding'
    report = {
        'result': 'PASS', 'regression_proof': regression,
        'production_sha256': {
            'shader_binding_bodies': hashlib.sha256(binding_bodies.encode()).hexdigest(),
            'pipeline_cache_and_resolution': hashlib.sha256(pipeline_prefix.encode()).hexdigest(),
            'draw_key_construction': hashlib.sha256(key_construction.encode()).hexdigest(),
        },
        'cases': ['PS and VS nonnull failed rebinds with cached same-address/hash-zero PSO',
                  'ordinary and indexed draws reject without query scopes or index uploads',
                  'same-address recovery reuses cached PSO without per-draw adoption or DXIL',
                  'intentional null pixel request retains depth-only pipeline',
                  'wrong-stage adoption and uncached DXIL lookup failure',
                  'reset clears latest requests and frees owned guest memory only'],
        'boundaries': 'guest adoption, host shader/pipeline construction and driver are fake; '
                      'registry/bind/resolve/reset/hooks, cache gate, key guard and submission are production',
    }
    (args.output / 'verification.json').write_text(json.dumps(report, indent=2) + '\n')
    print('PASS: production shader binding/cache gates; failed nonnull VS/PS reject before '
          'cached PSO, query scopes and draws; null/recovery/reset remain valid')


if __name__ == '__main__':
    main()
