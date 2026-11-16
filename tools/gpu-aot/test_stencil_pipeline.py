"""Exercise native stencil pipeline creation and Plume's submission reference."""
import argparse
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
p.add_argument('--negative-control', action='store_true')
a = p.parse_args()
root = Path(__file__).resolve().parents[2]
a.output.mkdir(parents=True, exist_ok=True)
draw = (root/'rexlego/src/gpu_native/draw.cpp').read_text()
dispatch = body(draw, 'bool DispatchDraw(')
assert 'std::getenv("LEGO_NATIVE_NO_STENCIL") == nullptr' in dispatch
assert 'std::getenv("LEGO_NATIVE_NO_CULL") == nullptr' in dispatch
assert 'state_bytes + word_offset) & 0x00FFFFFFu' in dispatch
assert 'OMSetStencilRef' not in dispatch
init = (root/'rexlego/src/gpu_native/hooks_device.cpp').read_text()
assert '(!std::getenv("LEGO_NATIVE_NO_STENCIL") &&' in init
assert '!InitializeStencilDefaults(guest_address, memory->virtual_membase())' in init
key = body(draw, 'struct PipelineKey {') + ';\n' + body(draw, 'struct PipelineKeyHash {') + ';\n'
pipeline = body(draw, 'plume::RenderPipeline* GetPipeline(')
start = pipeline.index('  const auto stencil = DecodeStencilState(')
end = pipeline.index('  desc.primitiveTopology = topology;', start)
stencil_body = pipeline[start:end]
types = (root/'thirdparty/plume/plume_render_interface_types.h').read_text()
enum_names = ('RenderComparisonFunction', 'RenderStencilOp', 'RenderFrontFace', 'RenderPrimitiveTopology')
enums = '\n'.join(body(types, f'enum class {name} ') + ';' for name in enum_names)
face = body(types, 'struct RenderStencilFaceDesc {') + ';'
plume = (root/'thirdparty/plume/plume_d3d12.cpp').read_text()
submit = body(plume, 'void D3D12CommandList::checkStencilRef(')
reference = body(draw, 'void SetNativePipelineStencilReference(')
if a.negative_control:
    reference = reference.replace('desc.stencilEnabled ? desc.stencilReference : 0', '0')
assert 'SetNativePipelineStencilReference(pipeline.get(), desc);' in pipeline
source = r'''
#include <array>
#include <cassert>
#include <cstdint>
#include <cstddef>
#include "gpu_native/stencil_state.h"
using u32=uint32_t;using u64=uint64_t;using i32=int32_t;
constexpr u32 kNativeRenderTargets=4,kNativeVertexStreams=18;
namespace plume {
''' + enums + '\n' + face + r'''
struct RenderGraphicsPipelineDesc {
 bool stencilEnabled=false; uint32_t stencilReadMask=255,stencilWriteMask=255,stencilReference=0;
 RenderStencilFaceDesc stencilFrontFace,stencilBackFace;
 RenderFrontFace frontFace=RenderFrontFace::COUNTER_CLOCKWISE;
};
}
using namespace legodimensions::gpu_native;
''' + key + r'''
namespace plume { enum class RenderFormat {UNKNOWN,D32_FLOAT_S8_UINT,D24_UNORM_S8_UINT};
constexpr bool RenderFormatIsStencil(RenderFormat f){return f!=RenderFormat::UNKNOWN;} }
plume::RenderGraphicsPipelineDesc Make(const PipelineKey& key) {
 plume::RenderGraphicsPipelineDesc desc;
 auto depth_format=static_cast<plume::RenderFormat>(key.depth_format);
 auto topology=static_cast<plume::RenderPrimitiveTopology>(key.topology);
 constexpr plume::RenderComparisonFunction comparisons[]={
 plume::RenderComparisonFunction::NEVER,plume::RenderComparisonFunction::LESS,
 plume::RenderComparisonFunction::EQUAL,plume::RenderComparisonFunction::LESS_EQUAL,
 plume::RenderComparisonFunction::GREATER,plume::RenderComparisonFunction::NOT_EQUAL,
 plume::RenderComparisonFunction::GREATER_EQUAL,plume::RenderComparisonFunction::ALWAYS};
''' + stencil_body + r'''
 return desc;
}
struct D3D12Pipeline {enum class Type {Graphics};Type type=Type::Graphics;};
struct D3D12GraphicsPipeline:D3D12Pipeline {uint32_t stencilRef=0;};
namespace plume {using RenderPipeline=::D3D12Pipeline;using D3D12GraphicsPipeline=::D3D12GraphicsPipeline;}
struct Recorder {uint32_t value=999,calls=0;void OMSetStencilRef(uint32_t ref){value=ref;++calls;}};
struct D3D12CommandList {D3D12Pipeline* activeGraphicsPipeline=nullptr;
 uint32_t activeStencilRef=~0u;Recorder* d3d;void checkStencilRef();};
''' + reference + '\n' + submit + r'''
int main(){
 PipelineKey key{};
 key.depth_format=uint32_t(plume::RenderFormat::D32_FLOAT_S8_UINT);
 key.topology=uint32_t(plume::RenderPrimitiveTopology::TRIANGLE_LIST);
 key.depth_control=1u|(7u<<8)|(7u<<20);
 key.stencil_masks=0x00FFFF00;
 Recorder recorder;D3D12GraphicsPipeline host;
 D3D12CommandList commands;commands.d3d=&recorder;commands.activeGraphicsPipeline=&host;
 for(auto format:{plume::RenderFormat::D32_FLOAT_S8_UINT,plume::RenderFormat::D24_UNORM_S8_UINT})
 for(uint32_t ref=0;ref<256;++ref){
  key.depth_format=uint32_t(format);
  auto previous=key;key.stencil_masks=0x005AA500|ref;
  assert(ref==0||!(previous==key));
  auto desc=Make(key);
  assert(desc.stencilEnabled&&desc.stencilReadMask==0xA5&&desc.stencilWriteMask==0x5A);
  assert(desc.stencilReference==ref);
  SetNativePipelineStencilReference(&host,desc);commands.checkStencilRef();
  assert(recorder.value==ref);
  auto calls=recorder.calls;commands.checkStencilRef();assert(recorder.calls==calls);
 }
 for(uint32_t operation=0;operation<8;++operation){
  key.depth_control=1u|128u|(5u<<8)|(2u<<20)|(operation<<11)|((7-operation)<<23);
  auto desc=Make(key);
  assert(desc.stencilFrontFace.compareFunction==plume::RenderComparisonFunction::NOT_EQUAL);
  assert(desc.stencilBackFace.compareFunction==plume::RenderComparisonFunction::EQUAL);
  assert(uint32_t(desc.stencilFrontFace.failOp)==uint32_t(plume::RenderStencilOp::KEEP)+operation);
  assert(uint32_t(desc.stencilBackFace.failOp)==uint32_t(plume::RenderStencilOp::KEEP)+7-operation);
 }
 key.depth_control=0;assert(!Make(key).stencilEnabled);
 key.depth_control=1;key.depth_format=0;assert(!Make(key).stencilEnabled);
}
'''
cpp = a.output/'stencil-pipeline.cpp'
cpp.write_text(source)
exe = a.output/'stencil-pipeline.exe'
subprocess.run([a.compiler, '-std=c++20', '-O2', '-I'+str(root/'rexlego/src'), str(cpp), '-o', str(exe)], check=True)
subprocess.run([str(exe)], check=True)
print('Native stencil pipeline and actual Plume reference submission PASS (256 references)')
