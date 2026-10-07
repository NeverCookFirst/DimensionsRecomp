"""Exercise native culling bodies, host winding, cache identity and guest state."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess


def body(source, signature):
    start = source.index(signature)
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


p = argparse.ArgumentParser(description=__doc__)
p.add_argument('output', type=Path)
p.add_argument('--compiler', default='clang++')
a = p.parse_args()
root = Path(__file__).resolve().parents[2]
a.output.mkdir(parents=True, exist_ok=True)
draw = (root/'rexlego/src/gpu_native/draw.cpp').read_text()
functions = body(draw, 'void ApplyCullPipeline(') + '\n' + body(draw, 'plume::RenderRect CullDrawScissor(')
key = body(draw, 'struct PipelineKey {') + ';\n' + body(draw, 'struct PipelineKeyHash {') + ';\n'
# Both-cull must still reach the ordinary submission and query scope; this is
# not a CPU draw rejection. Run the actual non-indexed submission body below.
nonindex_draw = draw.index('    commands->drawInstanced(count, 1, use_windows ? 0 : start, 0);')
nonindex_begin = draw.rfind('    QueryDrawScope query_scope(commands);', 0, nonindex_draw)
assert nonindex_begin >= 0
submission = draw[nonindex_begin:]
submission = submission[:submission.index('\n  }')]
assert 'CullDrawScissor(' in draw[draw.index('bool DispatchDraw('):]
assert draw.index('commands->setScissors(CullDrawScissor(') < draw.index(submission)
assert 'DecodeNativeCullState(' not in body(draw, 'bool DispatchDraw(')

plume_types = root/'thirdparty/plume/plume_render_interface_types.h'
plume_d3d = root/'thirdparty/plume/plume_d3d12.cpp'
host_oracle = plume_types.exists() and plume_d3d.exists()
if host_oracle:
    types = plume_types.read_text()
    enums = body(types, 'enum class RenderCullMode {') + ';\n' + body(types, 'enum class RenderFrontFace {') + ';\n'
    rect = body(types, 'struct RenderRect {') + ';\n'
    d3d = plume_d3d.read_text()
    mapping = body(d3d, 'switch (desc.cullMode) {') + '\n'
    face_line = 'psoDesc.RasterizerState.FrontCounterClockwise = desc.frontFace == RenderFrontFace::COUNTER_CLOCKWISE;'
    assert face_line in d3d
    mapping += face_line
else:
    # Standard asset-free checkout may not have initialized this submodule.
    # Production culling and key bodies still execute with the narrow fake ABI.
    enums = 'enum class RenderCullMode {UNKNOWN,NONE,FRONT,BACK}; enum class RenderFrontFace {UNKNOWN,CLOCKWISE,COUNTER_CLOCKWISE};'
    rect = '''struct RenderRect {int32_t left,top,right,bottom;
      bool operator==(const RenderRect&) const = default;
      bool isEmpty() const {return left>=right||top>=bottom;}};'''
    mapping = 'psoDesc.RasterizerState.CullMode = int(desc.cullMode); psoDesc.RasterizerState.FrontCounterClockwise = desc.frontFace == RenderFrontFace::COUNTER_CLOCKWISE;'

registers_path = root/'rexglue-sdk/include/rex/graphics/registers.h'
guest_oracle = registers_path.exists()
registers = ''
if guest_oracle:
    registers = '''namespace xenos {enum class PolygonModeEnable:uint32_t {}; enum class PolygonType:uint32_t {};}
      using Register=uint32_t; constexpr Register XE_GPU_REG_PA_SU_SC_MODE_CNTL=0;\n'''
    registers += body(registers_path.read_text(), 'union alignas(uint32_t) PA_SU_SC_MODE_CNTL {') + ';\n'

# When actual game output is present, also execute the untouched TU23 setter.
# Finding its definition is independent of codegen partition numbering.
setter = ''
setter_path = None
signature = 'DEFINE_REX_FUNC(sub_83FB7DA0)'
for path in sorted((root/'rexlego/generated/default').glob('*.cpp')):
    with path.open() as source:
        if any(line.startswith(signature + ' {') for line in source):
            if setter_path is not None:
                raise RuntimeError('Duplicate actual guest cull setter definition')
            setter_path = path
            setter = body(path.read_text(), signature)

cpp = a.output/'cull-state.cpp'
cpp.write_text(r'''
#include <array>
#include <bit>
#include <cassert>
#include <cstdint>
#include <unordered_map>
#include "gpu_native/cull_state.h"
#include "gpu_native/polygon_offset.h"
using u8=uint8_t;using u32=uint32_t;using u64=uint64_t;using i32=int32_t;
using namespace legodimensions::gpu_native;
constexpr u32 kNativeRenderTargets=4,kNativeVertexStreams=16;
namespace plume {
''' + enums + '\n' + rect + r'''
struct RenderGraphicsPipelineDesc {
 RenderCullMode cullMode=RenderCullMode::UNKNOWN;
 RenderFrontFace frontFace=RenderFrontFace::CLOCKWISE;
 bool stencilEnabled=false;
};
}
''' + functions + '\n' + key + registers + r'''
enum {D3D12_CULL_MODE_NONE=1,D3D12_CULL_MODE_FRONT=2,D3D12_CULL_MODE_BACK=3};
struct PSO {struct {int CullMode=0;bool FrontCounterClockwise=false;} RasterizerState;};
void MapToActualHost(const plume::RenderGraphicsPipelineDesc& desc,PSO& psoDesc) {
 using plume::RenderCullMode;using plume::RenderFrontFace;
''' + mapping + r'''
}
union PPCRegister {uint64_t u64;uint32_t u32;};
struct PPCContext {PPCRegister r3{},r4{},r11{};};
std::array<u8,65536> ram{};
u32 Load(u32 p){return u32(ram[p])<<24|u32(ram[p+1])<<16|u32(ram[p+2])<<8|ram[p+3];}
void Store(u32 p,u32 v){for(u32 i=0;i<4;++i)ram[p+i]=u8(v>>(24-8*i));}
u64 Load64(u32 p){return (u64(Load(p))<<32)|Load(p+4);}
void Store64(u32 p,u64 v){Store(p,u32(v>>32));Store(p+4,u32(v));}
#define REX_LOAD_U32(p) Load(p)
#define REX_STORE_U32(p,v) Store(p,v)
#define REX_LOAD_U64(p) Load64(p)
#define REX_STORE_U64(p,v) Store64(p,v)
#define REX_FUNC_PROLOGUE() ((void)0)
#define DEFINE_REX_FUNC(n) void n(PPCContext& ctx,u8* base)
#ifndef __clang__
// The generated TU23 leaf uses Clang's spelling; std::rotl is the same
// unsigned rotation for fixtures compiled with other GNU-style drivers.
#define __builtin_rotateleft64(value,amount) std::rotl(u64(value),int(amount))
#endif
''' + setter + r'''
struct Recorder {
 plume::RenderRect scissor{1,2,100,200};
 u32 expected_start=2;
 int begun=0,ended=0,draws=0,vertices=0,covered=0;
 void drawInstanced(u32 count,u32 instances,u32 start,u32 first) {
  assert(instances==1&&start==expected_start&&first==0);++draws;vertices+=count;
  covered+=scissor.isEmpty()?0:count;
 }
};
struct QueryDrawScope {Recorder* c;explicit QueryDrawScope(Recorder* c):c(c){++c->begun;}
 ~QueryDrawScope(){++c->ended;}};
void SubmitActualNonIndexed(Recorder* commands,u32 count,u32 start,bool use_windows) {
''' + submission + r'''
}
int main(){
 const plume::RenderRect original{1,2,100,200};
 std::unordered_map<PipelineKey,int,PipelineKeyHash> pipelines;
 PipelineKey legacy;pipelines[legacy]=99;
 for(u32 guest=0;guest<8;++guest) {
''' + (r'''
  PA_SU_SC_MODE_CNTL reg;reg.value=guest;
  assert(reg.cull_front==(guest&1)&&reg.cull_back==((guest>>1)&1));
  assert(reg.face==((guest>>2)&1));
''' if guest_oracle else '') + (r'''
  for(u32 initial:{0u,0xFFFFF8u,0xA3512800u,0xFFFFFFFFu}) {
   constexpr u32 dev=4096;Store(dev+10568,initial);Store64(dev+16,0xA010000000000001ULL);
   PPCContext ctx;ctx.r3.u32=dev;ctx.r4.u64=0xABCD000000000000ULL|guest;
   sub_83FB7DA0(ctx,ram.data());
   assert(Load(dev+10568)==((initial&~7u)|guest));
   assert(Load64(dev+16)==0xA010000000000041ULL);
  }
''' if setter else '') + r'''
  for(bool polygonal:{false,true})for(bool stencil:{false,true}) {
   plume::RenderGraphicsPipelineDesc desc;desc.stencilEnabled=stencil;
   desc.frontFace=plume::RenderFrontFace::CLOCKWISE;
   ApplyCullPipeline(desc,0,polygonal);
   assert(desc.cullMode==plume::RenderCullMode::NONE);
   assert(desc.frontFace==plume::RenderFrontFace::CLOCKWISE);
   assert(CullDrawScissor(original,0,polygonal)==original);
   const u32 control=kNativeCullCandidate|(guest&(polygonal?7u:4u));
   ApplyCullPipeline(desc,control,polygonal);
   const u32 faces=polygonal?guest&3u:0u;
   assert(desc.frontFace==((guest&4)?plume::RenderFrontFace::CLOCKWISE:plume::RenderFrontFace::COUNTER_CLOCKWISE));
   assert(desc.cullMode==(faces==1?plume::RenderCullMode::FRONT:faces==2?plume::RenderCullMode::BACK:plume::RenderCullMode::NONE));
   PSO host;MapToActualHost(desc,host);
   assert(host.RasterizerState.CullMode==(faces==1?2:faces==2?3:1));
   assert(host.RasterizerState.FrontCounterClockwise==!(guest&4));
   for(bool use_windows:{false,true}) {
    Recorder recorder;recorder.scissor=CullDrawScissor(original,control,polygonal);
    recorder.expected_start=use_windows?0:2;
    assert(recorder.scissor.isEmpty()==(faces==3));
    if(faces!=3)assert(recorder.scissor==original);
    SubmitActualNonIndexed(&recorder,6,2,use_windows);
    assert(recorder.begun==1&&recorder.ended==1&&recorder.draws==1&&recorder.vertices==6);
    assert(recorder.covered==(faces==3?0:6));
   }
   assert(NativeStencilWordOffset(control,129,polygonal)==(faces==1?10492u:10496u));
   assert(NativeStencilWordOffset(control,1,polygonal)==10496);
   assert(NativeStencilWordOffset(0,129,polygonal)==10496);
  }
  PipelineKey candidate;candidate.raster_control=kNativeCullCandidate|guest;
  assert(!(candidate==legacy));assert(PipelineKeyHash{}(candidate)!=PipelineKeyHash{}(legacy));
  pipelines[candidate]=guest;
 }
 assert(pipelines.size()==9);
 for(u32 guest=0;guest<8;++guest){PipelineKey key;key.raster_control=kNativeCullCandidate|guest;assert(pipelines.at(key)==int(guest));}
 // Only surviving faces contribute polygon offset. Para offsets are unaffected.
 const u32 biasmode=(1u<<11)|(1u<<12)|(1u<<13);
 for(u32 faces=0;faces<4;++faces) {
  auto bias=DecodePolygonOffset(CullPolygonOffsetMode(biasmode,kNativeCullCandidate|faces,true),true,16,1,32,2);
  assert(bias.slope_bits==std::bit_cast<u32>(faces==3?0.f:faces==1?2.f:1.f));
  assert(bias.depth_bias==(faces==3?0:faces==1?33554430:16777215));
  auto legacybias=DecodePolygonOffset(CullPolygonOffsetMode(biasmode,faces,true),true,16,1,32,2);
  assert(legacybias.depth_bias==16777215);
  auto pointbias=DecodePolygonOffset(CullPolygonOffsetMode(biasmode,kNativeCullCandidate|faces,false),false,16,1,32,2);
  assert(pointbias.depth_bias==16777215);
 }
 // Distinct reference/mask bytes survive selection of the actual guest word.
 Store(10492,0x00AABB11);Store(10496,0x00CCDD22);
 auto back=NativeStencilWordOffset(kNativeCullCandidate|1,129,true);
 auto front=NativeStencilWordOffset(kNativeCullCandidate|2,129,true);
 assert((Load(back)&0xFFFF00)==0xAABB00&&ram[back+3]==0x11);
 assert((Load(front)&0xFFFF00)==0xCCDD00&&ram[front+3]==0x22);
}
''')
exe = a.output/'cull-state.exe'
subprocess.run([a.compiler, '-std=c++20', '-O2', '-UNDEBUG', '-I', str(root/'rexlego/src'),
                str(cpp), '-o', str(exe)], check=True, timeout=45)
subprocess.run([str(exe)], check=True, timeout=10)
proof = {'passed': True, 'candidate': 'LEGO_NATIVE_CULL environment presence',
         'raster_cases': 32, 'actual_production_bodies': 5,
         'actual_plume_d3d12_mapping': host_oracle, 'actual_sdk_register_layout': guest_oracle,
         'actual_tu23_setter_cases': 32 if setter else 0,
         'setter_source': str(setter_path) if setter_path else None,
         'setter_sha256': hashlib.sha256(setter.encode()).hexdigest() if setter else None,
         'both_cull': 'Empty scissor; actual nonindexed submission/query body still runs',
         'nonindexed_window_modes': 'Both disabled original start and enabled rebased start execute actual submission',
         'scope': 'CPU state/winding/identity/submission contract; no GPU or visual repair claim'}
(a.output/'verification.json').write_text(json.dumps(proof, indent=2)+'\n')
print('PASS: cull/winding mapping, points/lines, both-face query submission, stencil/bias and nine distinct PSOs')
