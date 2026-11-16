"""Driver-check the production motion PSO builder against saved shader pairs."""
import argparse
import json
from pathlib import Path
import re
import subprocess

def body(source, signature):
    begin=source.index(signature);end=source.index('{',begin)+1;depth=1
    while depth:
        depth+=(source[end]=='{')-(source[end]=='}');end+=1
    return source[begin:end]

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('output',type=Path);p.add_argument('--bank',type=Path,required=True)
p.add_argument('--plume-lib',type=Path,required=True)
p.add_argument('--material-bank',type=Path,required=True)
p.add_argument('--capture',type=Path,action='append',required=True)
p.add_argument('--compiler',default='clang-cl')
p.add_argument('--verify-regression',action='store_true')
p.add_argument('--dxc',type=Path)
p.add_argument('--llvm-mc',type=Path)
p.add_argument('--all-camera-programs',action='store_true')
p.add_argument('--require-complete',action='store_true',help='Reject missing companions for captured camera programs instead of filtering them silently')
p.add_argument('--coverage-vs',type=Path,help='Original-signature UI VS for material reactive PSO creation')
a=p.parse_args();a.output.mkdir(parents=True,exist_ok=True)
root=Path(__file__).resolve().parents[2]
draw=(root/'rexlego/src/gpu_native/draw.cpp').read_text()
verified={int(v,16) for v in re.findall(r'0x([0-9A-Fa-f]+)ull',body(draw,'bool HasVerifiedTemporalCamera('))}
metadata=(a.bank/'temporal-bank.cpp').read_text()
available={int(v,16) for v in re.findall(r'\{0x([0-9A-Fa-f]+)ull',metadata)}
if a.all_camera_programs:
    verified |= {int(v,16) for v in re.findall(r'\{0x([0-9A-Fa-f]+)ull,0,\d+,\d+,\d+,1\}',metadata)}
pairs=set();excluded=[]
for path in a.capture:
    for vs,ps in re.findall(r'VS=([0-9a-fA-F]+) PS=([0-9a-fA-F]+)',path.read_text()):
        pair=(int(vs,16),int(ps,16))
        if pair[0] in verified and pair[1] in available:pairs.add(pair)
        elif pair[0] in verified and pair[1]:excluded.append(pair)
if a.require_complete:
    assert not excluded,'Captured camera programs have missing pixel companions: '+str(sorted(set(excluded)))
assert pairs,'No captured verified pairs'
fixture=r'''
#include <plume_d3d12.h>
#include "gpu_native/state.h"
#include "gpu_native/shaders.h"
#include "gpu_native/temporal_shaders.h"
#include "gpu_native/device.h"
#include "gpu_native/draw.h"
#include "gpu_native/vertex_declarations.h"
#include "gpu_native/depth_state.h"
#include "gpu_native/stencil_state.h"
#include "gpu_native/blend_state.h"
#include "gpu_native/float_blend_indices.h"
#include "gpu_native/cull_state.h"
#include <bit>
#include <atomic>
#include <chrono>
#include <cassert>
#include <mutex>
#include <unordered_map>
#include <cstdio>
#include <fstream>
#define REXLOG_INFO(...) ((void)0)
#define REXLOG_ERROR(...) ((void)0)
#define REXLOG_WARN(...) ((void)0)
namespace plume {std::unique_ptr<RenderInterface> CreateD3D12Interface();}
namespace legodimensions::gpu_native {
DrawTiming g_draw_timing;
plume::RenderDevice* g_device;
plume::RenderPipelineLayout* g_layout;
plume::RenderDevice* HostDevice::Device(){return g_device;}
plume::RenderPipelineLayout* HostDevice::PipelineLayout(){return g_layout;}
// The fallback branch only needs an ordinary VS signature for this link check.
plume::RenderShader* g_coverage_vs=nullptr;
plume::RenderShader* ResolveBoundShader(ShaderStage,u32){return g_coverage_vs?g_coverage_vs:ResolveTemporalShader(0xB881E7DFA89CF8F4ull,false,0);}
'''
for signature in ['struct PipelineKey {','struct PipelineKeyHash {']:
    fixture+=body(draw,signature)+';\n'
