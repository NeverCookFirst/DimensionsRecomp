"""Run production indexed-draw guards with an asset-free command recorder."""
import argparse
from pathlib import Path
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('output', type=Path)
p.add_argument('--compiler', default='clang++')
a = p.parse_args()
a.output.mkdir(parents=True, exist_ok=True)
root = Path(__file__).resolve().parents[2]
source = (root / 'rexlego/src/gpu_native/draw.cpp').read_text()
preflight_start = source.index('  BufferResourceView index_metadata;')
preflight_end = source.index('  timing.Next();', preflight_start)
issue_start = source.index('  } else if (indexed) {')
opening = source.index('{', issue_start)
depth, end = 1, opening + 1
while depth:
    depth += (source[end] == '{') - (source[end] == '}')
    end += 1
issue = source[opening + 1:end - 1]
code = r'''
#include <cassert>
#include <cstdint>
#include <random>
#include <utility>
#include "gpu_native/index_draw.h"
using namespace legodimensions::gpu_native;
using u32=uint32_t;using i32=int32_t;
struct FakeBuffer {FakeBuffer* at(u32 offset){assert(offset==0);return this;}} buffer;
struct BufferResourceView {
  FakeBuffer* buffer=nullptr;
  u32 length=0,guest_format=0,mirror_address=0;
};
enum class BufferKind{kIndex};
struct DrawBindings{u32 index_buffer=123;};
BufferResourceView metadata,full_view,window_view;
u32 requested_offset=0,requested_length=0;
BufferResourceView InspectBufferResource(u32 guest,BufferKind){assert(guest==123);return metadata;}
BufferResourceView ResolveBufferResourceView(u32 guest,BufferKind){assert(guest==123);return full_view;}
BufferResourceView ResolveBufferResourceWindow(u32 guest,BufferKind,u32 offset,u32 length){
  assert(guest==123);requested_offset=offset;requested_length=length;return window_view;
}
namespace plume {
enum class RenderFormat{R16_UINT,R32_UINT};
struct RenderIndexBufferView {
  FakeBuffer* buffer;
  u32 length;
  RenderFormat format;
  RenderIndexBufferView(FakeBuffer* b,u32 l,RenderFormat f):buffer(b),length(l),format(f){}
};
}
struct CommandRecorder {
  u32 bindings=0,draws=0,count=0,start=0;
  i32 base=0;
  plume::RenderFormat format=plume::RenderFormat::R32_UINT;
  void setIndexBuffer(const plume::RenderIndexBufferView* view){++bindings;format=view->format;}
  void drawIndexedInstanced(u32 n,u32 instances,u32 s,i32 b,u32 first_instance){
    assert(instances==1&&first_instance==0);++draws;count=n;start=s;base=b;
  }
};
u32 query_scopes=0;
struct QueryDrawScope{explicit QueryDrawScope(CommandRecorder*){++query_scopes;}};
bool Preflight(bool indexed,u32 start,u32 count,const DrawBindings& bindings){
''' + source[preflight_start:preflight_end] + r'''
  return true;
}
bool Issue(bool use_windows,u32 start,u32 count,i32 base_vertex,u32 min_index,
           u32 index_offset,u32 index_bytes,const DrawBindings& bindings,CommandRecorder* commands){
''' + issue + r'''
  return true;
}
int main(){
  const DrawBindings bindings;
  for(u32 format:{1u,2u,0u}) {
    const u32 width=format==1?2:4;
    metadata={nullptr,16*width,format,0x10000};
    assert(Preflight(true,15,1,bindings));
    assert(!Preflight(true,15,2,bindings));
    assert(!Preflight(true,16,1,bindings));
    assert(!Preflight(true,UINT32_MAX,1,bindings));
    assert(!Preflight(true,1,UINT32_MAX,bindings));
    metadata.mirror_address=0;assert(!Preflight(true,0,1,bindings));
    metadata={};assert(Preflight(false,UINT32_MAX,UINT32_MAX,bindings));
    full_view={&buffer,16*width,format,0x10000};
    CommandRecorder commands;query_scopes=0;
    assert(Issue(false,15,1,-7,0,0,0,bindings,&commands));
    assert(commands.draws==1&&commands.bindings==1&&query_scopes==1);
    assert(commands.count==1&&commands.start==15&&commands.base==-7);
    assert(commands.format==(format==1?plume::RenderFormat::R16_UINT:plume::RenderFormat::R32_UINT));
    for(auto parameters:{std::pair{15u,2u},std::pair{16u,1u},
                         std::pair{UINT32_MAX,1u},std::pair{1u,UINT32_MAX}}){
      commands={};query_scopes=0;
      assert(!Issue(false,parameters.first,parameters.second,19,0,0,0,bindings,&commands));
      assert(commands.bindings==0&&commands.draws==0&&query_scopes==0);
    }
    // A range upload's original start lies outside its small host view. It
    // must bind start=0 and retain the established -min_index vertex rebase.
    window_view={&buffer,6*width,format,0x12000};commands={};query_scopes=0;
    assert(Issue(true,100,6,-7,200,100*width,6*width,bindings,&commands));
    assert(requested_offset==100*width&&requested_length==6*width);
    assert(commands.draws==1&&commands.start==0&&commands.count==6&&commands.base==-200);
    window_view.length-=1;commands={};query_scopes=0;
    assert(!Issue(true,100,6,-7,200,100*width,6*width,bindings,&commands));
    assert(commands.bindings==0&&commands.draws==0&&query_scopes==0);
    // A one-index buffer may occupy just 2/4 bytes; no whole-buffer padding
    // requirement belongs in index range validation.
    full_view={&buffer,width,format,0x10000};commands={};
    assert(Issue(false,0,1,-1,0,0,0,bindings,&commands));
    full_view.buffer=nullptr;commands={};
    assert(!Issue(false,0,1,0,0,0,0,bindings,&commands));
    assert(!commands.bindings&&!commands.draws);
  }
  std::mt19937 random(0x83FC6A58);
  for(u32 n=0;n<20000;++n){
    const u32 width=n%2?2:4;
    const u32 start=random(),count=random(),length=random();
    const bool expected=count&&((uint64_t(start)+count)*width<=length);
    const auto window=DrawIndexWindow(start,count,width,length);
    assert(bool(window)==expected);
    if(window){assert(window.offset==uint64_t(start)*width);assert(window.length==uint64_t(count)*width);}
  }
  assert(!DrawIndexWindow(0,1,3,32));
  assert(!DrawIndexWindow(0,0,2,32));
  assert(!DrawIndexWindow(UINT32_MAX,UINT32_MAX,4,UINT32_MAX));
  const auto upper=DrawIndexWindow(1073741822,1,4,UINT32_MAX);
  assert(upper&&upper.offset==4294967288u&&upper.length==4);
}
'''
cpp = a.output / 'draw-index-bounds.cpp'
cpp.write_text(code)
exe = a.output / 'draw-index-bounds.exe'
subprocess.run([a.compiler, '-std=c++20', '-UNDEBUG',
                '-I' + str(root / 'rexlego/src'), str(cpp), '-o', str(exe)],
               check=True, timeout=45)
subprocess.run([str(exe.resolve())], check=True, timeout=10)
print('PASS: production index metadata/submission guards; 16/32-bit full/window '
      'views, signed base vertices and overflow; rejected draws record no GPU commands')
