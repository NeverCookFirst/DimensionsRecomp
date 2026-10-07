"""Execute actual stream selection and QuadList expansion with real byte converters."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess


def block(text, signature):
    start = text.index(signature)
    opening = text.index('{', start)
    depth, end = 1, opening + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


p = argparse.ArgumentParser(description=__doc__)
p.add_argument('output', type=Path)
p.add_argument('--compiler', default='clang++')
p.add_argument('--verify-regression', action='store_true')
p.add_argument('--shader-hlsl', type=Path)
a = p.parse_args()
root = Path(__file__).resolve().parents[2]
draw = (root / 'rexlego/src/gpu_native/draw.cpp').read_text()
buffers = (root / 'rexlego/src/gpu_native/buffers.cpp').read_text()
begin = draw.index('  BufferResourceView index_metadata;', draw.index('bool DispatchDraw('))
preflight = draw[begin:draw.index('  timing.Next();', begin)]
begin = draw.index('  std::array<VertexBufferWindow, kNativeVertexStreams> vertex_windows{};')
streams = draw[begin:draw.index('  if (mesh_trace) {', begin)]
quad = block(draw, '  if (primitive_type == static_cast<u32>(rex::graphics::xenos::PrimitiveType::kQuadList)) {')
swap = block(buffers, 'void ByteSwapElements(')
code = r'''
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <random>
#include <string>
#include <vector>
#include "gpu_native/buffer_window.h"
#include "gpu_native/index_draw.h"
#include "gpu_native/vertex_upload.h"
using namespace legodimensions::gpu_native;
using u8=uint8_t;using u16=uint16_t;using u32=uint32_t;using u64=uint64_t;using i32=int32_t;
struct be_u16 {u8 bytes[2];operator u16()const{return u16(u16(bytes[0])<<8|bytes[1]);}};
struct be_u32 {u8 bytes[4];operator u32()const{return u32(bytes[0])<<24|u32(bytes[1])<<16|u32(bytes[2])<<8|bytes[3];}};
constexpr u32 kNativeVertexStreams=4;
namespace rex::graphics::xenos {enum class PrimitiveType:u32{kQuadList=13};}
enum class BufferKind{kVertex,kIndex};
struct Buffer;struct BufferAddress {Buffer* buffer;u32 offset;};
struct Buffer {
 std::vector<u8> bytes;bool fail_map=false;u32 maps=0,unmaps=0;
 BufferAddress at(u32 n){assert(n<=bytes.size());return {this,n};}
 void* map(){++maps;return fail_map?nullptr:bytes.data();}void unmap(){++unmaps;}
};
namespace plume {
using RenderBuffer=Buffer;
enum class RenderHeapType{UPLOAD};enum class RenderFormat{R32_UINT};
struct RenderBufferDesc {u64 size;static auto IndexBuffer(u64 size,RenderHeapType){return RenderBufferDesc{size};}};
struct RenderIndexBufferView {Buffer* buffer;u64 length;
 RenderIndexBufferView(BufferAddress b,u64 n,RenderFormat):buffer(b.buffer),length(n){assert(b.offset==0);}};
struct RenderVertexBufferView {Buffer* buffer=nullptr;u32 offset=0,length=0;
 RenderVertexBufferView()=default;RenderVertexBufferView(BufferAddress b,u32 n):buffer(b.buffer),offset(b.offset),length(n){}};
struct RenderInputSlot {RenderInputSlot()=default;RenderInputSlot(u32,u32){}};
u32 RenderFormatSize(u32 n){return n;}
}
struct BufferResourceView {Buffer* buffer=nullptr;u32 length=0,guest_format=0,mirror_address=0;};
struct Binding {u32 buffer=0,stride=0,offset=0;};
struct Bindings {std::array<Binding,4> vertex_streams{};u32 index_buffer=3;};
struct Element {u32 slotIndex=0,alignedByteOffset=0,format=0;};
struct Declaration {const Element* elements;u32 element_count;u64 reversed_byte_elements;};
std::array<Buffer,5> sources;
u32 index_format=1;bool short_index_view=false,fail_index_map=false,fail_output_map=false,fail_allocation=false;
std::vector<std::unique_ptr<Buffer>> uploads;
std::vector<std::shared_ptr<Buffer>> retired;
std::vector<std::string> operations;
u64 uploaded_vertex_bytes=0;u32 window_calls=0,full_calls=0;
''' + swap + r'''
auto Upload(u32 guest,BufferKind kind,u32 offset,u32 size,const VertexByteOrder& order) {
 assert(guest&&guest<sources.size());const auto& bytes=sources[guest].bytes;
 assert(offset<=bytes.size()&&size<=bytes.size()-offset);
 auto b=std::make_unique<Buffer>();b->bytes.resize(size);
 const auto canonical=CanonicalVertexByteOrder(order);
 const bool fused=kind==BufferKind::kVertex&&WriteAlignedVertexUpload(b->bytes.data(),bytes.data()+offset,size,canonical);
 if(!fused){ByteSwapElements(b->bytes.data(),bytes.data()+offset,size,kind==BufferKind::kIndex&&index_format==1?2:4);
  if(kind==BufferKind::kVertex)assert(ApplyVertexByteOrder(b->bytes.data(),size,canonical));}
 if(kind==BufferKind::kVertex)uploaded_vertex_bytes+=size;
 b->fail_map=kind==BufferKind::kIndex&&fail_index_map;
 auto* result=b.get();uploads.push_back(std::move(b));
 return BufferResourceView{result,short_index_view&&kind==BufferKind::kIndex?1u:size,
     kind==BufferKind::kIndex?index_format:0u,(guest<<24)+offset};
}
BufferResourceView InspectBufferResource(u32 guest,BufferKind kind){assert(guest&&guest<sources.size());
 return {nullptr,u32(sources[guest].bytes.size()),kind==BufferKind::kIndex?index_format:0u,guest<<24};}
BufferResourceView ResolveBufferResourceView(u32 guest,BufferKind kind,const VertexByteOrder& order={}){
 ++full_calls;if(!guest)return {};return Upload(guest,kind,0,u32(sources[guest].bytes.size()),order);}
BufferResourceView ResolveBufferResourceWindow(u32 guest,BufferKind kind,u32 offset,u32 length,const VertexByteOrder& order={}){
 ++window_calls;return Upload(guest,kind,offset,length,order);}
struct Memory {template<class T>T TranslateVirtual(u32 address){const u32 guest=address>>24,offset=address&0xFFFFFF;
 assert(guest&&guest<sources.size()&&offset<sources[guest].bytes.size());
 return reinterpret_cast<T>(sources[guest].bytes.data()+offset);}} memory;
#define REX_KERNEL_MEMORY() (&memory)
struct Device {auto createBuffer(plume::RenderBufferDesc desc){
 if(fail_allocation)return std::unique_ptr<Buffer>{};auto b=std::make_unique<Buffer>();
 assert(desc.size<1024*1024);b->bytes.resize(desc.size);b->fail_map=fail_output_map;return b;}} device;
struct HostDevice {static auto Device(){return &device;}
 static void RetireResource(std::shared_ptr<Buffer> b){operations.emplace_back("retire");retired.push_back(std::move(b));}};
struct Commands {
 std::array<plume::RenderVertexBufferView,4> views;
 std::vector<u32> indices;u32 draws=0,count=0;i32 base=0;
 void setVertexBuffers(u32 first,const plume::RenderVertexBufferView* v,u32 n,const plume::RenderInputSlot*){
 assert(first==0&&n==4);std::copy(v,v+n,views.begin());}
 void setIndexBuffer(const plume::RenderIndexBufferView* view){
 assert(view&&view->buffer&&view->length%4==0);indices.resize(view->length/4);
 std::memcpy(indices.data(),view->buffer->bytes.data(),view->length);}
 void drawIndexedInstanced(u32 n,u32 instances,u32 start,i32 b,u32 first){
 assert(instances==1&&start==0&&first==0&&n==indices.size());++draws;count=n;base=b;operations.emplace_back("issue");}
};
struct QueryDrawScope {explicit QueryDrawScope(Commands*){operations.emplace_back("query_begin");}
 ~QueryDrawScope(){operations.emplace_back("query_end");}};
Buffer zero;
bool Draw(bool window_uploads,bool indexed,u32 start,u32 count,i32 base_vertex,
          const Bindings& bindings,const Declaration& declaration,Commands* commands,bool& window_result){
 const u32 primitive_type=13;
''' + preflight + r'''
 std::array<plume::RenderVertexBufferView,kNativeVertexStreams> views;
 std::array<plume::RenderInputSlot,kNativeVertexStreams> slots;
 std::array<BufferResourceView,kNativeVertexStreams> vertex_buffers;
 auto* null_buffer=&zero;
''' + streams + r'''
 window_result=use_windows;
''' + quad + r'''
 return true;
}
void Reset(){uploads.clear();retired.clear();operations.clear();uploaded_vertex_bytes=0;window_calls=full_calls=0;
 short_index_view=fail_index_map=fail_output_map=fail_allocation=false;}
void SetIndices(u32 format,u32 start,const std::vector<u32>& indices){
 index_format=format;const u32 size=format==1?2:4;sources[3].bytes.assign((start+indices.size()+2)*size,0xCC);
 for(u32 i=0;i<indices.size();++i)for(u32 byte=0;byte<size;++byte)
  sources[3].bytes[(start+i)*size+byte]=u8(indices[i]>>(8*(size-1-byte)));
}
struct Result {std::vector<std::vector<u8>> attributes;std::vector<u32> expanded;u64 bytes;bool windows;};
Result Run(bool flag,bool indexed,u32 start,u32 count,i32 base,const Bindings& bindings,const Declaration& decl){
 Reset();Commands commands;bool windows=false;assert(Draw(flag,indexed,start,count,base,bindings,decl,&commands,windows));
 assert(commands.draws==1&&commands.count==count/4*6);
 assert(operations==std::vector<std::string>({"query_begin","issue","query_end","retire"}));
 assert(retired.size()==1);const auto retained=retired.front();assert(retained->unmaps==1);
 Result result{{},commands.indices,uploaded_vertex_bytes,windows};
 for(u32 index:commands.indices){const int64_t vertex=int64_t(index)+commands.base;assert(vertex>=0);
  for(u32 e=0;e<decl.element_count;++e){const auto element=decl.elements[e];const auto binding=bindings.vertex_streams[element.slotIndex];
   if(!binding.buffer)continue;const auto view=commands.views[element.slotIndex];
   const u64 offset=u64(vertex)*binding.stride+element.alignedByteOffset;
   assert(offset+element.format<=view.length);
   result.attributes.emplace_back(view.buffer->bytes.begin()+view.offset+offset,
                                  view.buffer->bytes.begin()+view.offset+offset+element.format);
  }}
 // Expanded upload stays retained beyond query end; no actual GPU-fence claim.
 assert(retained->bytes.size()==commands.indices.size()*4);
 return result;
}
int main(){
 zero.bytes.resize(256);
 for(u32 guest:{1u,2u,4u}){sources[guest].bytes.resize(65536);
  for(u32 i=0;i<65536;++i)sources[guest].bytes[i]=u8((i*17+guest)%251);}
 Bindings bindings;bindings.vertex_streams[0]={1,28,12};bindings.vertex_streams[1]={2,36,20};
 bindings.vertex_streams[3]={4,0,1}; // Bound stale unused stream.
 std::array<Element,5> elements{{{0,0,12},{0,16,4},{1,0,12},{1,12,4},{2,0,4}}};
 Declaration decl{elements.data(),u32(elements.size()),(u64{1}<<1)|(u64{1}<<3)};
 for(u32 count:{4u,8u,20u,64u})for(u32 start:{0u,3u,97u}){
  const auto old=Run(false,false,start,count,INT32_MIN,bindings,decl);
  const auto next=Run(true,false,start,count,INT32_MIN,bindings,decl);
  assert(!old.windows&&next.windows&&old.attributes==next.attributes&&next.bytes<old.bytes);
  assert(next.bytes==u64(count)*(28+36));
  for(u32 i=0;i<next.expanded.size();++i)assert(old.expanded[i]==next.expanded[i]+start);
 }
 for(u32 format:{1u,0u})for(i32 base:{-7,0,11}){
  const std::vector<u32> indices{14,17,18,15,16,13,12,19};SetIndices(format,3,indices);
  const auto old=Run(false,true,3,8,base,bindings,decl);
  const auto next=Run(true,true,3,8,base,bindings,decl);
  assert(next.windows&&!old.windows&&old.attributes==next.attributes&&old.expanded==next.expanded);
  assert(next.bytes==8*(28+36));
 }
 SetIndices(0,2,{0x80000010,0x80000011,0x80000012,0x80000013});
 auto high=Run(true,true,2,4,INT32_MIN,bindings,decl);assert(!high.windows);
 // Actual captured paused UI QuadList offsets, strides and vertex counts.
 for(auto shape:std::array<std::array<u32,3>,8>{{{224,20,4},{468,36,24},{1476,36,20},
     {2364,36,24},{3228,36,28},{4236,36,64},{6540,36,24},{7404,36,32}}}){
  auto ui=bindings;ui.vertex_streams[0]={1,shape[1],shape[0]};ui.vertex_streams[1]={};
  auto ui_elements=elements;ui_elements[0]={0,0,12};ui_elements[1]={0,16,4};
  Declaration ui_decl{ui_elements.data(),2,u64{1}<<1};
  const auto old=Run(false,false,0,shape[2],0,ui,ui_decl),next=Run(true,false,0,shape[2],0,ui,ui_decl);
  assert(next.windows&&old.attributes==next.attributes&&next.bytes==u64(shape[1])*shape[2]);
 }
 // Whole-draw fallback retains original bytes for valid layouts that cannot
 // use an aligned range. A synthetic zero stream never disables other streams.
 for(u32 offset:{1u,3u}){auto changed=bindings;changed.vertex_streams[1].offset=offset;
  const auto old=Run(false,false,7,8,0,changed,decl),next=Run(true,false,7,8,0,changed,decl);
  assert(!next.windows&&old.attributes==next.attributes&&next.bytes==old.bytes);}
 auto changed=bindings;changed.vertex_streams[1].buffer=0;
 assert(Run(true,false,7,8,0,changed,decl).windows);
 for(u32 invalid_stride:{0u,3u,UINT32_MAX}){changed=bindings;changed.vertex_streams[1].stride=invalid_stride;
  Reset();Commands commands;bool windows=false;
  // Only selection/binding is examined for malformed layouts; do not simulate
  // a GPU read of invalid bytes. Packed conversion may reject these layouts.
  auto constant_elements=elements;constant_elements[3].slotIndex=0;
  // Keep packed conversions off the invalid-stride stream so the actual
  // whole-buffer fallback is observable without a simulated GPU fetch.
  Draw(true,false,0,4,0,changed,{constant_elements.data(),3,2},&commands,windows);assert(!windows);
 }
 for(u32 count:{0u,3u,0xFFFFFFFCu}){Reset();Commands commands;bool windows=false;
  assert(!Draw(false,false,0,count,0,bindings,decl,&commands,windows));assert(retired.empty()&&operations.empty());}
 Reset();Commands commands;bool windows=false;
 assert(!Draw(false,false,UINT32_MAX,4,0,bindings,decl,&commands,windows));
 SetIndices(1,3,{14,17,18,15,16,13,12,19});
 for(unsigned failure=0;failure<4;++failure){Reset();commands={};windows=false;
  short_index_view=failure==0;fail_allocation=failure==1;fail_output_map=failure==2;fail_index_map=failure==3;
  assert(!Draw(true,true,3,8,0,bindings,decl,&commands,windows));
  assert(commands.draws==0&&retired.empty()&&operations.empty());
  for(const auto& upload:uploads)if(upload->maps&&!upload->fail_map)assert(upload->unmaps==upload->maps);
 }
 std::mt19937 random(15213);
 for(unsigned i=0;i<10000;++i){const u32 count=random();const u64 expanded=u64(count/4)*6;
  const auto expected=count&&count%4==0&&expanded<=UINT32_MAX?u32(expanded):0;
  assert(ExpandedQuadIndexCount(count)==expected);}
 std::cout<<"PASS actual QuadList stream/vertex-byte/index expansion equivalence, queries and retained upload order\n";
}
'''
a.output.mkdir(parents=True, exist_ok=True)


def build(name, contents):
    cpp = a.output / (name + '.cpp')
    exe = a.output / (name + '.exe')
    cpp.write_text(contents)
    subprocess.run([a.compiler, '-std=c++20', '-UNDEBUG', '-I' + str(root / 'rexlego/src'),
                    str(cpp), '-o', str(exe)], check=True, timeout=45)
    return exe.resolve()


subprocess.run([str(build('quad-windows', code))], check=True, timeout=10)
negative = None
if a.verify_regression:
    old = code.replace('if (window_uploads && count) {',
                       'if (window_uploads && count && primitive_type != 13) {', 1)
    assert old != code
    result = subprocess.run([str(build('old-quad-exclusion', old))],
                            capture_output=True, text=True, timeout=10)
    assert result.returncode != 0 and 'next.windows' in result.stderr, result.stderr
    negative = 'Original QuadList exclusion fails actual full-vs-range attribute-byte equality case window expectation'
hlsl = []
if a.shader_hlsl:
    for path in sorted(a.shader_hlsl.glob('vs-*.hlsl')):
        data = path.read_bytes()
        assert b'SV_VertexID' not in data and b'SV_InstanceID' not in data, path
        hlsl.append({'path': str(path.resolve()), 'sha256': hashlib.sha256(data).hexdigest()})
    assert hlsl
(a.output / 'verification.json').write_text(json.dumps({
    'result': 'PASS', 'production_sha256': {
        'preflight': hashlib.sha256(preflight.encode()).hexdigest(),
        'stream_selection_binding': hashlib.sha256(streams.encode()).hexdigest(),
        'quad_expansion_submission': hashlib.sha256(quad.encode()).hexdigest(),
        'ByteSwapElements': hashlib.sha256(swap.encode()).hexdigest()},
    'negative_control': negative, 'vertex_hlsl_without_id_semantics': hlsl,
    'checks': ['nonindexed start and ignored base', 'indexed16/32 nonzero start and signed base',
               'all expanded-index attribute bytes', 'packed field phase', 'multi/unused/synthetic streams',
               'eight actual paused UI shapes', 'alignment fallback', 'INT32MAX index fallback',
               'count/start overflow', '10000 expanded-count cases', 'source view/allocation/map failures',
               'query scope and retirement order'],
    'boundaries': 'Actual draw preflight/window/binding/QuadList bodies, ByteSwapElements and real vertex upload helpers; '
                  'mock buffer adoption/driver and retained uploads. No GPU fence or runtime speed claim.'}, indent=2) + '\n')