fixture+='std::mutex g_pipeline_mutex;\nstd::unordered_map<PipelineKey,std::unique_ptr<plume::RenderPipeline>,PipelineKeyHash> g_temporal_pipelines;\n'
for signature in ['bool HasVerifiedTemporalCamera(','void ApplyCullPipeline(','void SetNativePipelineStencilReference(','plume::RenderPipeline* GetTemporalPipeline(']:
    fixture+=body(draw,signature)+'\n'
fixture+='}\nusing namespace legodimensions::gpu_native;\nint main(){\n'
fixture+=r'''
 setvbuf(stdout,nullptr,_IONBF,0);
 printf("BEGIN production PSO driver check\n");
 auto owner_interface=plume::CreateD3D12Interface();auto device=owner_interface->createDevice();assert(device);g_device=device.get();
 plume::RenderPipelineLayoutBuilder layout;layout.begin(false,true);
 plume::RenderDescriptorSetBuilder textures;textures.begin();textures.addTexture(0,65536);textures.end(true,65536);
 for(int i=0;i<3;++i)layout.addDescriptorSet(textures);
 plume::RenderDescriptorSetBuilder samplers;samplers.begin();samplers.addSampler(0,2048);samplers.end(true,2048);layout.addDescriptorSet(samplers);
 for(unsigned r:{0u,1u,2u,4u})layout.addRootDescriptor(r,4,plume::RenderRootDescriptorType::CONSTANT_BUFFER);
 layout.addPushConstant(3,4,16,plume::RenderShaderStageFlag::PIXEL);layout.end();auto root=layout.create(device.get());assert(root);g_layout=root.get();
 std::vector<plume::RenderInputElement> elements;
 const auto add=[&](const char* name,unsigned index,plume::RenderFormat f=plume::RenderFormat::R32G32B32A32_FLOAT){
   plume::RenderInputElement e;e.semanticName=name;e.semanticIndex=index;e.format=f;e.slotIndex=0;e.alignedByteOffset=0;elements.push_back(e);};
 for(unsigned i=0;i<5;++i)add("POSITION",i);
 for(const auto* name:{"NORMAL","TANGENT","BINORMAL","BLENDWEIGHT"})add(name,0);
 add("BLENDINDICES",0,plume::RenderFormat::R32G32B32A32_UINT);
 for(unsigned i=0;i<2;++i)add("COLOR",i);
 for(unsigned i=0;i<16;++i)add("TEXCOORD",i);
 VertexDeclarationView declaration;declaration.elements=elements.data();declaration.element_count=elements.size();declaration.supported=true;
 const uint64_t pairs[][2]={
'''
fixture+=','.join(f'{{0x{vs:016X}ull,0x{ps:016X}ull}}' for vs,ps in sorted(pairs))+'};\n'
fixture+=r'''
 unsigned count=0;
 for(auto& pair:pairs)for(unsigned spec=0;spec<4;++spec)for(unsigned comparison:{1u,2u,3u,6u}) {
   PipelineKey key;key.vertex_shader=key.pixel_shader=1;key.vertex_shader_hash=pair[0];key.pixel_shader_hash=pair[1];
   key.declaration_hash=1;key.spec_constants=spec;key.color_write_mask=15;
   key.render_target_formats[0]=unsigned(plume::RenderFormat::R8G8B8A8_UNORM);
   key.depth_control=2u|4u|(comparison<<4);key.raster_control=kNativeCullCandidate|6;
   key.strides.fill(16);key.depth_format=unsigned(plume::RenderFormat::D32_FLOAT_S8_UINT);
   auto* pipeline=GetTemporalPipeline(key,declaration,plume::RenderPrimitiveTopology::TRIANGLE_LIST,plume::RenderFormat::D32_FLOAT_S8_UINT);
   if(!pipeline){printf("FAIL VS=%016llX PS=%016llX spec=%u comparison=%u\n",pair[0],pair[1],spec,comparison);return 2;}
   const auto created=g_draw_timing.temporal_pipeline_creations;
   assert(GetTemporalPipeline(key,declaration,plume::RenderPrimitiveTopology::TRIANGLE_LIST,plume::RenderFormat::D32_FLOAT_S8_UINT)==pipeline);
   // All draws bind explicit IA views; their dynamic stride cannot require a
   // second D3D12 pipeline. Actual stride-dependent reads are checked separately.
   key.strides[0]=48;key.strides[15]=32;
   assert(GetTemporalPipeline(key,declaration,plume::RenderPrimitiveTopology::TRIANGLE_LIST,plume::RenderFormat::D32_FLOAT_S8_UINT)==pipeline);
   assert(g_draw_timing.temporal_pipeline_creations==created);++count;
 }
 g_temporal_pipelines.clear();ResetTemporalShaders();ResetFloatBlendIndicesShaders();printf("PASS: %u production motion PSOs from saved world shader pairs, four specializations and four depth comparisons\n",count);
}
'''
cpp=a.output/'pipeline-gpu.cpp';cpp.write_text(fixture);exe=a.output/'pipeline-gpu.exe'
if a.coverage_vs:
    extra='''
 std::ifstream input(R"(%s)",std::ios::binary|std::ios::ate);assert(input);std::vector<char> bytes(size_t(input.tellg()));input.seekg(0);input.read(bytes.data(),bytes.size());
 auto coverage_vs=device->createShader(bytes.data(),bytes.size(),"main",plume::RenderShaderFormat::DXIL);assert(coverage_vs);g_coverage_vs=coverage_vs.get();
 unsigned coverage_count=0;
 for(auto hash:{0x75A4D1ABA62F7594ull,0xEAAE2C4ABD94FC99ull})for(unsigned spec=0;spec<4;++spec)
 for(unsigned blend:{0x00010001u,0x01010706u,0x01010106u,0x01010101u,0x01010701u})
 for(unsigned depth_case:{0u,1u,2u}){
  PipelineKey key;key.vertex_shader=key.pixel_shader=1;key.vertex_shader_hash=0xACAF01F01E1C03D2ull;key.pixel_shader_hash=hash;
  key.declaration_hash=1;key.spec_constants=spec;key.color_write_mask=15;key.blend_controls[0]=blend;
  key.render_target_formats[0]=unsigned(plume::RenderFormat::R8G8B8A8_UNORM);key.strides.fill(16);
  auto depth_format=depth_case?plume::RenderFormat::D32_FLOAT_S8_UINT:plume::RenderFormat::UNKNOWN;
  key.depth_format=unsigned(depth_format);key.depth_control=depth_case?(2u|((depth_case==2)?4u:0u)|(3u<<4)):0u;
  const auto mode=TemporalCoverageForBlend(DecodeColorBlend(blend));assert(HasTemporalCoverageShader(hash,mode,spec));
  auto* pipeline=GetTemporalPipeline(key,declaration,plume::RenderPrimitiveTopology::TRIANGLE_LIST,depth_format);
  assert(pipeline);assert(GetTemporalPipeline(key,declaration,plume::RenderPrimitiveTopology::TRIANGLE_LIST,depth_format)==pipeline);++coverage_count;
 }
 g_coverage_vs=nullptr;printf("PASS: %%u production material reactive PSOs, original VS signature, exact alias, five modes, four specializations, no-depth/depth-read/depth-write\\n",coverage_count);
'''%a.coverage_vs.resolve().as_posix()
    fixture=fixture.replace(' g_temporal_pipelines.clear();ResetTemporalShaders();',extra+' g_temporal_pipelines.clear();ResetTemporalShaders();')
    cpp.write_text(fixture)
