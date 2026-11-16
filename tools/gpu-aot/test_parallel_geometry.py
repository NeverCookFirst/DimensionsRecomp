"""Check the production parallel geometry planner against explicit draw boundaries."""
from pathlib import Path
import argparse
import subprocess

args = argparse.ArgumentParser()
args.add_argument("output", type=Path)
out = args.parse_args().output
out.mkdir(parents=True, exist_ok=True)
root = Path(__file__).resolve().parents[2]
draw = (root / "rexlego/src/gpu_native/draw.cpp").read_text()
body = draw[draw.index("bool PrepareDrawGeometry("):draw.index("void PrepareDrawWork(")]
source = r'''
#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <climits>
#include <cstring>
#include <future>
#include <memory>
#include <optional>
#include <vector>
#include "gpu_native/state.h"
#include "gpu_native/draw_preparation.h"
#include "gpu_native/vertex_layout_plan.h"
#include "gpu_native/buffer_window.h"
#include "gpu_native/vertex_fetch_range.h"
#include "gpu_native/index_draw.h"
#include "gpu_native/index_bounds.h"
#include "gpu_native/index_snapshot_cache.h"
#include "gpu_native/instanced_fetch.h"
#include "gpu_native/alpha_test.h"
#include "gpu_native/shader_input_usage.h"
#define REXCVAR_GET(x) true
#define REXLOG_WARN(...) ((void)0)
using namespace legodimensions::gpu_native;
using u8=uint8_t; using u32=uint32_t; using u64=uint64_t; using i32=int32_t;
namespace plume {
struct Element { const char* semanticName="POSITION"; u32 semanticIndex=0,slotIndex=0,alignedByteOffset=0,format=12; };
u32 RenderFormatSize(u32 n) { return n; }
}
struct Declaration {
  const plume::Element* elements=nullptr; u32 element_count=0;
  bool supported=true,has_r11g11b10_normal=false;
  u64 content_hash=1,reversed_byte_elements=0,half_swapped_elements=0,half_swapped_wide_elements=0;
};
enum class ShaderStage {kVertex,kPixel};
u64 BoundShaderHash(ShaderStage) {return 100;}
struct PipelineKey {u64 vertex_shader_hash=0,pixel_shader_hash=0;u32 spec_constants=0;};
struct Bytecode {const u8* data=nullptr;u32 size=0;};
Bytecode FindDxil(u64,u32) {return {};}
bool FloatBlendIndicesInput(std::span<const plume::Element>) {return false;}
bool FindFloatBlendIndicesDxil(u64,u32) {return false;}
bool FindPositionPrecisionDxil(u64,u32) {return false;}
bool NativeFsr3Enabled() {return false;}
struct Metadata {void* buffer=nullptr;u32 length=256,guest_format=1,mirror_address=64;};
using BufferResourceView=Metadata;
struct BufferPreparationSnapshot {Metadata metadata;bool cached=true;};
struct PreparedBufferVersion {Metadata view;};
PreparedBufferVersion PrepareCachedBuffer(const BufferPreparationSnapshot& input,u32 offset,u32 length,bool window,const VertexByteOrder& ={}) {
  if(!input.cached)return {};
  auto view=input.metadata; view.buffer=reinterpret_cast<void*>(uintptr_t(1));
  if(window){view.mirror_address+=offset;view.length=length;}
  return {view};
}
bool legodimensions::gpu_native::CpuMemoryUnchanged(const CpuMemoryStamp& s) {
  return !s.pages.empty() && s.pages[0].second==1;
}
struct ParallelDrawWork {
  DrawBindings bindings;
  Declaration declaration;
  bool indexed=true,geometry_ready=false,use_windows=false;
  u32 start=0,count=3,min_index=0;i32 base_vertex=0;
  std::shared_ptr<std::array<u8,13000>> device=std::make_shared<std::array<u8,13000>>();
  std::array<BufferPreparationSnapshot,16> vertex_inputs;
  BufferPreparationSnapshot index_input;
  IndexDrawWindow index_window;
  std::shared_ptr<const IndexBoundsSnapshot> index_bounds;
  VertexLayoutPlan layout;
  std::array<bool,16> active_streams{};
  std::array<VertexBufferWindow,16> windows{};
  std::array<PreparedBufferVersion,16> vertices;
  PreparedBufferVersion indices;
};
'''
source += body
source += r'''
int main(){
  IndexSnapshotCache cache;
  unsigned captures=0;
  auto capture=[&] {
    ++captures;
    auto result=std::make_shared<IndexBoundsSnapshot>();
    result->stamp.pages.emplace_back(0,1); result->bounds={2,4};
    return result;
  };
  auto first=cache.Get(0,6,true,capture);
  assert(cache.Get(0,6,true,capture)==first && captures==1);
  assert(cache.Get(2,6,true,capture)!=first && captures==2);
  auto changed=std::const_pointer_cast<IndexBoundsSnapshot>(first);
  changed->stamp.pages[0].second=2;
  assert(cache.Get(0,6,true,capture)!=first && captures==3);
  auto uncached=cache.Get(0,6,false,capture);
  assert(uncached!=cache.Get(0,6,false,capture) && captures==5);
  for(unsigned i=1;i<4100;++i) cache.Get(i*2,6,true,capture);
  assert(first->bounds.min==2 && first->bounds.max==4);
  unsigned unknowns=0;
  auto unknown=[&] {++unknowns;return std::make_shared<IndexBoundsSnapshot>();};
  cache.Get(0,8,true,unknown);cache.Get(0,8,true,unknown);
  assert(unknowns==2);
  plume::Element element;
  ParallelDrawWork prototype;
  prototype.declaration.elements=&element;prototype.declaration.element_count=1;
  prototype.bindings.vertex_streams[0]={123,0,16};
  prototype.index_window=DrawIndexWindow(0,3,2,256);
  auto snapshot=std::make_shared<IndexBoundsSnapshot>();
  snapshot->stamp.pages.emplace_back(0,1); snapshot->bounds={2,4};
  prototype.index_bounds=snapshot;
  auto work=prototype;
  assert(PrepareDrawGeometry(work) && work.geometry_ready && work.use_windows);
  assert(work.min_index==2 && work.windows[0].offset==32 && work.windows[0].length==48);
  work=prototype;work.base_vertex=-2;
  assert(PrepareDrawGeometry(work) && work.windows[0].offset==0 && work.windows[0].length==48);
  work=prototype;work.base_vertex=-3;
  assert(!PrepareDrawGeometry(work) && work.geometry_ready);
  work=prototype;work.vertex_inputs[0].metadata.length=64;
  assert(!PrepareDrawGeometry(work) && work.geometry_ready);
  work=prototype;work.vertex_inputs[0].cached=false;
  assert(!PrepareDrawGeometry(work) && !work.geometry_ready);
  work=prototype;work.index_input.cached=false;
  assert(!PrepareDrawGeometry(work) && !work.geometry_ready);
  work=prototype;
  auto dirty=std::make_shared<IndexBoundsSnapshot>(*snapshot);
  dirty->stamp.pages[0].second=2; work.index_bounds=dirty;
  assert(!PrepareDrawGeometry(work) && !work.geometry_ready);
  work=prototype;work.indexed=false;work.start=UINT32_MAX;work.count=2;
  assert(!PrepareDrawGeometry(work) && work.geometry_ready);
  work=prototype;work.bindings.vertex_streams[0].buffer=0;
  assert(PrepareDrawGeometry(work) && !work.vertices[0].view.buffer);
  std::vector<std::future<void>> jobs;
  for(unsigned n=0;n<3;++n)jobs.push_back(std::async(std::launch::async,[prototype] {
    for(unsigned i=0;i<1000;++i) {
      auto draw=prototype;
      assert(PrepareDrawGeometry(draw) && draw.windows[0].offset==32 && draw.min_index==2);
    }
  }));
  for(auto& job:jobs)job.get();
}
'''
cpp = out / "parallel-geometry.cpp"
cpp.write_text(source)
exe = out / "parallel-geometry.exe"
subprocess.run(["clang++", "-std=c++23", "-O2", "-UNDEBUG", "-D_CRT_SECURE_NO_WARNINGS",
                "-I" + str(root / "rexlego/src"),
                "-I" + str(root.parent / "dev-assets/sdk-install/include"),
                str(cpp), "-o", str(exe)], check=True)
subprocess.run([str(exe.resolve())], check=True, timeout=30)
print("PASS production parallel geometry: rebasing, bounds, overflow, dirty snapshots, cache misses, null streams and 3000 concurrent draws")
