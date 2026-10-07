"""Exercise the real bounded draw diagnostic reader, recorder and trigger."""
import argparse
from pathlib import Path
import subprocess
import tempfile


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
    args.output.mkdir(parents=True, exist_ok=True)
    root = Path(__file__).resolve().parents[2]
    source = (root / "rexlego/src/gpu_native/draw.cpp").read_text()
    reader = function(source, "bool ReadDrawDiagnosticBytes(")
    device_reader = function(source, "std::optional<u32> ReadDrawDiagnosticDeviceWord(")
    recorder = function(source, "struct CpuDrawDiagnostic {") + ";"
    # Check that the production integration reaches the recorder before draw
    # rejection, publishes the real layout, and sets success only at submission.
    dispatch = function(source, "bool DispatchDraw(")
    assert dispatch.index("BeginCpuDrawDiagnostic(") < dispatch.index("ShouldSkipBoundPixelShader()")
    assert "if (cpu_trace) cpu_trace->Layout(declaration);" in dispatch
    assert dispatch.index("cpu_trace->submitted = true") > dispatch.index("drawIndexedInstanced(")
    harness = r'''
#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <vector>
#include "gpu_native/draw_diagnostic.h"
using namespace legodimensions::gpu_native;
using u8=uint8_t;using u32=uint32_t;using u64=uint64_t;using i32=int32_t;using i64=int64_t;
template<class T> struct BigEndian {
  std::array<u8,sizeof(T)> data{};
  BigEndian& operator=(T value) {
    for(size_t i=0;i<sizeof(T);++i)data[sizeof(T)-1-i]=u8(value>>(i*8));return *this;
  }
  operator T() const {T value=0;for(auto b:data)value=T((value<<8)|b);return value;}
};
using be_u16=BigEndian<uint16_t>;using be_u32=BigEndian<u32>;
namespace rex::memory {
constexpr u32 kMemoryAllocationCommit=1,kMemoryProtectRead=1;
struct HeapAllocationInfo {u32 allocation_base=0,allocation_size=0,region_size=0,state=0,protect=0;};
}
struct Heap {
 u32 size=4096,base=0x10000,readable_pages=1;
 u32 page_size()const{return size;}u32 heap_base()const{return base;}
 bool QueryRegionInfo(u32 page,rex::memory::HeapAllocationInfo* info) {
  assert(page>=base&&(page-base)%size==0);
  if(page>=uint64_t(base)+2*size)return false;
  *info={base,2*size,size,1,u32((page-base)/size<readable_pages)};return true;
 }
};
struct Memory {
 Heap heap;std::vector<u8> bytes=std::vector<u8>(8192);u32 translations=0;
 Heap* LookupHeap(u32 address) {
  return address>=heap.base&&uint64_t(address)<uint64_t(heap.base)+2*heap.size?&heap:nullptr;
 }
 template<class T>T TranslateVirtual(u32 address) {
  ++translations;assert(address>=heap.base);return reinterpret_cast<T>(bytes.data()+address-heap.base);
 }
 u8* virtual_membase(){return reinterpret_cast<u8*>(reinterpret_cast<uintptr_t>(bytes.data())-heap.base);}
 void Configure(u32 size,u32 base) {heap={size,base,1};bytes.assign(2*size,0);translations=0;}
 void Word(u32 offset,u32 value) {for(u32 i=0;i<4;++i)bytes[offset+i]=u8(value>>(24-i*8));}
} fixture_memory;
#define REX_KERNEL_MEMORY() (&::fixture_memory)
''' + reader + r'''
constexpr u32 kNativeVertexStreams=16;
enum class BufferKind{kVertex,kIndex};
struct BufferResourceView {void* buffer=nullptr;u32 length=0,guest_format=0,mirror_address=0;};
struct Stream {u32 buffer=0,offset=0,stride=0;};
struct DrawBindings {u32 vertex_declaration=0,index_buffer=0,depth_stencil=0;
 std::array<u32,4> render_targets{};std::array<Stream,16> vertex_streams{};};
struct D3DBuffer {u8 bytes[32];};
struct D3DDevice {u8 prefix[0x2FD0];be_u32 vertex_declaration;u8 suffix[0x6080-0x2FD4];};
namespace plume {
enum class RenderFormat:u32 {FLOAT3=1,COLOR=2};
u32 RenderFormatSize(RenderFormat format){return format==RenderFormat::FLOAT3?12:4;}
struct RenderInputElement {const char* semanticName;u32 semanticIndex,slotIndex,alignedByteOffset;RenderFormat format;};
}
struct VertexDeclarationView {const plume::RenderInputElement* elements=nullptr;u32 element_count=0;
 bool supported=true;u32 swapped_positions=0,swapped_normals=0,swapped_texcoords=0,sint_texcoords=0;
 u64 reversed_byte_elements=0;};
DrawBindings bindings;
DrawBindings SnapshotDrawBindings(){return bindings;}
enum class ShaderStage{kVertex,kPixel};
u64 BoundShaderHash(ShaderStage stage){return stage==ShaderStage::kVertex?0x1234:0xabcd;}
std::atomic<u32> g_probe_frame{8};
std::map<u32,BufferResourceView> metadata;
BufferResourceView InspectBufferResource(u32 address,BufferKind){return metadata.at(address);}
template<class Words>std::string LongProbeHex(const Words& words) {
 std::ostringstream s;s<<std::hex<<std::setfill('0');for(auto w:words)s<<std::setw(8)<<u32(w)<<',';return s.str();
}
''' + device_reader + '\n' + recorder + r'''
void Touch(const std::filesystem::path& path,std::string_view text="") {
 std::ofstream file(path,std::ios::binary);file<<text;assert(file.good());
}
std::string Read(const std::filesystem::path& path){std::ifstream f(path,std::ios::binary);return {std::istreambuf_iterator<char>(f),{}};}
int main(int argc,char**argv){
 assert(argc==2);const std::filesystem::path directory=argv[1];
 for(u32 size:{4096u,65536u,16777216u}) {
  // A non-page-aligned heap base catches incorrect absolute/4KiB alignment.
  fixture_memory.Configure(size,0x11000);
  std::array<u8,16> sample{};const auto base=fixture_memory.heap.base;
  fixture_memory.bytes[size-1]=0x71;
  assert(ReadDrawDiagnosticBytes(base+size-1,sample.data(),1)&&sample[0]==0x71);
  const u32 translated=fixture_memory.translations;
  assert(!ReadDrawDiagnosticBytes(base+size-1,sample.data(),2));
  assert(!ReadDrawDiagnosticBytes(base+size,sample.data(),1));
  assert(fixture_memory.translations==translated);
  fixture_memory.heap.readable_pages=2;
  assert(ReadDrawDiagnosticBytes(base+size-1,sample.data(),2));
 }
 std::array<u8,800> sample{};
 assert(!ReadDrawDiagnosticBytes(0,sample.data(),1));
 assert(!ReadDrawDiagnosticBytes(0xFFFFFFF8,sample.data(),16));
 assert(!ReadDrawDiagnosticBytes(fixture_memory.heap.base,sample.data(),769));
 assert(!ReadDrawDiagnosticBytes(fixture_memory.heap.base,nullptr,1));
 assert(!ReadDrawDiagnosticBytes(fixture_memory.heap.base,sample.data(),0));
 assert(DrawDiagnosticVertexOffset(2,16,4,8,4,48)==44);
 assert(!DrawDiagnosticVertexOffset(-1,16,0,0,4,48));
 assert(!DrawDiagnosticVertexOffset(INT64_MAX,UINT32_MAX,0,0,4,UINT32_MAX));
 assert(!DrawDiagnosticVertexOffset(INT64_MAX,2,UINT32_MAX,UINT32_MAX,4,UINT32_MAX));
 assert(!DrawDiagnosticVertexOffset(2,16,4,8,5,48));
 assert(!DrawDiagnosticVertexOffset(0,0,0,0,4,48));
 assert(!DrawDiagnosticVertexOffset(0,16,0,0,0,48));
 using S=DrawDiagnosticSession;
 assert(S::ParseTrigger("")==0&&S::ParseTrigger("skip=0")==0);
 assert(S::ParseTrigger("skip=100\r\n")==100&&S::ParseTrigger("skip=4096")==4096);
 for(std::string_view invalid:{"skip=","skip=-1","skip=+1","skip=4097","skip=1x",
   "skip=1 "," skip=1","skip=42949672960","skip=1\n\n","100"})assert(!S::ParseTrigger(invalid));
 const auto disabled_trigger=directory/"disabled";Touch(disabled_trigger);
 S disabled;assert(!disabled.Begin(1));assert(std::filesystem::exists(disabled_trigger));
 const auto trigger=directory/"draw trigger";S session(trigger);
 assert(!session.Begin(7));Touch(trigger);assert(!session.Begin(7)); // no second poll/frame
 assert(!session.Begin(8));assert(!std::filesystem::exists(trigger));assert(!session.Begin(8));
 for(u32 i=1;i<=64;++i){assert(session.Begin(9)==i);session.Write("draw\n");}
 assert(!session.Begin(9));Touch(trigger);assert(!session.Begin(10));
 assert(std::filesystem::exists(trigger));assert(session.written()==64*5);
 const auto skip_trigger=directory/"late";Touch(skip_trigger,"skip=100\n");S late(skip_trigger);
 assert(!late.Begin(UINT32_MAX));
 for(u32 i=0;i<100;++i)assert(!late.Begin(0));
 assert(late.Begin(0)==1&&late.attempted_ordinal()==101);late.Write("late\n");
 assert(!late.Begin(1)); // cannot spill into another frame
 const auto bad_trigger=directory/"bad";Touch(bad_trigger,std::string(40,'0'));S bad(bad_trigger);
 assert(!bad.Begin(1));assert(!bad.Begin(2)); // truncated input must not arm
 Touch(bad_trigger,"skip=0");assert(!bad.Begin(3));assert(bad.Begin(4)==1);bad.Write("valid\n");
 const auto budget_trigger=directory/"budget";Touch(budget_trigger);S budget(budget_trigger);
 assert(!budget.Begin(1));
 const std::string block(S::kMaxRecordBytes,'x');
 for(u32 i=1;i<=64;++i){assert(budget.Begin(2)==i);budget.Write(block);budget.Write(block);}
 assert(budget.written()==S::kMaxBytes);
 assert(std::filesystem::file_size(budget.output())==S::kMaxBytes);
 const auto large_trigger=directory/"large";Touch(large_trigger);S large(large_trigger);
 assert(!large.Begin(1));assert(large.Begin(2)==1);large.Write(block+"x");
 assert(Read(large.output())=="record_truncated\n");
 // Feed actual production recorder known big-endian indices/position/color.
 fixture_memory.Configure(65536,0x10000);
 auto* device=fixture_memory.TranslateVirtual<D3DDevice*>(0x12000);
 fixture_memory.Word(0x2000+offsetof(D3DDevice,vertex_declaration),0x10080);
 fixture_memory.Word(0x2000+10564,0x90000);fixture_memory.Word(0x2000+10572,0x400);
 fixture_memory.Word(0x2000+10500,0x3b808081);fixture_memory.Word(0x2000+10620,0x3f800000);
 fixture_memory.Word(0x2000+13032,0x44A00000);fixture_memory.Word(0x2000+13036,0x44340000);
 fixture_memory.Word(0x2000+0x780+48*16,0x3f800000);
 fixture_memory.Word(0x2000+0x1780+4*16+12,0x3f000000);
 fixture_memory.Word(0x2000+0x1780+45*16,0x3f800000);
 fixture_memory.Word(128+24,2);for(u32 i=0;i<24;++i)fixture_memory.bytes[128+52+i]=u8(i);
 fixture_memory.bytes[1024]=0;fixture_memory.bytes[1025]=3;
 fixture_memory.bytes[1026]=0;fixture_memory.bytes[1027]=1;
 fixture_memory.bytes[1028]=0;fixture_memory.bytes[1029]=2;
 bindings={};bindings.index_buffer=0x10200;bindings.vertex_streams[0]={0x10300,4,16};
 metadata[0x10200]={nullptr,6,1,0x10400};metadata[0x10300]={nullptr,128,0,0x10500};
 // base -1 + index3 => vertex2; offset4 + 2*16 =36, color offset+12.
 fixture_memory.Word(1280+36,0x3f800000);fixture_memory.Word(1280+48,0xff112233);
 const plume::RenderInputElement elements[]={{"POSITION",0,0,0,plume::RenderFormat::FLOAT3},
  {"COLOR",0,0,12,plume::RenderFormat::COLOR}};
 VertexDeclarationView decl;decl.elements=elements;decl.element_count=2;
 const auto recorder_trigger=directory/"recorder";Touch(recorder_trigger);S trace(recorder_trigger);
 assert(!trace.Begin(7));const u32 id=trace.Begin(8);assert(id);
 {CpuDrawDiagnostic draw(trace,id,device,6,true,0,3,-1);draw.Layout(decl);draw.submitted=true;}
 auto text=Read(trace.output());
 for(std::string_view expected:{"attempted_ordinal=1","VS=1234 PS=abcd","raw_count=2",
  "raw_decl_bytes=000102030405060708090a0b0c0d0e0f1011121314151617",
  "clip_control=90000 viewport_control=400", "viewport_width_bits=44a00000 viewport_height_bits=44340000",
  "alpha_ref_bits=3b808081 legacy_float10620_bits=3f800000",
  "VS_C48=3f800000,00000000,00000000,00000000,",
  "PS_C4=00000000,00000000,00000000,3f000000,",
  "PS_C45=3f800000,00000000,00000000,00000000,",
  "first_indices=3,1,2,","host_attr=COLOR0","vertex=2 source=10524 bytes=3f800000",
  "vertex=2 source=10530 bytes=ff112233","submitted=1"})assert(text.find(expected)!=std::string::npos);
 // Rejected/invalid indices must not cause fabricated vertex samples.
 metadata[0x10200].mirror_address=0x20000;
 {CpuDrawDiagnostic draw(trace,trace.Begin(8),device,6,true,0,3,-1);draw.Layout(decl);}
 text=Read(trace.output());assert(text.find("first_indices=unreadable,unreadable,unreadable,")!=std::string::npos);
 assert(text.find("submitted=0")!=std::string::npos);
 // The fourth non-indexed quad corner is essential to distinguish perimeter
 // from strip ordering. Capture it through the real recorder with the same
 // twelve-sample cap; index/base handling above remains independently tested.
 fixture_memory.Word(1280+52,0x3f000000);fixture_memory.Word(1280+56,0xbf000000);
 fixture_memory.Word(1280+60,0);fixture_memory.Word(1280+64,0xffaabbcc);
 {CpuDrawDiagnostic draw(trace,trace.Begin(8),device,6,false,0,4,0);draw.Layout(decl);}
 text=Read(trace.output());
 assert(text.find("vertex=3 source=10534 bytes=3f000000bf00000000000000")!=std::string::npos);
 assert(text.find("vertex=3 source=10540 bytes=ffaabbcc")!=std::string::npos);
 // Non-null device pointers at a protected boundary, outside the guest
 // range, and zero-count early rejects must never be dereferenced by capture.
 auto* truncated=fixture_memory.TranslateVirtual<D3DDevice*>(0x1FFF8);
 const u32 translated=fixture_memory.translations;
 {CpuDrawDiagnostic draw(trace,trace.Begin(8),truncated,6,false,0,0,0);}
 assert(fixture_memory.translations==translated);
 auto* outside=reinterpret_cast<D3DDevice*>(reinterpret_cast<uintptr_t>(fixture_memory.virtual_membase())-1);
 {CpuDrawDiagnostic draw(trace,trace.Begin(8),outside,6,false,0,0,0);}
 assert(fixture_memory.translations==translated);
 text=Read(trace.output());assert(text.find("device_fields=unreadable")!=std::string::npos);
 std::cout<<"PASS: real CPU draw recorder/reader; one frame/64 attempts/1MiB; late selection; heap/protection/overflow bounds\n";
}
'''
    cpp = args.output / "draw-diagnostic.cpp"
    exe = args.output / "draw-diagnostic.exe"
    cpp.write_text(harness)
    subprocess.run([args.compiler, "-std=c++20", "-UNDEBUG", "-I" + str(root / "rexlego/src"),
                    str(cpp), "-o", str(exe)], check=True, timeout=45)
    cases = tempfile.mkdtemp(prefix="case-", dir=args.output.resolve())
    subprocess.run([str(exe.resolve()), cases], check=True, timeout=10)


if __name__ == "__main__":
    main()