command=[a.compiler,str(cpp),str(root/'rexlego/src/gpu_native/temporal_shaders.cpp'),str(root/'rexlego/src/gpu_native/float_blend_indices.cpp'),'/std:c++latest','/O2','/MD','/EHsc','/UNDEBUG','/DNOMINMAX',
         '/I'+str(root/'rexlego/src'),'/I'+str(root/'rexglue-sdk/include'),'/I'+str(root/'thirdparty/plume'),
         '/I'+str(root/'thirdparty/plume/contrib/D3D12MemoryAllocator/include'),
         '/DLEGO_TEMPORAL_SHADER_METADATA="'+(a.bank.resolve()/'temporal-bank.cpp').as_posix()+'"',
         '/DLEGO_FLOAT_BLEND_INDICES_METADATA="'+(a.material_bank.resolve()/'float-blend-indices-bank.cpp').as_posix()+'"',
         '/Fo'+str(a.output.resolve())+'\\','/Fe'+str(exe),'/link',str(a.plume_lib),str(a.bank/'temporal-bank.obj'),str(a.material_bank/'float-blend-indices-bank.obj'),'d3d12.lib','dxgi.lib','dxguid.lib','user32.lib']
result=subprocess.run(command,capture_output=True,text=True,timeout=120);(a.output/'compile.log').write_text(result.stdout+result.stderr);result.check_returncode()
result=subprocess.run([str(exe.resolve())],capture_output=True,text=True,timeout=120);(a.output/'run.log').write_text(result.stdout+result.stderr);print(result.stdout+result.stderr);result.check_returncode()
assert 'production motion PSOs from saved world shader pairs' in result.stdout,'Fixture exited before completing motion PSO creation'
if a.coverage_vs:
    assert '120 production material reactive PSOs' in result.stdout,'Fixture exited before completing all depth/material cases'
