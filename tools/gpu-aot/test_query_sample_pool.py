"""Exercise production occlusion scope pooling with tracked driver allocations."""
import argparse
from pathlib import Path
import subprocess
p=argparse.ArgumentParser();p.add_argument('output',type=Path);p.add_argument('--compiler',default='clang++');a=p.parse_args()
root=Path(__file__).resolve().parents[2];a.output.mkdir(parents=True,exist_ok=True)
source=(root/'rexlego/src/gpu_native/queries.cpp').read_text()
start=source.index('struct QueryDrawScope::Impl {');end=source.index('void FailActiveQueries()',start)
production=source[start:end]
cpp=a.output/'query-pool.cpp'
cpp.write_text(r'''
#include <memory>
#include <vector>
#include <mutex>
#include <algorithm>
#include <unordered_map>
#include <cassert>
#include <cstdint>
using u32=uint32_t;using u64=uint64_t;
template<class T>struct ComPtr{std::shared_ptr<T> value;T* Get(){return value.get();}ComPtr* operator&(){return this;}};
struct Heap{};struct Resource{};
struct Samples{ComPtr<Heap> heap;ComPtr<Resource> readback;};
struct Query{bool active=false,failed=false;std::vector<std::shared_ptr<Samples>> draws;};
std::mutex mutex;std::unordered_map<u32,std::shared_ptr<Query>> queries;
std::vector<std::shared_ptr<Samples>> sample_pool;constexpr size_t kMaxPooledSamples=256;
constexpr int D3D12_QUERY_HEAP_TYPE_OCCLUSION=1,D3D12_QUERY_TYPE_OCCLUSION=1,D3D12_HEAP_TYPE_READBACK=1;
constexpr int D3D12_RESOURCE_DIMENSION_BUFFER=1,D3D12_TEXTURE_LAYOUT_ROW_MAJOR=1,D3D12_HEAP_FLAG_NONE=0,D3D12_RESOURCE_STATE_COPY_DEST=1;
struct D3D12_QUERY_HEAP_DESC{int Type=0;unsigned Count=0;};
struct D3D12_HEAP_PROPERTIES{int Type=0,CreationNodeMask=0,VisibleNodeMask=0;};
struct D3D12_RESOURCE_DESC{int Dimension=0,Layout=0;u64 Width=0;unsigned Height=0,DepthOrArraySize=0,MipLevels=0;struct {int Count=0;} SampleDesc;};
#define IID_PPV_ARGS(p) (p)
#define FAILED(h) ((h)!=0)
struct Device{
 unsigned heaps=0,buffers=0;bool fail=false;
 int CreateQueryHeap(D3D12_QUERY_HEAP_DESC*,ComPtr<Heap>* p){if(fail)return 1;++heaps;p->value=std::make_shared<Heap>();return 0;}
 int CreateCommittedResource(D3D12_HEAP_PROPERTIES*,int,D3D12_RESOURCE_DESC*,int,void*,ComPtr<Resource>* p){if(fail)return 1;++buffers;p->value=std::make_shared<Resource>();return 0;}
} device;
struct Commands{unsigned begins=0,ends=0,resolves=0;void BeginQuery(Heap*,int,int){++begins;}void EndQuery(Heap*,int,int){++ends;}void ResolveQueryData(Heap*,int,int,int,Resource*,int){++resolves;}} commands;
namespace plume{struct RenderCommandList{};struct D3D12CommandList:RenderCommandList{Commands* d3d;};struct D3D12Device{Device* d3d;};}
plume::D3D12Device host{&device};struct HostDevice{static auto Device(){return &host;}};
class QueryDrawScope{public:explicit QueryDrawScope(plume::RenderCommandList*);~QueryDrawScope();struct Impl;std::unique_ptr<Impl> impl_;};
'''+production+r'''
int main(){
 plume::D3D12CommandList list;list.d3d=&commands;
 {QueryDrawScope scope(&list);assert(!scope.impl_&&device.heaps==0);}
 auto q=std::make_shared<Query>();q->active=true;queries[1]=q;
 {QueryDrawScope scope(&list);}assert(device.heaps==1&&q->draws.size()==1);
 auto first=q->draws[0];q->draws.clear();
 {QueryDrawScope scope(&list);}assert(device.heaps==2&&q->draws[0]!=first);
 // An older generation held by an in-flight submission forbids recycling.
 auto retired=std::make_shared<Query>();retired->draws.push_back(first);first.reset();
 auto second=q->draws[0];q->draws.clear();second.reset();
 for(int frame=0;frame<1000;++frame){
   {QueryDrawScope scope(&list);}assert(q->draws[0]!=retired->draws[0]);q->draws.clear();
 }
 assert(device.heaps==2&&device.buffers==2);
 retired.reset();sample_pool.clear();
 device.fail=true;{QueryDrawScope scope(&list);assert(!scope.impl_);}assert(q->failed);
 device.fail=false;q->failed=false;
 for(int i=0;i<300;++i){QueryDrawScope scope(&list);}
 assert(sample_pool.size()==256&&q->draws.size()==300);
 // Every actual queried draw still begins, ends and resolves one real query.
 assert(commands.begins==commands.ends&&commands.ends==commands.resolves);
 q->draws.clear();sample_pool.clear();queries.clear();
}
''')
exe=a.output/'query-pool.exe'
subprocess.run([a.compiler,'-std=c++20','-UNDEBUG','-O2',str(cpp),'-o',str(exe)],check=True,timeout=45)
subprocess.run([str(exe.resolve())],check=True,timeout=10)
print('PASS: production occlusion sample pool reuse, in-flight ownership, bound, failures, and per-draw query commands')
