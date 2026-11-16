"""Exercise production stream-window selection, binding and draw rebasing."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('output', type=Path)
p.add_argument('--compiler', default='clang++')
p.add_argument('--shader-hlsl', type=Path,
               help='Optional real captured vertex HLSL directory: verify no vertex-ID semantics')
p.add_argument('--verify-regression', action='store_true')
a = p.parse_args()
root = Path(__file__).resolve().parents[2]
source = (root / 'rexlego/src/gpu_native/draw.cpp').read_text()
start = source.index('  std::array<VertexBufferWindow, kNativeVertexStreams> vertex_windows{};')
end = source.index('  if (mesh_trace) {', start)
streams = source[start:end]
issue_start = source.index('    commands->drawInstanced(count, 1,')
issue = source[issue_start:source.index('\n', issue_start)]
code = r'''
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#undef assert
#define assert(c) do{if(!(c)){std::fprintf(stderr,"failed: %s\n",#c);std::exit(23);}}while(0)
#include <cstdint>
#include <cstring>
#include <memory>
#include <random>
#include <vector>
#include "gpu_native/buffer_window.h"
#include "gpu_native/index_draw.h"
#include "gpu_native/index_bounds.h"
#include "gpu_native/vertex_fetch_range.h"
#include "gpu_native/shader_input_usage.h"
#include "gpu_native/vertex_byte_order.h"
#include "gpu_native/vertex_layout_plan.h"
#include "gpu_native/draw_preparation.h"
#include "gpu_native/instanced_fetch.h"
using namespace legodimensions::gpu_native;
using u8=uint8_t;using u32=uint32_t;using u64=uint64_t;using i32=int32_t;
using be_u16=uint16_t;using be_u32=uint32_t;
constexpr u32 kNativeVertexStreams=4;
namespace rex::graphics::xenos {enum class PrimitiveType:u32{kQuadList=13};}
struct Buffer {std::vector<u8> bytes;u8* at(u32 n){assert(n<=bytes.size());return bytes.data()+n;}};
namespace plume {
struct RenderVertexBufferView {u8* address;u32 length;RenderVertexBufferView():address(nullptr),length(0){}
 RenderVertexBufferView(u8* a,u32 n):address(a),length(n){} };
struct RenderInputSlot {RenderInputSlot()=default;RenderInputSlot(u32,u32){} };
u32 RenderFormatSize(u32 n){return n;}
}
enum class BufferKind{kVertex,kIndex};
struct BufferResourceView {Buffer* buffer=nullptr;u32 length=0,guest_format=0,mirror_address=0;};
struct Binding {u32 buffer=0,stride=0,offset=0;};
struct Bindings {std::array<Binding,4> vertex_streams{};};
struct Element {u32 slotIndex=0,alignedByteOffset=0,format=0;const char* semanticName="POSITION";u32 semanticIndex=0;};
struct {u64 vertex_shader_hash=0,pixel_shader_hash=0;u32 spec_constants=0;} key;
struct ShaderBytes {const u8* data=nullptr;size_t size=0;};
u32 shader_lookups=0;bool missing_shader=false;
ShaderBytes FindDxil(u64,u32){
 ++shader_lookups;
 if(missing_shader)return {};
 static const auto bytes=[] {
  std::array<u8,96> data{};
  const auto word=[&](size_t at,u32 value){std::memcpy(data.data()+at,&value,4);};
  std::memcpy(data.data(),"DXBC",4);word(28,1);word(32,36);
  std::memcpy(data.data()+36,"ISG1",4);word(40,52);word(44,1);
  word(56,40);data[77]=15;std::memcpy(data.data()+84,"POSITION",9);return data;
 }();
 return {bytes.data(),bytes.size()};
}
#define REXLOG_WARN(...) ((void)0)
struct Declaration {const Element* elements=nullptr;u32 element_count=0;u64 reversed_byte_elements=0,content_hash=0;
 u64 half_swapped_elements=0,half_swapped_wide_elements=0;};
bool RetainedDrawPreparationEnabled(){return std::getenv("LEGO_NATIVE_RETAINED_DRAW_STATE")==nullptr;}
struct {PreparedValue<std::array<u64,5>,VertexLayoutPlan> vertex_layout;} g_prepared_draw;
bool fsr_enabled=false,float_input=false,float_variant=false,position_variant=false;
bool NativeFsr3Enabled(){return fsr_enabled;}
bool FloatBlendIndicesInput(std::span<const Element>){return float_input;}
bool FindFloatBlendIndicesDxil(u64,u32){return float_variant;}
bool FindPositionPrecisionDxil(u64,u32){return position_variant;}
VertexInputUsageCache g_vertex_input_usage;
std::array<Buffer,5> sources;
std::vector<std::unique_ptr<Buffer>> uploads;
struct Request {u32 guest,offset,length,stride,phase;std::vector<u32> reversed;};
std::vector<Request> requests;
u32 full_resolves=0,translations=0,inspections=0;
BufferResourceView InspectBufferResource(u32 guest,BufferKind){assert(guest&&guest<sources.size());
 ++inspections;
 return {nullptr,u32(sources[guest].bytes.size()),0,guest*0x10000};}
BufferResourceView ResolveBufferResourceWindow(u32 guest,BufferKind,u32 offset,u32 length,const VertexByteOrder& order){
 assert(guest&&guest<sources.size());const auto& bytes=sources[guest].bytes;
 assert(offset<=bytes.size()&&length<=bytes.size()-offset);
 requests.push_back({guest,offset,length,order.stride,order.stream_offset,
                    {order.reversed_elements.begin(),order.reversed_elements.end()}});
 // Upload contents are a driver-boundary stand-in. The actual conversion,
 // generation validation and immutable lifetime bodies have their own fixture.
 auto b=std::make_unique<Buffer>();b->bytes.assign(bytes.begin()+offset,bytes.begin()+offset+length);
 auto* result=b.get();uploads.push_back(std::move(b));return {result,length,0,guest*0x10000+offset};
}
BufferResourceView ResolveBufferResourceView(u32 guest,BufferKind,const VertexByteOrder&){
 ++full_resolves;if(!guest)return {};assert(guest<sources.size());
 return {&sources[guest],u32(sources[guest].bytes.size()),0,guest*0x10000};
}
struct Memory {template<class T>T TranslateVirtual(u32){++translations;assert(false&&"nonindexed draws must not read indices");return nullptr;}} memory;
#define REX_KERNEL_MEMORY() (&memory)
struct Commands {
 std::array<plume::RenderVertexBufferView,4> views;
 u32 start=~0u,count=0,draws=0;
 void setVertexBuffers(u32 first,const plume::RenderVertexBufferView* v,u32 count,const plume::RenderInputSlot*){
  assert(first==0&&count==4);std::copy(v,v+count,views.begin());}
 void drawInstanced(u32 n,u32 instances,u32 s,u32 first){assert(instances==1&&first==0);++draws;count=n;start=s;}
};
Buffer zero;
// Instanced-fetch expansion is never planned for fixture draws.
struct ExpandedStreamStub{Buffer* buffer=nullptr;unsigned long long offset=0;unsigned length=0;};
template<class... A> ExpandedStreamStub ExpandInstancedFetchStream(const A&...){return {};}
template<class V,class A,class B>
void BindNativeDrawVertexBuffers(Commands*c,const V&,const A&views,const B&slots){
 c->setVertexBuffers(0,views.data(),views.size(),slots.data());
}
bool Draw(bool window_uploads,bool indexed,u32 primitive_type,u32 start,u32 count,i32 base_vertex,
          const Bindings& bindings,const Declaration& declaration,Commands* commands){
 std::array<plume::RenderVertexBufferView,kNativeVertexStreams> views;
 std::array<plume::RenderInputSlot,kNativeVertexStreams> slots;
 std::array<BufferResourceView,kNativeVertexStreams> vertex_buffers;
 auto* null_buffer=&zero;IndexDrawWindow index_window;BufferResourceView index_metadata;int draw_device=0;
 legodimensions::gpu_native::InstancedFetchPlan instanced_fetch;  // fixture draws are not instanced
''' + streams + '\n' + issue + r'''
 return use_windows;
}
void reset(){requests.clear();uploads.clear();full_resolves=0;inspections=0;}
int main(){
 zero.bytes.resize(256);
 for(u32 guest=1;guest<sources.size();++guest){sources[guest].bytes.resize(32768);
  for(u32 i=0;i<32768;++i)sources[guest].bytes[i]=u8((i*17+guest)%251);}
 Bindings bindings;bindings.vertex_streams[0]={1,28,12};bindings.vertex_streams[1]={2,16,32};
 bindings.vertex_streams[3]={3,16,0}; // Stale, unused stream must not be resolved.
 std::array<Element,3> elements{{{0,0,12},{0,16,4},{1,4,4}}};
 Declaration declaration{elements.data(),3,2,123};Commands commands;key.vertex_shader_hash=456;
 assert(Draw(true,false,6,123,7,INT32_MIN,bindings,declaration,&commands));
 assert(commands.start==0&&commands.count==7&&requests.size()==2&&full_resolves==0&&translations==0);
 assert(inspections==2); // Multiple attributes/window planning inspect each used stream once.
 assert(requests[0].offset==12+123*28&&requests[0].length==7*28&&requests[0].phase==0);
 assert(requests[0].reversed==std::vector<u32>{16});
 assert(requests[1].offset==32+123*16&&requests[1].length==7*16);
 assert(commands.views[2].address==zero.at(0)&&commands.views[3].address==zero.at(0));
 for(u32 slot=0;slot<2;++slot)for(u32 vertex=0;vertex<7;++vertex)
  for(const auto& e:elements)if(e.slotIndex==slot){const auto b=bindings.vertex_streams[slot];
   assert(std::memcmp(commands.views[slot].address+vertex*b.stride+e.alignedByteOffset,
    sources[b.buffer].at(b.offset+(123+vertex)*b.stride+e.alignedByteOffset),e.format)==0);}
 reset();commands={};assert(!Draw(false,false,6,123,7,0,bindings,declaration,&commands));
 assert(commands.start==123&&requests.empty()&&full_resolves==2);
 assert(shader_lookups==1); // The second draw used the real cached input mask.
 // A different draw must observe a replaced/shrunk resource, even at the same address.
 auto retained=sources[2].bytes;sources[2].bytes.resize(1);reset();commands={};
 assert(!Draw(true,false,6,123,7,0,bindings,declaration,&commands));
 assert(commands.draws==0&&requests.empty());assert(shader_lookups==1);sources[2].bytes=retained;
 reset();commands={};assert(Draw(true,false,13,123,8,0,bindings,declaration,&commands));
 assert(requests.size()==2&&full_resolves==0); // Actual quad issue/index expansion has its own fixture.
 // A bad later used stream disables the range path for EVERY used stream.
 for(u32 bad:{0u,3u,UINT32_MAX}){
  auto changed=bindings;changed.vertex_streams[1].stride=bad;reset();commands={};
  assert(!Draw(true,false,6,123,7,0,changed,declaration,&commands));
  if(bad==0)assert(commands.start==123&&requests.empty()&&full_resolves==2);
  else assert(commands.draws==0&&requests.empty()&&full_resolves==0);
 }
 auto changed=bindings;changed.vertex_streams[1].offset=32760;reset();commands={};
 assert(!Draw(true,false,6,0,1,0,changed,declaration,&commands)&&requests.empty());
 changed=bindings;changed.vertex_streams[0].offset=1;reset();commands={};
 assert(!Draw(true,false,6,0,1,0,changed,declaration,&commands)&&requests.empty());
 auto bad_elements=elements;bad_elements[2].alignedByteOffset=15;reset();commands={};
 assert(!Draw(true,false,6,0,1,0,bindings,{bad_elements.data(),3,2},&commands)&&requests.empty());
 for(auto range:{std::pair{0u,0u},std::pair{UINT32_MAX,2u},std::pair{1u,UINT32_MAX}}){
  reset();commands={};assert(!Draw(true,false,6,range.first,range.second,0,bindings,declaration,&commands));
  assert(requests.empty()&&(range.second==0?commands.start==range.first:commands.draws==0));
 }
 changed=bindings;changed.vertex_streams[1].buffer=0;reset();commands={};
 assert(Draw(true,false,6,123,7,0,changed,declaration,&commands));
 assert(requests.size()==1&&commands.views[1].address==zero.at(0)&&commands.start==0);
 // Actual signature reads POSITION only: keep declaration stream1 but do not
 // inspect, hash/convert/upload it or let its invalid stride disable windows.
 auto spare_elements=elements;spare_elements[2].semanticName="TEXCOORD";
 Declaration spare{spare_elements.data(),3,2,124};
 for(bool windows:{false,true}) {
  reset();commands={};assert(Draw(windows,false,6,123,7,0,bindings,spare,&commands)==windows);
  assert(commands.draws==1&&inspections==1&&full_resolves==(windows?0:1));
  assert(requests.size()==(windows?1:0));assert(commands.views[1].address==zero.at(0));
 }
 changed=bindings;changed.vertex_streams[1].stride=0;reset();commands={};
 assert(Draw(true,false,6,123,7,0,changed,spare,&commands)&&requests.size()==1);
 // Replays and alternate shaders retain all declaration streams; they must
 // not inherit a native base-program-only read mask.
 for(unsigned mode=0;mode<3;++mode){
  fsr_enabled=mode==0;float_input=float_variant=mode==1;position_variant=mode==2;
  reset();commands={};assert(Draw(true,false,6,123,7,0,bindings,spare,&commands));
  assert(requests.size()==2&&inspections==2&&commands.views[1].address!=zero.at(0));
 }
 fsr_enabled=float_input=float_variant=position_variant=false;
 // Declaration generations never reuse another layout's cached mask.
 reset();commands={};assert(Draw(true,false,6,123,7,0,bindings,declaration,&commands));
 assert(requests.size()==2&&inspections==2);
 key.vertex_shader_hash=789;missing_shader=true;
 reset();commands={};assert(Draw(true,false,6,123,7,0,bindings,spare,&commands));
 assert(requests.size()==2); // No bytecode means all declared streams, no retained absence.
 missing_shader=false;reset();commands={};
 assert(Draw(true,false,6,123,7,0,bindings,spare,&commands)&&requests.size()==1);
 auto invalid_slot=spare_elements;invalid_slot[2].slotIndex=kNativeVertexStreams;
 reset();commands={};assert(!Draw(true,false,6,123,7,0,bindings,
  {invalid_slot.data(),3,2,125},&commands)&&requests.empty()&&commands.draws==0);
 std::mt19937 random(123136);
 for(u32 i=0;i<10000;++i){const u32 start=random(),count=random();u32 last=0;
  const bool expected=count&&u64(start)+count-1<=UINT32_MAX;
  assert(NonIndexedVertexRange(start,count,last)==expected);
  if(expected)assert(last==u64(start)+count-1);}
}
'''
a.output.mkdir(parents=True, exist_ok=True)

def build(name, contents):
    cpp = a.output / (name + '.cpp')
    exe = a.output / (name + ('.exe' if os.name == 'nt' else ''))
    cpp.write_text(contents)
    subprocess.run([a.compiler, '-std=c++20', '-UNDEBUG', '-I' + str(root / 'rexlego/src'),
                    str(cpp), '-o', str(exe)], check=True, timeout=45)
    return exe.resolve()

subprocess.run([str(build('nonindexed-windows', code))], check=True, timeout=10)
negative = None
if a.verify_regression:
    uploaded_unused=code.replace('active_vertex_streams=shader_streams;', '(void)shader_streams;')
    assert uploaded_unused!=code
    result=subprocess.run([str(build('uploaded-unused-stream',uploaded_unused))],capture_output=True,text=True,timeout=10)
    assert result.returncode==23
    replay_mask=code.replace('required_inputs && !NativeFsr3Enabled()', 'bool(required_inputs)')
    assert replay_mask!=code
    result=subprocess.run([str(build('unsafe-replay-mask',replay_mask))],capture_output=True,text=True,timeout=10)
    assert result.returncode==23
    ignored=code.replace('return used;', 'return u64{0};',1)
    assert ignored!=code
    result=subprocess.run([str(build('ignored-cached-inputs',ignored))],capture_output=True,text=True,timeout=10)
    assert result.returncode==23
    uncached=code.replace('if (!vertex_metadata_inspected[i]) {','if (true) {')
    assert uncached!=code
    result=subprocess.run([str(build('repeated-metadata-inspection',uncached))],capture_output=True,text=True,timeout=10)
    assert result.returncode==23
    old = code.replace('use_windows = window_uploads;', 'use_windows = false;', 1)
    assert old != code
    result = subprocess.run([str(build('indexed-only', old))], capture_output=True, text=True, timeout=10)
    assert result.returncode != 0
    negative = 'original indexed-only eligibility fails real nonindexed production selection'
hlsl = []
if a.shader_hlsl:
    for path in sorted(a.shader_hlsl.glob('vs-*.hlsl')):
        data = path.read_bytes()
        assert b'SV_VertexID' not in data and b'SV_InstanceID' not in data, path
        hlsl.append({'path': str(path.resolve()), 'sha256': hashlib.sha256(data).hexdigest()})
    assert hlsl, 'No captured vertex HLSL found'
(a.output / 'verification.json').write_text(json.dumps({
    'result': 'PASS', 'production_stream_body_sha256': hashlib.sha256(streams.encode()).hexdigest(),
    'production_issue_statement': issue.strip(), 'negative_control': negative,
    'vertex_hlsl_without_id_semantics': hlsl,
    'checks': ['all used streams', 'unused/missing streams', 'bounds/alignment/overflow fallback',
               'packed reversal origin', 'nonindexed base ignored', 'first/last attribute equivalence',
               'default flag unchanged', 'QuadList stream eligibility', '10000 widened-range cases',
               'cached actual input mask still rejects a later resource shrink',
               'unused declared stream avoids metadata/upload with windows on/off',
               'FSR/float-index/position variants retain conservative uploads',
               'unused stride cannot disable used-stream windows'],
    'boundaries': 'Actual draw selection/binding/issue snippets and range helpers; fake upload driver. '
                  'Run buffer_variants and draw_upload for actual conversion/invalidation/fence lifetime bodies.'
}, indent=2) + '\n')
print('PASS production nonindexed selection/binding/rebase, all used streams and bounds')