negative_exit=None
if a.verify_regression:
    assert a.dxc and a.llvm_mc
    ps_hash=next(p for p in sorted({p for _,p in pairs if p})
        if 'SV_IsFrontFace' in (a.bank/f'{p:016X}-ps-0-motion.hlsl').read_text())
    source=(a.bank/f'{ps_hash:016X}-ps-0-motion.hlsl').read_text()
    signature=re.search(r'void main\((.*?)\)\s*\{',source,re.S)
    params=signature[1].split(',')
    front=next(param for param in params if 'SV_IsFrontFace' in param)
    params.remove(front)
    params.insert(next(i for i,param in enumerate(params) if 'LEGOCURRENTCLIP' in param),front)
    bad_source=a.output/'frontface-register-mutant.hlsl'
    bad_source.write_text(source[:signature.start(1)]+','.join(params)+source[signature.end(1):])
    bad_dxil=a.output/'frontface-register-mutant.dxil'
    subprocess.run([str(a.dxc),str(bad_source),'-T','ps_6_0','-E','main','-Fo',str(bad_dxil)],check=True,capture_output=True,timeout=60)
    blob=bytearray((a.bank/'temporal-bank.bin').read_bytes())
    while len(blob)%16:blob.append(0)
    code=bad_dxil.read_bytes();offset=len(blob);blob.extend(code)
    bad_bank=a.output/'frontface-register-mutant.bin';bad_bank.write_bytes(blob)
    bad_metadata=a.output/'frontface-register-mutant.cpp'
    bad_metadata.write_text(re.sub(r'\{0x'+f'{ps_hash:016X}'+r'ull,1,0,\d+,\d+,0\}',
        '{0x'+f'{ps_hash:016X}'+f'ull,1,0,{offset},{len(code)},0'+'}',metadata))
    asm=a.output/'frontface-register-mutant.s'
    asm.write_text('.section .rdata,"dr"\n.p2align 4\n.globl g_temporalShaderBlob\ng_temporalShaderBlob:\n.incbin "'+bad_bank.resolve().as_posix()+'"\n')
    bad_object=a.output/'frontface-register-mutant.obj'
    subprocess.run([str(a.llvm_mc),'-triple=x86_64-pc-windows-msvc','-filetype=obj',str(asm),'-o',str(bad_object)],check=True,timeout=60)
    bad_exe=a.output/'frontface-register-mutant.exe'
    bad_command=[('/DLEGO_TEMPORAL_SHADER_METADATA="'+bad_metadata.resolve().as_posix()+'"') if v.startswith('/DLEGO_TEMPORAL_SHADER_METADATA=')
        else '/Fe'+str(bad_exe) if v.startswith('/Fe') else str(bad_object) if v==str(a.bank/'temporal-bank.obj') else v for v in command]
    compiled=subprocess.run(bad_command,capture_output=True,text=True,timeout=120)
    (a.output/'negative-compile.log').write_text(compiled.stdout+compiled.stderr);compiled.check_returncode()
    rejected=subprocess.run([str(bad_exe.resolve())],capture_output=True,text=True,timeout=120)
    (a.output/'negative-run.log').write_text(rejected.stdout+rejected.stderr)
    assert rejected.returncode==2 and f'PS={ps_hash:016X}' in rejected.stdout,rejected.stdout+rejected.stderr
    negative_exit=rejected.returncode
    print('PASS: restoring the front-face register shift makes the actual driver reject the motion PSO')
(a.output/'verification.json').write_text(json.dumps({'game_launched':False,'pairs':[[f'{v:016X}',f'{p:016X}'] for v,p in sorted(pairs)],'excluded_missing_companions':[[f'{v:016X}',f'{p:016X}'] for v,p in sorted(set(excluded))],'required_complete':a.require_complete,'production_pso_builder':True,'original_vs_fallback_fixture':True,'frontface_shift_negative_exit':negative_exit,'result':'PASS'},indent=2)+'\n')
