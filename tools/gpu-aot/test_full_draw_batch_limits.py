"""Exercise production draw capture boundaries at tunable batch limits."""
import argparse, subprocess
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('output',type=Path);a=p.parse_args();a.output.mkdir(parents=True,exist_ok=True)
root=Path(__file__).resolve().parents[2]
s=(root/'rexlego/src/gpu_native/draw.cpp').read_text()
def body(sig):
 start=s.index(sig);end=s.index('{',start)+1;depth=1
 while depth:depth+=(s[end]=='{')-(s[end]=='}');end+=1
 return s[start:end]
code=r"""
#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cstdlib>
#include <memory>
#include <vector>
using u32=unsigned;using u64=unsigned long long;using i32=int;
struct DrawBindings{std::array<u32,3> render_targets{};u32 depth_stencil=0;std::array<u32,16> textures{};int vertex_streams=0;};
struct D3DDevice{DrawBindings bindings;};
DrawBindings ReadDeviceBindings(D3DDevice* d){return d->bindings;}
DrawBindings SnapshotDrawBindings(){return {};}
struct FullDrawWork{std::shared_ptr<D3DDevice> device;DrawBindings bindings;u32 primitive,start,count;bool indexed;i32 base_vertex;u64 sequence;};
template<class T>struct DrawWorkPool{std::shared_ptr<T> Acquire(){return std::make_shared<T>();}};
std::vector<std::shared_ptr<FullDrawWork>> g_full_draw_batch;
std::vector<std::vector<std::shared_ptr<FullDrawWork>>> batches;
u64 g_full_draw_sequence=0;std::atomic<bool> g_draw_batch_pending=false;
void FlushFullDrawBatch(){if(g_full_draw_batch.empty())return;batches.push_back(std::move(g_full_draw_batch));g_full_draw_batch.clear();g_draw_batch_pending=false;}
"""+body('std::atomic<u32>& FullDrawBatchLimit()')+'\n'+body('void CaptureFullDraw(')+r"""
int main(){
 for(u32 limit:{1u,32u,96u,256u}){
  batches.clear();g_full_draw_sequence=0;FullDrawBatchLimit()=limit;
  for(u32 i=0;i<769;++i){auto d=std::make_shared<D3DDevice>();d->bindings.render_targets[0]=1;
   CaptureFullDraw(d,4,true,i,3,0);}
  FlushFullDrawBatch();u64 sequence=0;
  for(const auto& batch:batches){assert(batch.size()<=limit);for(const auto& work:batch){assert(work->sequence==++sequence);assert(work->start==sequence-1);}}
  assert(sequence==769);
 }
 batches.clear();FullDrawBatchLimit()=96;
 auto a=std::make_shared<D3DDevice>();a->bindings.render_targets[0]=1;
 auto b=std::make_shared<D3DDevice>();b->bindings.render_targets[0]=2;
 CaptureFullDraw(a,4,false,0,3,0);CaptureFullDraw(b,4,false,1,3,0);
 assert(batches.size()==1&&batches[0].size()==1);FlushFullDrawBatch();
 batches.clear();CaptureFullDraw(a,4,false,0,3,0);
 auto self=std::make_shared<D3DDevice>(*a);self->bindings.textures[2]=1;
 CaptureFullDraw(self,4,false,1,3,0);assert(batches.size()==2&&g_full_draw_batch.empty());
 batches.clear();for(int i=0;i<40;++i)CaptureFullDraw(a,4,false,i,3,0);
 FullDrawBatchLimit()=32;CaptureFullDraw(a,4,false,40,3,0);
 assert(batches.size()==1&&batches[0].size()==41&&g_full_draw_batch.empty());
}
"""
h=a.output/'batch-limits.cpp';h.write_text(code);exe=a.output/'batch-limits.exe'
subprocess.run(['clang++','-std=c++20','-O2',str(h),'-o',str(exe)],check=True,timeout=45)
subprocess.run([str(exe.resolve())],check=True,timeout=15)
print('PASS production capture: FIFO, 1/32/96/256 limits, target changes, self sampling and live limit reduction.')
